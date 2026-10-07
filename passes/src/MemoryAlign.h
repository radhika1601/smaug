#pragma once
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "Vectorize/LoopVectorizationLegality.h"
#include <map>
#include <set>
#include <tuple>

using namespace llvm;
#define DEBUG_TYPE "mem-align"
#include "MPCVecUtils.h"

class MemoryAlign {
public:
  MemoryAlign(Function &F, ScalarEvolution &SE_, LoopInfo &LI_,
              TargetTransformInfo &TTI_, DominatorTree &DT_,
              BlockFrequencyInfo *BFI_, TargetLibraryInfo *TLI_,
              DemandedBits &DB_, AssumptionCache &AC_,
              LoopAccessInfoManager &LAIs_, OptimizationRemarkEmitter &ORE_,
              ProfileSummaryInfo *PSI_)
      : F(&F), SE(&SE_), LI(&LI_), TTI(&TTI_), DT(&DT_), BFI(BFI_), TLI(TLI_),
        DB(&DB_), AC(&AC_), LAIs(&LAIs_), ORE(&ORE_), PSI(PSI_) {}
  bool runImpl();

private:
  Function *F;
  ScalarEvolution *SE;
  LoopInfo *LI;
  TargetTransformInfo *TTI;
  DominatorTree *DT;
  BlockFrequencyInfo *BFI;
  TargetLibraryInfo *TLI;
  DemandedBits *DB;
  AssumptionCache *AC;
  LoopAccessInfoManager *LAIs;
  OptimizationRemarkEmitter *ORE;
  ProfileSummaryInfo *PSI;
  MPCVecUtils utils;
  bool processLoop(Loop *L);

  void getInstsToDuplicate(Instruction *I,
                           SmallVector<Instruction *> &toDuplicate, Loop *L,
                           Instruction *induction);
  std::pair<BasicBlock *, BasicBlock *>
  createNewLoop(IRBuilder<> &Builder, Loop *L, BasicBlock *insertBefore,
                BasicBlock *insertAfter, Value *loopCount, StringRef name = "");
};
std::pair<BasicBlock *, BasicBlock *>
MemoryAlign::createNewLoop(IRBuilder<> &Builder, Loop *L,
                           BasicBlock *insertBefore, BasicBlock *insertAfter,
                           Value *loopCount, StringRef name) {
  PHINode *induction = L->getInductionVariable(*SE);
  BasicBlock *newLoop = BasicBlock::Create(
      F->getContext(), name + ".storesLoop", F, insertBefore);
  BasicBlock *newPreheader =
      BasicBlock::Create(F->getContext(), name + ".storesPH", F, newLoop);

  PHINode *newInduction = PHINode::Create(loopCount->getType(), 2, "", newLoop);

  BranchInst::Create(newLoop, newPreheader);
  Builder.SetInsertPoint(newLoop);
  Value *indNext = Builder.CreateAdd(
      newInduction, ConstantInt::get(loopCount->getType(), 1), "ind.next");
  newInduction->addIncoming(indNext, newLoop);
  newInduction->addIncoming(
      induction->getIncomingValueForBlock(L->getLoopPreheader()), newPreheader);
  Value *latchCmp = Builder.CreateICmpEQ(indNext, loopCount);
  BranchInst::Create(insertBefore, newLoop, latchCmp, newLoop);
  insertAfter->replaceSuccessorsPhiUsesWith(newLoop);

  auto terminator = insertAfter->getTerminator();
  if (BranchInst *bi = dyn_cast<BranchInst>(terminator)) {
    uint8_t n = bi->getNumSuccessors();
    for (uint8_t i = 0; i < n; ++i) {
      if (bi->getSuccessor(i) == insertBefore) {
        bi->setSuccessor(i, newPreheader);
      }
    }
  }

  return std::make_pair(newPreheader, newLoop);
}

bool MemoryAlign::runImpl() {

  SmallVector<Loop *, 8> Worklist;

  for (Loop *TopLevelLoop : *LI) {
    for (Loop *L : depth_first(TopLevelLoop)) {
      if (L->isInnermost()) {
        if (utils.hasSecretSharedInsts(L->getBlocksVector()))
          Worklist.push_back(L);
      }
    }
  }

  bool Changed = false;

  // Now walk the identified inner loops.
  for (Loop *L : Worklist) {
    Changed |= processLoop(L);
  }

  #ifndef NDEBUG
  if (Changed) {
    LLVM_DEBUG(dbgs() << "\n\n\n");
    for (auto &BB : *F) {
      LLVM_DEBUG(dbgs() << BB << "\n");
    }
  }
  #endif

  // Process each loop nest in the function.
  return Changed;
}

void MemoryAlign::getInstsToDuplicate(Instruction *I,
                                      SmallVector<Instruction *> &toDuplicate,
                                      Loop *L, Instruction *induction) {
  if (I == induction)
    return;
  if (std::find(toDuplicate.begin(), toDuplicate.end(), I) != toDuplicate.end())
    return;
  if (!L->contains(I->getParent()))
    return;
  for (uint8_t i = 0; i < I->getNumOperands(); ++i) {
    auto op = I->getOperand(i);
    if (auto opInst = dyn_cast<Instruction>(op)) {
      getInstsToDuplicate(opInst, toDuplicate, L, induction);
    }
  }
  toDuplicate.push_back(I);
}

bool MemoryAlign::processLoop(Loop *L) {
  assert(L->isInnermost() && "Only process inner loops.");

  smaug::LoopVectorizeHints Hints(L, true, *ORE, TTI);

  PredicatedScalarEvolution PSE(*SE, *L);

  // Check if it is legal to vectorize the loop.
  smaug::LoopVectorizationRequirements Requirements;
  smaug::LoopVectorizationLegality LVL(L, PSE, DT, TTI, TLI, F, *LAIs, LI, ORE,
                                &Requirements, &Hints, DB, AC, BFI, PSI);

  if (!LVL.canVectorize(false) && !LVL.MemoryNotVectorizable ) {
    return false;
  }

  auto loopCount = utils.getTripCount(L, SE);
  if (!loopCount)
    return false;

  SmallVector<Instruction *> loadInsts, storeInsts;

  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      auto ptr = getLoadStorePointerOperand(&I);
      if (!ptr)
        continue;

      auto *ScalarTy = getLoadStoreType(&I);
      if (!LVL.isConsecutivePtr(ScalarTy, ptr)) {
        if (isa<LoadInst>(&I))
          loadInsts.push_back(&I);
        if (isa<StoreInst>(&I))
          storeInsts.push_back(&I);
      }
    }
  }

  if (loadInsts.size() == 0 && storeInsts.size() == 0)
    return false;

  IRBuilder<> Builder(F->getContext());

  Instruction *induction = L->getInductionVariable(*SE);

  // get instructions to duplicate
  SmallVector<Instruction *> toDuplicate;
  for (auto I : loadInsts) {
    getInstsToDuplicate(I, toDuplicate, L, induction);
  }

  // Capture the original preheader BEFORE createNewLoop modifies the CFG,
  // since L->getLoopPreheader() changes after the new storesLoop is inserted.
  BasicBlock *origPH = L->getLoopPreheader();

  // If origPH is an outer loop header (e.g., reconHeader), hoist the malloc
  // to the outer loop's preheader to avoid repeated MPC allocations per
  // iteration, and free after the outer loop. Both blocks are found before
  // createNewLoop: LoopInfo does not know the new storesLoop blocks, so
  // afterwards the outer loop appears to exit into them. The malloc is only
  // hoisted when the free can follow it out of the outer loop.
  BasicBlock *outerPH = nullptr;
  BasicBlock *freeInsertBB = L->getExitBlock();
  {
    Loop *outerLoop = LI->getLoopFor(origPH);
    if (outerLoop && outerLoop->getHeader() == origPH) {
      BasicBlock *PH = outerLoop->getLoopPreheader();
      BasicBlock *outerExit = outerLoop->getExitBlock();
      if (PH && outerExit) {
        outerPH = PH;
        freeInsertBB = outerExit;
      }
    }
  }

  auto p = createNewLoop(Builder, L, L->getHeader(), origPH,
                         loopCount, L->getName());
  BasicBlock *storePH = p.first;
  BasicBlock *storeLoop = p.second;
  std::map<Instruction *, Instruction *> loadMallocMap;
  BasicBlock *mallocInsertBB = outerPH ? outerPH : storePH;

  Builder.SetInsertPoint(mallocInsertBB->getFirstInsertionPt());
  for (auto I : loadInsts) {
    auto ptr = utils.createMalloc(Builder, F, I->getType(), loopCount,
                                  I->hasMetadata("secret_shared"),
                                  "");
    loadMallocMap.insert(std::make_pair(I, ptr));
  }

  Instruction &newInduction = *(storeLoop->begin());
  Instruction *insertAfter = nullptr;
  std::map<Instruction *, Instruction *> duplicatedInsts;
  for (auto I : toDuplicate) {
    Instruction *newInst = I->clone();
    if (insertAfter)
      newInst->insertAfter(insertAfter);
    else
      newInst->insertBefore(storeLoop->getFirstNonPHI());

    insertAfter = newInst;
    duplicatedInsts.insert(std::make_pair(I, newInst));
    utils.replaceOperand(newInst, induction, &newInduction);

    llvm::Use *ops = newInst->getOperandList();
    int num_ops = newInst->getNumOperands();
    for (int k = 0; k < num_ops; ++k) {
      Instruction *op = dyn_cast<Instruction>(ops[k]);
      if (duplicatedInsts.find(op) != duplicatedInsts.end()) {
        newInst->setOperand(k, duplicatedInsts[op]);
      }
    }
  }

  for (auto I : loadInsts) {
    auto newInst = duplicatedInsts.at(I);
    Builder.SetInsertPoint(newInst->getInsertionPointAfterDef().value());
    auto ptr =
        Builder.CreateGEP(I->getType(), loadMallocMap.at(I), &newInduction);
    // errs() << *(I->getType()) << " " << *ptr << "\n";
    Builder.CreateStore(duplicatedInsts.at(I), ptr, false);

    Builder.SetInsertPoint(I->getInsertionPointAfterDef().value());
    ptr = Builder.CreateGEP(I->getType(), loadMallocMap.at(I), induction);
    auto newVal = Builder.CreateLoad(I->getType(), ptr);
    I->replaceAllUsesWith(newVal);
    Builder.SetInsertPoint(freeInsertBB->getFirstNonPHI());
    utils.CreateFree(Builder, F, loadMallocMap.at(I));
    I->eraseFromParent();
  }

  if (storeInsts.size() > 0) {
    errs() << "TODO: memory align storeinsts\n";
  }

  return true;
}
