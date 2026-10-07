#include "MPCLoopReconstruct.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>

#define DEBUG_TYPE "mpc-loop-reconstruct"
#include "MPCVecUtils.h"

// The buffers the instructions of L write to. Unknown is set when L writes
// memory that cannot be named, e.g. through a call.
struct LoopWrites {
  SmallPtrSet<const Value *, 8> Objects;
  bool Unknown = false;
};

static LoopWrites loopWrites(Loop *L) {
  LoopWrites W;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB) {
      if (!I.mayWriteToMemory())
        continue;
      if (auto *SI = dyn_cast<StoreInst>(&I))
        W.Objects.insert(getUnderlyingObject(SI->getPointerOperand()));
      else if (auto *MI = dyn_cast<MemIntrinsic>(&I))
        W.Objects.insert(getUnderlyingObject(MI->getRawDest()));
      else
        W.Unknown = true;
    }
  return W;
}

// The instructions of L that V is computed from, operands before users, when
// V depends on the loop only through the outer index: it uses no phi of L,
// no instruction with side effects, and, when W is given, no load from a
// buffer that L writes. The leaves are `outer` and values defined outside L.
// Buffers are told apart by their underlying objects, as in the rest of the
// pipeline, where distinct arguments and allocations do not alias.
static bool collectInitChain(Value *V, Loop *L, Value *outer,
                             SmallVectorImpl<Instruction *> &chain,
                             SmallPtrSetImpl<Instruction *> &seen,
                             const LoopWrites *W = nullptr) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I || V == outer || !L->contains(I) || seen.contains(I))
    return true;
  if (isa<PHINode>(I) || I->mayHaveSideEffects() || I->isTerminator())
    return false;
  if (auto *LI = dyn_cast<LoadInst>(I))
    if (W && (W->Unknown || W->Objects.contains(getUnderlyingObject(
                                LI->getPointerOperand()))))
      return false;
  seen.insert(I);
  for (Value *Op : I->operands())
    if (!collectInitChain(Op, L, outer, chain, seen, W))
      return false;
  chain.push_back(I);
  return true;
}

// Whether init is phi's own buffer at the outer index, which is used in
// place rather than read before the loop.
static bool isOwnBufferLoad(Value *init, Value *storePtr, Value *outer) {
  auto *LI = dyn_cast<LoadInst>(init);
  auto *GEP = LI ? dyn_cast<GetElementPtrInst>(LI->getPointerOperand()) : nullptr;
  return GEP && storePtr && GEP->getPointerOperand() == storePtr &&
         GEP->getNumIndices() == 1 && GEP->getOperand(1) == outer &&
         GEP->getSourceElementType() == LI->getType();
}

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
            Value *init = icmp->getPredicate() == ICmpInst::ICMP_NE
                              ? selInst->getFalseValue()
                              : selInst->getTrueValue();
            innerPhis.insert(std::make_pair(&phi, init));
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

  // The initial values are computed before the loop, one per outer index,
  // so they may depend on the loop only through it, and may not read a
  // buffer the loop writes: that read would see the buffer before the
  // loop's writes. Checked before anything is changed.
  LoopWrites writes = loopWrites(L);
  for (auto &[phi, init] : innerPhis) {
    auto it = innerPhiStorePtrs.find(phi);
    Value *storePtr = it == innerPhiStorePtrs.end() ? nullptr : it->second;
    if (isOwnBufferLoad(init, storePtr, udiv))
      continue;
    SmallVector<Instruction *> chain;
    SmallPtrSet<Instruction *, 8> seen;
    if (!collectInitChain(init, L, udiv, chain, seen, &writes)) {
      LLVM_DEBUG(dbgs() << *init
                        << " initial value depends on more than the outer "
                           "index, or reads a buffer the loop writes\n");
      return false;
    }
  }

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

      MDNode *secret = originalPhi->getMetadata("secret_shared");
      Value *storePtr = innerPhiStorePtrs[originalPhi];
      // Set when the phi's buffer is initialized element by element before
      // the loop, so the fill below is not needed.
      bool initialized = false;
      auto *initInst = dyn_cast<Instruction>(innerPhi.second);
      if (initInst && L->contains(initInst)) {
        // The initial value depends on the outer index, which is now
        // `induction`.
        LoadInst *load = dyn_cast<LoadInst>(initInst);
        auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand())
                         : nullptr;
        bool plainLoad = gep && gep->getNumIndices() == 1 &&
                         gep->getOperand(1) == induction &&
                         gep->getSourceElementType() == load->getType();
        // The initial value is the phi's own buffer at [induction], so the
        // buffer already holds the running value. Use the load directly.
        if (isOwnBufferLoad(initInst, storePtr, induction)) {
          auto user =
              dyn_cast<Instruction>(originalPhi->getUniqueUndroppableUser());
          user->replaceAllUsesWith(load);
          user->eraseFromParent();
          originalPhi->eraseFromParent();
          continue;
        }
        if (plainLoad && load->getType() == type) {
          // The initial values come from another buffer with the same
          // indexing and type: copy it.
          Builder.SetInsertPoint(preheader->getTerminator());
          const DataLayout &DL =
              header->getParent()->getParent()->getDataLayout();
          Value *count =
              Builder.CreateZExtOrTrunc(outerCount, Builder.getInt64Ty());
          CallInst *copy = Builder.CreateMemCpy(
              storePtr, DL.getABITypeAlign(type), gep->getPointerOperand(),
              DL.getABITypeAlign(type),
              Builder.CreateMul(count,
                                Builder.getInt64(DL.getTypeAllocSize(type))));
          if (secret)
            copy->setMetadata("secret_shared", secret);
        } else {
          // Compute each initial value in a loop over the outer index before
          // the reconstructed loop: the instructions it is computed from are
          // cloned with `induction` replaced by the counter, and the result,
          // widened for a bool phi, is stored to the phi's buffer.
          SmallVector<Instruction *> chain;
          SmallPtrSet<Instruction *, 8> seen;
          collectInitChain(initInst, L, induction, chain, seen);
          BasicBlock *initEnd =
              SplitBlock(preheader, preheader->getTerminator());
          BasicBlock *initLoop = BasicBlock::Create(
              header->getContext(), "reconInit", header->getParent(), initEnd);
          preheader->getTerminator()->eraseFromParent();
          Builder.SetInsertPoint(preheader);
          Value *count =
              Builder.CreateZExtOrTrunc(outerCount, Builder.getInt64Ty());
          Builder.CreateCondBr(Builder.CreateICmpSGT(count, Builder.getInt64(0)),
                               initLoop, initEnd);
          Builder.SetInsertPoint(initLoop);
          PHINode *k = Builder.CreatePHI(Builder.getInt64Ty(), 2);
          k->addIncoming(Builder.getInt64(0), preheader);
          DenseMap<Value *, Value *> map;
          map[induction] = Builder.CreateZExtOrTrunc(k, induction->getType());
          for (Instruction *I : chain) {
            Instruction *C = I->clone();
            for (Use &U : C->operands())
              if (Value *M = map.lookup(U.get()))
                U.set(M);
            Builder.Insert(C);
            map[I] = C;
          }
          Value *wide = Builder.CreateZExtOrTrunc(map[initInst], type);
          StoreInst *st =
              Builder.CreateStore(wide, Builder.CreateGEP(type, storePtr, k));
          if (secret) {
            if (auto *W = dyn_cast<Instruction>(wide); W && W != map[initInst])
              W->setMetadata("secret_shared", secret);
            st->setMetadata("secret_shared", secret);
          }
          Value *next = Builder.CreateAdd(k, Builder.getInt64(1));
          k->addIncoming(next, initLoop);
          Builder.CreateCondBr(Builder.CreateICmpSLT(next, count), initLoop,
                               initEnd);
          // Later code inserts before the loop at the end of the preheader.
          preheader = initEnd;
        }
        initialized = true;
      }

      Builder.SetInsertPoint(header->getFirstNonPHI());
      auto gep =
          Builder.CreateGEP(type, innerPhiStorePtrs[originalPhi], induction);
      LoadInst *bufLoad = Builder.CreateLoad(type, gep);
      if (initialized && secret)
        bufLoad->setMetadata("secret_shared", secret);
      Value *val = bufLoad;
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
      if (initialized) {
        originalPhi->eraseFromParent();
        continue;
      }
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