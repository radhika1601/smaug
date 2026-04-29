#pragma once
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

class PathCondition;
class CheckSecretShared;
class ConditionTree;

class MPCLoopCoalescingPass
    : public llvm::PassInfoMixin<MPCLoopCoalescingPass> {
private:
  CheckSecretShared *checkSecretShared;
  PathCondition *pc;
  bool samePrivateCondition(ConditionTree *a, ConditionTree *b, Function &F);
  void runImpl(Loop *outerLoop, Loop *innerLoop, ConstantInt *outerCount,
               ConstantInt *innerCount, IRBuilder<> &Builder, Function *F,
               DominatorTree *DT, LoopInfo *LI);
  void replaceOperand(Instruction *I, Value *oldOp, Value *newOp) {
    for (size_t i = 0; i < I->getNumOperands(); ++i) {
      if (I->getOperand(i) == oldOp) {
        I->setOperand(i, newOp);
        return;
      }
    }
  }

  void getOrder(Loop *L, SmallVector<BasicBlock *> &order, LoopInfo *LI);

public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};
