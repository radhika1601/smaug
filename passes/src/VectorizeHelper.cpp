#include "VectorizeHelper.h"
#include "map"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/IRBuilder.h"
#define DEBUG_TYPE "vec-help"
#include "MPCVecUtils.h"

BasicBlock *VectorizeHelperPass::createLoop(
    IRBuilder<> &Builder, BasicBlock *insertBefore, BasicBlock *insertAfter,
    BasicBlock *exit, SmallVector<Instruction *> &instrs,
    SmallDenseMap<Instruction *, Instruction *> &liftedMap, PHINode *induction,
    Instruction *indNextInst, Instruction *BackEdgeCond, bool ifTrueContinue, bool indCanBeIndex) {
  BasicBlock *header = BasicBlock::Create(
      insertBefore->getContext(), "", insertBefore->getParent(), insertBefore);
  auto *insertAfterTerm = insertAfter->getTerminator();
  insertAfterTerm->replaceSuccessorWith(insertBefore, header);
  PHINode *counter =
      PHINode::Create(induction->getType(), 2, "newCounter", header);
  counter->addIncoming(induction->getIncomingValueForBlock(insertAfter),
                       insertAfter);
  Instruction *incrementedCounter = nullptr, *cond = nullptr;
  std::map<Instruction *, Instruction *> instrsMap;
  instrsMap.insert({induction, counter});
  Instruction *insertInstAfter = counter;
  std::set<Instruction *> phisToUpdate;
  PHINode *loadStoreIndex = nullptr;
  if(indCanBeIndex)
    loadStoreIndex = counter;
  for (Instruction *I : instrs) {
    bool updateLater = false;
    if (PHINode *phi = dyn_cast<PHINode>(I)) {
      Value *checkVal = phi->getIncomingValueForBlock(insertBefore);
      if (Instruction *check = dyn_cast<Instruction>(checkVal)) {
        if (check->getParent() == insertBefore)
          if (liftedMap.find(check) == liftedMap.end()) {
            updateLater = true;
          }
      }
    }
    Instruction *newInst = I->clone();
    newInst->copyMetadata(*I);
    newInst->insertAfter(insertInstAfter);

    insertInstAfter = newInst;
    instrsMap.insert({I, newInst});
    if (I == indNextInst)
      incrementedCounter = newInst;
    else if (I == BackEdgeCond)
      cond = newInst;
    // Create Store
    if (liftedMap.find(I) != liftedMap.end()) {
      if (loadStoreIndex == nullptr) {
        loadStoreIndex = PHINode::Create(Builder.getInt64Ty(), 2, "");
        loadStoreIndex->insertAfter(counter);
        loadStoreIndex->addIncoming(Builder.getInt64(0), insertAfter);
        Builder.SetInsertPoint(header->getFirstNonPHI());
        auto tmp = Builder.CreateAdd(loadStoreIndex, Builder.getInt64(1));
        loadStoreIndex->addIncoming(tmp, header);
      }
      auto type = I->getType() == Builder.getInt1Ty() ? Builder.getInt8Ty()
                                                      : I->getType();
      Builder.SetInsertPoint(header,
                             ++llvm::BasicBlock::iterator(insertInstAfter));
      Value *ptr = Builder.CreateGEP(type, liftedMap.at(I), loadStoreIndex, "");
      Value *toStore = newInst;
      if (I->getType() == Builder.getInt1Ty())
        toStore = Builder.CreateZExt(newInst, Builder.getInt8Ty());
      insertInstAfter = Builder.CreateStore(toStore, ptr);
      insertInstAfter->copyMetadata(*I);
    }
    if (I->isUsedInBasicBlock(exit)) {
      std::set<Instruction *> updateUsers;
      for (auto *user : I->users()) {
        if (Instruction *UserInst = dyn_cast<Instruction>(user)) {
          if (UserInst->getParent() == exit)
            updateUsers.insert(UserInst);
        }
      }
      for (auto *user : updateUsers) {
        replaceOperand(user, I, newInst);
      }
    }

    if (updateLater) {
      phisToUpdate.insert(newInst);
      continue;
    }
    updateNewInstOps(newInst, instrsMap, liftedMap, Builder, loadStoreIndex,
                     counter, incrementedCounter, header, insertAfter);
  }
  updateNewInstOps(counter, instrsMap, liftedMap, Builder, loadStoreIndex,
                   counter, incrementedCounter, header, insertAfter);
  counter->addIncoming(incrementedCounter, header);
  llvm::BranchInst *term;
  if (ifTrueContinue)
    term = BranchInst::Create(header, insertBefore, cond, header);
  else
    term = BranchInst::Create(insertBefore, header, cond, header);

  MDNode *loopMetadata =
      MDNode::get(insertAfter->getContext(),
                  MDString::get(insertAfter->getContext(), "loop"));
  term->setMetadata("llvm.loop", loopMetadata);

  for (auto *I : phisToUpdate) {
    PHINode *phi = dyn_cast<PHINode>(I);
    auto incomingVal = phi->getIncomingValueForBlock(insertBefore);
    Instruction *check = dyn_cast<Instruction>(incomingVal);
    if (check && (instrsMap.find(check) != instrsMap.end()))
      phi->setIncomingValueForBlock(insertBefore, instrsMap.at(check));
    else {
      phi->setIncomingValueForBlock(insertBefore, incomingVal);
    }
  }
  header->replacePhiUsesWith(insertBefore, header);
  insertBefore->replacePhiUsesWith(insertAfter, header);

  return header;
}

bool VectorizeHelperPass::processLoop(Loop *L) {
  assert(L->isInnermost() && "Only process inner loops.");

  MPCVecUtils utils = MPCVecUtils();
  if (!utils.hasSecretSharedInsts(L->getBlocksVector()))
    return false;

  smaug::LoopVectorizeHints Hints(L, true, *ORE, TTI);

  // Function containing loop
  Function *F = L->getHeader()->getParent();

  PredicatedScalarEvolution PSE(*SE, *L);

  // Check if it is legal to vectorize the loop.
  smaug::LoopVectorizationRequirements Requirements;
  smaug::LoopVectorizationLegality LVL(L, PSE, DT, TTI, TLI, F, *LAIs, LI, ORE,
                                &Requirements, &Hints, DB, AC, BFI, PSI);

  // can vectorize and does not have reduction vars => nothing to do
  bool canVectorize = LVL.canVectorize(false);
  if (canVectorize && LVL.getReductionVars().empty())
    return false;
  if (!canVectorize && LVL.MemoryNotVectorizable)
    return false;

  // if (LVL.canVectorize(false)) {
  //   errs() << "can vectorize\n";
  // } else {
  //   errs() << "cannot vectorize\n";
  // }

  // Having a single exit block implies there's also one exiting block.
  if (!L->getExitBlock()) {
    LLVM_DEBUG(dbgs() << "!L->getExitBlock()");
    return false;
  }
  if (!L->isLoopSimplifyForm()) {
    LLVM_DEBUG(dbgs() << "!L->isLoopSimplifyForm()");
    return false;
  }
  if (!L->isRotatedForm()) {
    LLVM_DEBUG(dbgs() << "!L->isRotatedForm()");
    return false;
  }

  SmallDenseSet<Instruction *> toBeReplaced;
  // errs() << "to be replaced\n";
  for (auto &I : *(L->getHeader())) {
    if (willBeReplaced(&I)) {
      toBeReplaced.insert(&I);
      // errs() << "\t" << I << "\n";
    }
  }
  if (toBeReplaced.empty()) {
    LLVM_DEBUG(dbgs() << "no instructions to be replaced with MPC functions\n");
    return false;
  }
  LLVM_DEBUG(dbgs() << "here\n");

  const LoopAccessInfo *LAI = &(LAIs->getInfo(*L));
  auto *Dependences = LAI->getDepChecker().getDependences();
  // Can't work when there are memory dependence cycles
  if (!Dependences->empty()) {
    LLVM_DEBUG(dbgs() << "Can't work when there are memory dependence cycles");
    return false;
  }

  Value *tripCount = utils.getTripCount(L, SE);
  if (tripCount == nullptr) {
    return false;
  }
  SmallVector<Instruction *> isolate;
  if (L->getBlocksVector().size() != 1) {
    LLVM_DEBUG(dbgs() << "BB > 1\n");
    return false;
  }
  SmallDenseSet<Instruction *> deps; // Instructions that cannot be vectorized
  SmallVector<const Instruction *> finalInsts;
  SmallVector<Instruction *> lift;

  auto *induction = L->getInductionVariable(*SE);
  if (!induction) {
    LLVM_DEBUG(dbgs() << "induction not found\n");
    return false;
  }
  auto *BackEdge = utils.getLoopBackEdge(L, LI);
  auto *BackEdgeCond = L->getLatchCmpInst();
  auto *BackEdgeInst = dyn_cast<Instruction>(BackEdge);
  auto *indNext = induction->getIncomingValueForBlock(L->getLoopLatch());
  auto *indNextInst = dyn_cast<Instruction>(indNext);

  this->revertAndIcmpToTrunc(L->getHeader());

  SmallVector<Instruction *> allInsts;
  SmallDenseSet<Instruction *> usedOutside;
  for (auto &I : *(L->getHeader())) {
    if (&I == induction)
      continue;
    if (&I == BackEdgeInst)
      continue;
    if (&I == BackEdgeCond)
      continue;
    if (&I == indNext)
      continue;
    allInsts.push_back(&I);
  }
  SmallVector<SmallDenseSet<Instruction *>> partsSets;
  SmallVector<SmallVector<Instruction *>> parts;
  getParts(partsSets, toBeReplaced, allInsts, induction, L->getHeader(), LVL);
  if (partsSets.size() <= 1) {
    return false;
  }

  SmallDenseSet<Instruction *> liftSet;
  SmallDenseSet<Instruction *> operandDeps, indNextDeps, BackEdgeCondDeps;
  SmallDenseSet<Instruction *> emptyVec;
  dependsOn(BackEdgeCond, emptyVec, BackEdgeCondDeps, liftSet, induction);
  dependsOn(indNextInst, emptyVec, indNextDeps, liftSet, induction);
  
  for (size_t i = 0; i < partsSets.size(); ++i) {
    if (i != 0) {
      for (Instruction *I : partsSets[i]) {
        dependsOn(I, partsSets[i], operandDeps, liftSet, induction);
      }
    }
    parts.push_back({});
    for (auto &I : *(L->getHeader())) {
      if (operandDeps.contains(&I) || partsSets[i].contains(&I) ||
          (&I == indNextInst) || (&I == BackEdgeCond) ||
          (indNextDeps.contains(&I)) || BackEdgeCondDeps.contains(&I)) {
        parts[i].push_back(&I);
      }
    }
    partsSets[i].clear();
    operandDeps.clear();
  }

  partsSets.clear();
  // for (size_t i = 0; i < parts.size(); ++i) {
  //   errs() << "part " << i << "\n";
  //   for (auto *I : parts[i]) {
  //     errs() << "\t" << *I << "\n";
  //   }
  // }

  llvm::IRBuilder<> Builder(F->getContext());
  SmallDenseMap<Instruction *, Instruction *> liftedMap;
  BasicBlock *PH = L->getLoopPreheader();
  BasicBlock *bb1 = L->getHeader();
  BasicBlock *exit = L->getExitBlock();

  BasicBlock *liftedBB = BasicBlock::Create(F->getContext(), "lifted", F, bb1);
  BranchInst::Create(bb1, liftedBB);
  auto PHterm = PH->getTerminator();
  PHterm->replaceSuccessorWith(bb1, liftedBB);
  bb1->replacePhiUsesWith(PH, liftedBB);

  for (Instruction *I : liftSet) {
    Builder.SetInsertPoint(liftedBB->getFirstInsertionPt());
    // if(type->)
    bool isi1 = I->getType() == IntegerType::getInt1Ty(F->getContext());
    auto type = isi1 ? IntegerType::getInt8Ty(F->getContext()) : I->getType();
    auto mallocVal = utils.createMalloc(Builder, F, type, tripCount,
                                        I->hasMetadata("secret_shared"), "");
    Instruction *newInst = dyn_cast<Instruction>(mallocVal);
    if (I->hasMetadata("secret_shared")) {
      auto node = I->getMetadata("secret_shared");
      newInst->setMetadata("secret_shared", node);
    }
    liftedMap.insert(std::make_pair(I, newInst));
  }

  bool ifTrueContinue = true;
  if (BackEdge->getSuccessor(0) != bb1)
    ifTrueContinue = false;

  bool indCanBeIndex = false;
  if(Constant *c = dyn_cast<Constant>(induction->getIncomingValueForBlock(liftedBB))){
    if(c->isZeroValue() && indNextInst->getOpcode() == Instruction::Add){
      auto v = indNextInst->getOperand(0);
      if(v == induction){
        v = indNextInst->getOperand(1);
      }
      if(Constant *c1 = dyn_cast<Constant>(v)){
        if(c1->isOneValue()){
          indCanBeIndex = true;
        }
      }
    }
  }

  // Create new loop
  BasicBlock *insertBefore = bb1;
  BasicBlock *insertAfter = liftedBB;
  MDNode *mdnode = nullptr;
  if (bb1->getTerminator()->hasMetadata("llvm.mpc.loop.flattened"))
    mdnode = MDNode::get(
        bb1->getContext(),
        MDString::get(bb1->getContext(), "llvm.mpc.loop.flattened"));
  for (size_t i = 0; i < parts.size(); ++i) {
    auto newBB = createLoop(Builder, insertBefore, insertAfter, exit, parts[i],
                            liftedMap, induction, indNextInst, BackEdgeCond,
                            ifTrueContinue, indCanBeIndex);
    if (mdnode) {
      newBB->getTerminator()->setMetadata("llvm.mpc.loop.flattened", mdnode);
    }
    insertAfter = newBB;
  }

  auto *term = insertAfter->getTerminator();
  term->replaceSuccessorWith(bb1, exit);
  exit->replacePhiUsesWith(bb1, insertAfter);
  bb1->eraseFromParent();

  Builder.SetInsertPoint(exit->getFirstNonPHI());
  for (auto &p : liftedMap) {
    utils.CreateFree(Builder, F, p.second);
  }

  return true;
}

bool VectorizeHelperPass::runImpl(
    Function &F, ScalarEvolution &SE_, LoopInfo &LI_, TargetTransformInfo &TTI_,
    DominatorTree &DT_, BlockFrequencyInfo *BFI_, TargetLibraryInfo *TLI_,
    DemandedBits &DB_, AssumptionCache &AC_, LoopAccessInfoManager &LAIs_,
    OptimizationRemarkEmitter &ORE_, ProfileSummaryInfo *PSI_) {

  SE = &SE_;
  LI = &LI_;
  TTI = &TTI_;
  DT = &DT_;
  BFI = BFI_;
  TLI = TLI_;
  AC = &AC_;
  LAIs = &LAIs_;
  DB = &DB_;
  ORE = &ORE_;
  PSI = PSI_;
  // Build up a worklist of inner-loops to vectorize. This is necessary as the
  // act of distributing a loop creates new loops and can invalidate iterators
  // across the loops.
  SmallVector<Loop *, 8> Worklist;

  for (Loop *TopLevelLoop : *LI)
    for (Loop *L : depth_first(TopLevelLoop))
      // We only handle inner-most loops.
      if (L->isInnermost()) {

        Worklist.push_back(L);
      }
  // Now walk the identified inner loops.
  bool Changed = false;
  for (Loop *L : Worklist) {
    LLVM_DEBUG(dbgs() << "process loop\n");
    Changed |= formLCSSARecursively(*L, *DT, LI, SE);
    Changed |= processLoop(L);
    LLVM_DEBUG(dbgs() << "processed " << Changed << "\n");
  }

  // Process each loop nest in the function.
  return Changed;
}

PreservedAnalyses VectorizeHelperPass::run(llvm::Function &F,
                                           llvm::FunctionAnalysisManager &FAM) {
  bool hasSecretShared = false;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (I.hasMetadata("secret_shared")) {
        hasSecretShared = true;
      }
    }
  }
  if (!hasSecretShared)
    return PreservedAnalyses::all();

  auto &LI = FAM.getResult<LoopAnalysis>(F);

  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);
  auto &DB = FAM.getResult<DemandedBitsAnalysis>(F);
  auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  LoopAccessInfoManager &LAIs = FAM.getResult<LoopAccessAnalysis>(F);
  auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  ProfileSummaryInfo *PSI =
      MAMProxy.getCachedResult<ProfileSummaryAnalysis>(*F.getParent());
  BlockFrequencyInfo *BFI = nullptr;
  if (PSI && PSI->hasProfileSummary())
    BFI = &FAM.getResult<BlockFrequencyAnalysis>(F);

  bool Changed = false;
  Changed |= runImpl(F, SE, LI, TTI, DT, BFI, &TLI, DB, AC, LAIs, ORE, PSI);
  LLVM_DEBUG(dbgs() << Changed << "\n");
  if (!Changed)
    return PreservedAnalyses::all();
  //   PreservedAnalyses PA;
  //   PA.preserve<LoopAnalysis>();
  //   PA.preserve<DominatorTreeAnalysis>();
  return PreservedAnalyses::none();
}
