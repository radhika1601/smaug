#include "ParallelizeReductions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#define DEBUG_TYPE "parallelize-reductions"
#include "MPCVecUtils.h"

void ParallelizeReductionsPass::getReductionInsts(
    Loop *L, SmallVector<Instruction *> &instrs, SmallVector<Value *> &arrays,
    smaug::LoopVectorizationLegality::ReductionList &reductionVars) {
  for (auto var : reductionVars) {
    auto val = var.first->getIncomingValueForBlock(L->getBlocks()[0]);
    Instruction *i = dyn_cast<Instruction>(val);
    // errs() << *i << "\n";
    // i->getOperandList();
    instrs.push_back(i);
    for (auto itr = i->operands().begin(); itr != i->operands().end(); ++itr) {
      if (*itr != var.first) {
        Instruction *load = dyn_cast<Instruction>(*itr);
        // fix for when there can be multiple getelementptr instructions
        if (load && isa<LoadInst>(load)) {
          // errs() << "load\n";
          // load->dump();
          auto ptr = load->getOperand(0);
          // ptr->dump();
          Instruction *tmp = dyn_cast<Instruction>(ptr);
          if (tmp && isa<GetElementPtrInst>(tmp)) {
            // errs() << "GetElementPtrInst\n";
            // tmp->dump();
            // tmp->getOperand(0)->dump();
            arrays.push_back(tmp->getOperand(0));
          }
        }
      }
    }
  }

  // errs() << "Arrays\n";
  #ifndef NDEBUG
  for (auto x : arrays) {
    x->dump();
  }
  #endif
}

bool ParallelizeReductionsPass::processLoop(Loop *L) {
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

  if (!LVL.canVectorize(false)) {
    // errs() << "Could not prove legality" << *L << "\n";
    Hints.emitRemarkWithHints();
    return false;
  }

  auto reductionVars = LVL.getReductionVars();
  if (reductionVars.empty()) {
    // errs() << "no reduction variables " << *L << "\n";
    return false;
  }

  Value *tc = utils.getTripCount(L, SE);
  if (!tc) {
    // errs() << "trip count not found " << *L << "\n";
    return false;
  }
  // tc->dump();

  SmallDenseSet<const Instruction *> finalInsts;
  utils.getlatchInsts(L, finalInsts, SE, LI);
  auto *induction = L->getInductionVariable(*SE);

  /*

  for(i : 0, n-1){
    sum += x[i]
  }

  prev_red = x
  j = 1
  l_floor = N;
  while(l_floor != 1){
    l = (l_floor) % 2
    l_floor = l_floor >> 1
    l = l + l_floor
    red : [l]
    for(i: l_floor){
      red = prev_red[i] + prev_red[i+l_floor]
    }
    if(l != l_floor){
      red[l_floor] = prev_red [l_floor * 2]
    }
    if(j != 1){
      deallocate(prev_red)
    }
    j += 1
    prev_red = red
    l_floor = l
  }

  sum = red[0]

  */

  SmallVector<Instruction *> instrs;
  SmallVector<Value *> arrays;
  getReductionInsts(L, instrs, arrays, reductionVars);
  // errs() << "arrays " << arrays.size() << "\n";
  auto PH = L->getLoopPreheader();
  auto Exit = L->getExitBlock();
  // errs() << "exit block " << Exit->getName() << "\n";
  llvm::IRBuilder<> Builder(F->getContext());
  BasicBlock *newPH = BasicBlock::Create(F->getContext(), "newPH", F, Exit);

  // auto PhtoNewPH = BranchInst::Create(newPH);
  // ReplaceInstWithInst(PH->getTerminator(), PhtoNewPH);

  Builder.SetInsertPoint(newPH);
  llvm::Value *one = llvm::ConstantInt::get(tc->getType(), 1);
  auto i = Builder.CreateICmpULE(tc, one);
  BasicBlock *newLoopStart =
      BasicBlock::Create(F->getContext(), "newLoopStart", F, PH);
  Builder.CreateCondBr(i, PH, newLoopStart);
  Builder.SetInsertPoint(newLoopStart);

  // assuming only one reduction variable
  auto l_floor = PHINode::Create(tc->getType(), 2, "", newLoopStart);
  l_floor->addIncoming(tc, newPH);
  // errs() << "l_floor\n";
  auto redVarOrig = reductionVars.front().first;

  auto prev_red = PHINode::Create(arrays[0]->getType(), 2, "", newLoopStart);
  prev_red->addIncoming(arrays[0], newPH);
  // errs() << "prev_red\n";

  auto j = PHINode::Create(tc->getType(), 2, "", newLoopStart);
  j->addIncoming(one, newPH);
  // errs() << "j\n";

  Builder.SetInsertPoint(newLoopStart);
  // errs() << "insertpoint\n";
  auto mod_lfloor = Builder.CreateAnd(l_floor, 1, "");
  auto updated_lfloor = Builder.CreateShl(l_floor, 1, "");
  auto l = Builder.CreateAdd(mod_lfloor, updated_lfloor, "");
  auto red = Builder.CreateAlloca(prev_red->getType(), l, "");
  llvm::Value *zero = llvm::ConstantInt::get(updated_lfloor->getType(), 0);
  auto x = Builder.CreateICmpSGT(updated_lfloor, zero);
  // create branch instr

  // add the loop for updated_lfloor
  auto innerLoop = BasicBlock::Create(F->getContext(), "innerLoop", F, PH);
  auto indInnerLoop =
      PHINode::Create(updated_lfloor->getType(), 2, "", innerLoop);
  indInnerLoop->addIncoming(one, newLoopStart);
  Builder.SetInsertPoint(innerLoop);
  auto loadPtr =
      Builder.CreateGEP(redVarOrig->getType(), prev_red, indInnerLoop, "");
  auto v1 = Builder.CreateLoad(redVarOrig->getType(), loadPtr, "");
  auto indPlusLfloor = Builder.CreateAdd(indInnerLoop, updated_lfloor);
  loadPtr =
      Builder.CreateGEP(redVarOrig->getType(), prev_red, indPlusLfloor, "");
  auto v2 = Builder.CreateLoad(redVarOrig->getType(), loadPtr, "");

  auto redInst = instrs[0]->clone();
  redInst->setOperand(0, v1);
  redInst->setOperand(1, v2);
  redInst->insertAfter(v2);

  auto resPtr = Builder.CreateGEP(redVarOrig->getType(), red, indInnerLoop, "");
  Builder.CreateStore(redInst, resPtr);
  auto indNextInnerLoop = Builder.CreateAdd(indInnerLoop, one);
  indInnerLoop->addIncoming(indNextInnerLoop, innerLoop);
  auto innerLoopCond = Builder.CreateICmpEQ(indNextInnerLoop, updated_lfloor);

  BasicBlock *innerLoopExit =
      BasicBlock::Create(F->getContext(), "innerLoopExit", F, PH);
  Builder.CreateCondBr(innerLoopCond, innerLoopExit, innerLoop);

  BranchInst::Create(innerLoop, innerLoopExit, x, newLoopStart);

  Builder.SetInsertPoint(innerLoopExit);
  auto lVslFloor = Builder.CreateICmpEQ(l, updated_lfloor, "");

  BasicBlock *setOddVal =
      BasicBlock::Create(F->getContext(), "setOddVal", F, PH);
  BasicBlock *deallocateCondBB =
      BasicBlock::Create(F->getContext(), "deallocateCondBB", F, PH);
  Builder.CreateCondBr(lVslFloor, deallocateCondBB, setOddVal);

  Builder.SetInsertPoint(setOddVal);

  // TODO: confirm correctness of ashr or lshr
  auto ind2 = Builder.CreateAShr(updated_lfloor, 1, "");
  loadPtr = Builder.CreateGEP(redVarOrig->getType(), prev_red, ind2);
  v1 = Builder.CreateLoad(redVarOrig->getType(), loadPtr);
  resPtr = Builder.CreateGEP(redVarOrig->getType(), red, l_floor);
  Builder.CreateStore(v1, resPtr);
  Builder.CreateBr(deallocateCondBB);

  // deallocate if needed
  Builder.SetInsertPoint(deallocateCondBB);
  auto deallocaCond = Builder.CreateICmpNE(j, one, "");

  BasicBlock *deallocateBB =
      BasicBlock::Create(F->getContext(), "deallocateBB", F, PH);
  BasicBlock *newLoopEnd =
      BasicBlock::Create(F->getContext(), "newLoopEnd", F, PH);
  Builder.CreateCondBr(deallocaCond, deallocateBB, newLoopEnd);

  Builder.SetInsertPoint(deallocateBB);
  Builder.CreateFree(prev_red);
  Builder.CreateBr(newLoopEnd);

  Builder.SetInsertPoint(newLoopEnd);

  auto jPlusOne = Builder.CreateAdd(j, one);
  j->addIncoming(jPlusOne, newLoopEnd);
  prev_red->addIncoming(red, newLoopEnd);
  l_floor->addIncoming(l, newLoopEnd);

  auto loopCond = Builder.CreateICmpNE(l, one, "");
  Builder.CreateCondBr(loopCond, newLoopStart, PH);

  // newPH->dump();
  // newLoopStart->dump();
  // innerLoop->dump();
  // innerLoopExit->dump();
  // setOddVal->dump();
  // deallocateCondBB->dump();
  // deallocateBB->dump();
  // newLoopEnd->dump();

  return true;
}

bool ParallelizeReductionsPass::runImpl(
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

  bool Changed = false;

  // The vectorizer requires loops to be in simplified form.
  // Since simplification may add new inner loops, it has to run before the
  // legality and profitability checks. This means running the loop vectorizer
  // will simplify all loops, regardless of whether anything end up being
  // vectorized.
  for (const auto &L : *LI)
    Changed |=
        simplifyLoop(L, DT, LI, SE, AC, nullptr, false /* PreserveLCSSA */);

  // Build up a worklist of inner-loops to vectorize. This is necessary as the
  // act of distributing a loop creates new loops and can invalidate iterators
  // across the loops.
  SmallVector<Loop *, 8> Worklist;

  for (Loop *TopLevelLoop : *LI)
    for (Loop *L : depth_first(TopLevelLoop))
      // We only handle inner-most loops.
      if (L->isInnermost())
        Worklist.push_back(L);

  // Now walk the identified inner loops.
  for (Loop *L : Worklist) {
    Changed |= formLCSSARecursively(*L, *DT, LI, SE);
    // errs() << "process loop\n";
    Changed |= processLoop(L);
    // errs() << "processed " << Changed << "\n";
  }

  // Process each loop nest in the function.
  return Changed;
}

PreservedAnalyses
ParallelizeReductionsPass::run(llvm::Function &F,
                               llvm::FunctionAnalysisManager &AM) {
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

  // errs() << "run\n";
  auto &LI = AM.getResult<LoopAnalysis>(F);
  // There are no loops in the function. Return before computing other expensive
  // analyses.
  if (LI.empty())
    return PreservedAnalyses::all();
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TTI = AM.getResult<TargetIRAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  auto &DB = AM.getResult<DemandedBitsAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  LoopAccessInfoManager &LAIs = AM.getResult<LoopAccessAnalysis>(F);
  auto &MAMProxy = AM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  ProfileSummaryInfo *PSI =
      MAMProxy.getCachedResult<ProfileSummaryAnalysis>(*F.getParent());
  BlockFrequencyInfo *BFI = nullptr;
  if (PSI && PSI->hasProfileSummary())
    BFI = &AM.getResult<BlockFrequencyAnalysis>(F);

  //   LoopAccessInfoManager &LAIs = AM.getResult<LoopAccessAnalysis>(F);
  bool Changed = runImpl(F, SE, LI, TTI, DT, BFI, &TLI, DB, AC, LAIs, ORE, PSI);
  // errs() << Changed << "\n";
  if (!Changed)
    return PreservedAnalyses::all();
  //   PreservedAnalyses PA;
  //   PA.preserve<LoopAnalysis>();
  //   PA.preserve<DominatorTreeAnalysis>();
  return PreservedAnalyses::none();
}
