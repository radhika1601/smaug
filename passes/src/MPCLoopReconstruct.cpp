#include "MPCLoopReconstruct.h"
#include "llvm/Support/FormatVariadic.h"

#include <map>

#define DEBUG_TYPE "mpc-loop-reconstruct"
#include "MPCVecUtils.h"

bool MPCLoopReconstructPass::processLoop(Loop *L) {
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
  if (canVectorize)
    return false;
  if (!canVectorize && LVL.MemoryNotVectorizable)
    return false;
  PHINode *induction = L->getInductionVariable(*SE);
  if (induction == nullptr) {
    LLVM_DEBUG(dbgs() << "induction nullptr\n");
    return false;
  }
  Instruction *udiv = nullptr, *urem = nullptr;
  bool additionalUsers = false;
  BasicBlock *preheader = L->getLoopPreheader();
  BasicBlock *header = L->getHeader();
  BasicBlock *exit = L->getExitBlock();
  BasicBlock *latch = L->getLoopLatch();
  auto condition = L->getLatchCmpInst();
  auto indNext = dyn_cast<Instruction>(
      induction->getIncomingValueForBlock(L->getLoopLatch()));
  Instruction *totalCount = dyn_cast<Instruction>(condition->getOperand(1));
  if (totalCount == indNext)
    totalCount = dyn_cast<Instruction>(condition->getOperand(0));

  if (totalCount == nullptr) {
    LLVM_DEBUG(dbgs() << "total count not found" << *condition << " \n");
    return false;
  }

  for (User *user : induction->users()) {
    if (user == condition || user == indNext)
      continue;
    if (Instruction *I = dyn_cast<Instruction>(user)) {
      switch (I->getOpcode()) {
      case Instruction::UDiv:
        if (udiv)
          LLVM_DEBUG(dbgs()
                     << "udiv already set " << *udiv << " " << *I << "\n");
        udiv = I;
        break;
      case Instruction::URem:
        if (urem)
          LLVM_DEBUG(dbgs()
                     << "urem already set " << *urem << " " << *I << "\n");
        urem = I;
        break;
      case Instruction::Trunc:
      case Instruction::BitCast:
      case Instruction::Freeze:
      case Instruction::GetElementPtr:
        break;
      default:
        LLVM_DEBUG(dbgs() << *I << "\n");
        additionalUsers = true;
        break;
      }
    }
  }

  if (additionalUsers) {
    LLVM_DEBUG(dbgs() << "additional users\n");
    return false;
  }

  Value *innerCount = nullptr;
  if (udiv)
    innerCount = udiv->getOperand(1);
  if (!innerCount && urem)
    innerCount = urem->getOperand(1);

  if (urem && innerCount != urem->getOperand(1)) {
    LLVM_DEBUG(dbgs() << "Could not get innerCount " << *(urem->getOperand(1))
                      << " " << *innerCount << "\n");
    return false;
  }
  Value *outerCount = nullptr;
  if (totalCount->getOpcode() == Instruction::Mul) {
    outerCount = totalCount->getOperand(0);
    if (outerCount == innerCount)
      outerCount = totalCount->getOperand(1);
  }
  if (outerCount == nullptr) {
    LLVM_DEBUG(dbgs() << "outerCount not found " << *totalCount << "\n");
    return false;
  }

  MapVector<PHINode *, Value *> outerPhis, innerPhis;
  std::map<PHINode *, Value *> innerPhiStorePtrs, outerPhiStorePtrs;
  for (PHINode &phi : header->phis()) {
    if (&phi == induction)
      continue;
    bool phiFromLoopFlatenned = false, inner = false;
    if (phi.getNumUses() == 1)
      for (auto user : phi.users())
        if (SelectInst *selInst = dyn_cast<SelectInst>(user)) {
          Instruction *selCondition =
              dyn_cast<Instruction>(selInst->getCondition());
          ICmpInst *icmp = dyn_cast<ICmpInst>(selCondition);
          if (icmp) {
            if (icmp->isEquality() && icmp->getOperand(0) == urem)
              if (Constant *c = dyn_cast<Constant>(icmp->getOperand(1)))
                if (c->isZeroValue()) {
                  phiFromLoopFlatenned = true;
                }
          }
          if (!phiFromLoopFlatenned) {
            LLVM_DEBUG(dbgs()
                       << *selInst
                       << " select uses phi but the choise is not based on "
                          "inneritr equals zero\n");
            return false;
          }

          if (icmp->getPredicate() == ICmpInst::ICMP_EQ &&
              selInst->getFalseValue() == &phi)
            inner = true;
          else if (icmp->getPredicate() == ICmpInst::ICMP_NE &&
                   selInst->getTrueValue() == &phi)
            inner = true;

          if (inner) {
            innerPhis.insert(std::make_pair<PHINode *, Value *>(
                &phi, icmp->getPredicate() == ICmpInst::ICMP_NE
                          ? selInst->getFalseValue()
                          : selInst->getTrueValue()));
          } else {
            outerPhis.insert(std::make_pair<PHINode *, Value *>(
                &phi, icmp->getPredicate() == ICmpInst::ICMP_NE
                          ? selInst->getTrueValue()
                          : selInst->getFalseValue()));
          }

          Value *v = phi.getIncomingValueForBlock(latch);
          if (v->getType() == Type::getInt1Ty(header->getContext())) {
            for (User *user : v->users()) {
              if (isa<ZExtInst>(user)) {
                v = user;
                break;
              }
            }
          }
          Value *storePtr = nullptr;
          for (User *user : v->users()) {
            if (StoreInst *storeInst = dyn_cast<StoreInst>(user)) {
              auto ptr = storeInst->getPointerOperand();
              if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ptr)) {
                if (gep->getOperand(1) == udiv)
                  storePtr = gep->getOperand(0);
              }
            }
          }

          if (storePtr != nullptr) {
            if (inner)
              innerPhiStorePtrs.insert(std::pair(&phi, storePtr));
            else
              outerPhiStorePtrs.insert(std::pair(&phi, storePtr));
          }
        }
  }

  // errs() << "inner phis \n";
  // for (auto innerPHI : innerPhis) {
  //   errs() << *(innerPHI.first) << " " << *(innerPHI.second) << " ";
  //   if (innerPhiStorePtrs.find(innerPHI.first) != innerPhiStorePtrs.end())
  //     errs() << *(innerPhiStorePtrs[innerPHI.first]);
  //   errs() << "\n";
  // }

  if (outerPhis.size() > 0) {
    #ifndef NDEBUG
    LLVM_DEBUG(dbgs() << "outer phis \n");
    for (auto outerPHI : outerPhis) {
      LLVM_DEBUG(dbgs() << *(outerPHI.first) << " " << *(outerPHI.second)
                        << " ");
      if (outerPhiStorePtrs.find(outerPHI.first) != outerPhiStorePtrs.end())
        LLVM_DEBUG(dbgs() << *(outerPhiStorePtrs[outerPHI.first]));
      LLVM_DEBUG(dbgs() << "\n");
    }
    #endif
    return false;
  }

  bool swap = false;
  if (!innerPhis.empty() && outerPhis.empty())
    swap = true;

  IRBuilder<> Builder(header->getContext());
  SmallDenseSet<Value *> newStorePtrs;
  if (swap) {
    if (innerPhis.size() != innerPhiStorePtrs.size()) {
      for (auto innerPHI : innerPhis) {
        if (innerPhiStorePtrs.find(innerPHI.first) == innerPhiStorePtrs.end()) {
          Builder.SetInsertPoint(preheader->getTerminator());
          auto ptr = MPCVecUtils::createMalloc(
              Builder, F, innerPHI.first->getType(), outerCount,
              innerPHI.first->hasMetadata("secret_shared"),
              "");
          innerPhiStorePtrs.insert({innerPHI.first, ptr});
          newStorePtrs.insert(ptr);
        }
      }
    }
  }

  BasicBlock *reconHeader = BasicBlock::Create(
      header->getContext(), "reconHeader", header->getParent(), header);
  preheader->replaceSuccessorsPhiUsesWith(reconHeader);
  preheader->getTerminator()->replaceSuccessorWith(header, reconHeader);
  BasicBlock *reconLatch =
      BasicBlock::Create(header->getContext(), "reconLatch",
                         header->getParent(), L->getExitBlock());
  latch->getTerminator()->replaceSuccessorWith(exit, reconLatch);
  // LCSSA phi nodes in exit still reference latch as incoming block, but latch
  // no longer has a direct edge to exit. Update them to use reconLatch instead.
  exit->replacePhiUsesWith(latch, reconLatch);
  Builder.SetInsertPoint(reconHeader);
  PHINode *reconInduction = Builder.CreatePHI(
      induction->getType(), induction->getNumIncomingValues());
  reconInduction->addIncoming(induction->getIncomingValueForBlock(reconHeader),
                              preheader);
  Builder.CreateBr(header);
  Builder.SetInsertPoint(reconLatch);

  Value *reconIndNext = Builder.CreateAdd(
      reconInduction, ConstantInt::get(reconInduction->getType(), 1));
  reconInduction->addIncoming(reconIndNext, reconLatch);
  Value *reconLatchCond =
      Builder.CreateICmpEQ(reconIndNext, (swap ? innerCount : outerCount));
  Builder.CreateCondBr(reconLatchCond, exit, reconHeader);
  MDNode *mdnode = MDNode::get(
      header->getContext(), MDString::get(header->getContext(), "llvm.loop"));
  reconLatch->getTerminator()->setMetadata("llvm.loop", mdnode);

  if (condition->getOperand(1) == totalCount)
    condition->setOperand(1, (swap ? outerCount : innerCount));
  else
    condition->setOperand(0, (swap ? outerCount : innerCount));

  Builder.SetInsertPoint(header->getFirstNonPHI());
  Instruction *tmp = dyn_cast<Instruction>(
      Builder.CreateMul(swap ? induction : reconInduction, innerCount));
  Instruction *overallInd = dyn_cast<Instruction>(
      Builder.CreateAdd(tmp, swap ? reconInduction : induction));
  induction->replaceAllUsesWith(overallInd);
  tmp->setOperand(0, induction);
  if (indNext->getOperand(0) == overallInd)
    indNext->setOperand(0, induction);
  else
    indNext->setOperand(1, induction);

  if (udiv) {
    if (swap) {
      udiv->replaceAllUsesWith(induction);
    } else {
      udiv->replaceAllUsesWith(reconInduction);
    }
    udiv->eraseFromParent();
  }
  if (urem) {
    if (swap) {
      urem->replaceAllUsesWith(reconInduction);
    } else {
      urem->replaceAllUsesWith(induction);
    }
    urem->eraseFromParent();
  }
  if (swap) {
    for (auto innerPhi : innerPhis) {
      PHINode *originalPhi = innerPhi.first;
      auto type = originalPhi->getType();
      if (type == Builder.getInt1Ty()) {
        type = Builder.getInt8Ty();
      }

      if (LoadInst *load = dyn_cast<LoadInst>(innerPhi.second)) {
        GetElementPtrInst *gep =
            dyn_cast<GetElementPtrInst>(load->getPointerOperand());
        // If the initial value is load(anyBuffer[induction]), the buffer is
        // already correctly initialized (by vec-help or a previous pass).
        // Use the existing load directly and skip buffer initialization.
        if (gep && gep->getOperand(1) == induction) {
          auto user =
              dyn_cast<Instruction>(originalPhi->getUniqueUndroppableUser());
          user->replaceAllUsesWith(load);
          user->eraseFromParent();
          originalPhi->eraseFromParent();
          continue;
        }
      }

      Builder.SetInsertPoint(header->getFirstNonPHI());
      auto gep =
          Builder.CreateGEP(type, innerPhiStorePtrs[originalPhi], induction);
      Value *val = Builder.CreateLoad(type, gep);
      if (newStorePtrs.find(innerPhiStorePtrs[originalPhi]) !=
          newStorePtrs.end()) {
        Builder.SetInsertPoint(header->getTerminator());
        Builder.CreateStore(originalPhi->getIncomingValueForBlock(header), gep);
      }
      if (originalPhi->getType() == Builder.getInt1Ty())
        val = Builder.CreateTrunc(val, Builder.getInt1Ty());
      auto user =
          dyn_cast<Instruction>(originalPhi->getUniqueUndroppableUser());
      user->replaceAllUsesWith(val);
      user->eraseFromParent();
      Builder.SetInsertPoint(preheader->getTerminator());
      llvm::Function *storeFunc = cast<llvm::Function>(
          header->getParent()
              ->getParent()
              ->getOrInsertFunction(
                  "_ZN3MPC5storeEPvS0_iibb",
                  FunctionType::get(Type::getVoidTy(header->getContext()),
                                    {Builder.getPtrTy(), Builder.getPtrTy(),
                                     Builder.getInt32Ty(), Builder.getInt32Ty(),
                                     Builder.getInt1Ty(), Builder.getInt1Ty()},
                                    false))
              .getCallee());
      Value *elementPtr = Builder.CreateAlloca(type, Builder.getInt32(1), "");
      Value *toStore = Builder.CreateZExtOrTrunc(innerPhi.second, type);
      Builder.CreateStore(toStore, elementPtr);
      Value *n = Builder.CreateTruncOrBitCast(outerCount, Builder.getInt32Ty());
      int elementSize =
          header->getParent()->getParent()->getDataLayout().getTypeAllocSize(
              type);
      bool storeValInAllParties = true;
      if (originalPhi->hasMetadata("secret_shared")) {
        if (Instruction *I = dyn_cast<Instruction>(innerPhi.second)) {
          if (!I->hasMetadata("secret_shared"))
            storeValInAllParties = false;
        } else
          storeValInAllParties = false;
      }

      Builder.CreateCall(storeFunc, {innerPhiStorePtrs[originalPhi], elementPtr,
                                     n, Builder.getInt32(elementSize),
                                     Builder.getInt1(storeValInAllParties),
                                     Builder.getInt1(false)});
      originalPhi->eraseFromParent();
    }
  }

  return true;
}

bool MPCLoopReconstructPass::runImpl(
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

  SmallVector<Loop *, 8> Worklist;

  for (Loop *L : *LI) {
    BasicBlock *header = L->getHeader();
    if (header->getTerminator()->hasMetadata("llvm.mpc.loop.flattened"))
      Worklist.push_back(L);
  }

  bool Changed = false;
  for (Loop *L : Worklist) {
    Changed |= processLoop(L);
  }
  return Changed;
}

PreservedAnalyses
MPCLoopReconstructPass::run(llvm::Function &F,
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
  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

// // Create the printf function prototype (if it doesn't already exist)
// Function *PrintfFunc =
//     header->getParent()->getParent()->getFunction("printf");
// if (!PrintfFunc) {
//   // Declare the printf function: int printf(const char *fmt, ...)
//   FunctionType *PrintfType = FunctionType::get(
//       IntegerType::getInt32Ty(header->getContext()),
//       PointerType::get(Type::getInt8Ty(header->getContext()), 0), true);
//   PrintfFunc = Function::Create(PrintfType, Function::ExternalLinkage,
//                                 "printf", header->getParent()->getParent());
// }
// // Create the format string as a global constant (e.g., "%d\n")
// Value *FormatStr = Builder.CreateGlobalStringPtr("Value: %d %d\n");

// // Cast the integer to a type suitable for printf (e.g., int32)
// Value *IntToPrint1 =
//     Builder.CreateIntCast(induction, Builder.getInt32Ty(), false);
// Value *IntToPrint2 =
//     Builder.CreateIntCast(reconInduction, Builder.getInt32Ty(), false);

// // Call printf with the format string and the integer value
// Builder.CreateCall(PrintfFunc, {FormatStr, IntToPrint1, IntToPrint2});