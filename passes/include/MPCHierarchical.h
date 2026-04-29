#pragma once
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <map>
#include <set>

using namespace llvm;

class PathCondition;
class ConditionTree;

class MPCHierarchicalPass : public llvm::PassInfoMixin<MPCHierarchicalPass> {
private:
  llvm::Constant *trueValue;
  MapVector<BasicBlock *, Value *> private_pc;
  MapVector<BasicBlock *, Value *> public_pc;
  PathCondition *pc;
  llvm::LLVMContext *context;
  LoopInfo *LI;
  std::map<ConditionTree *, std::pair<Value *, Value *>> conditionValMap;
  SmallVector<Value *> globalPHICondSelIntrs;

  MapVector<Value *, Value *> phiMap;
  SmallVector<BasicBlock *> newBBs;
  SmallVector<BasicBlock *> oldBBs;

  // ---- Utility methods (mirrored from MPCTransformPass) ----

  inline void replaceOperand(Instruction *I, Instruction *oldOp, Value *newOp) {
    llvm::Use *ops = I->getOperandList();
    int num_ops = I->getNumOperands();
    for (int i = 0; i < num_ops; ++i) {
      if (Instruction *op = dyn_cast<Instruction>(ops[i])) {
        if (op == oldOp) {
          I->setOperand(i, newOp);
          break;
        }
      }
    }
  }

  inline void setSecretShared(Value *val) {
    Instruction *I = dyn_cast<Instruction>(val);
    Function *F = I->getParent()->getParent();
    auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
    auto *node = MDNode::get(F->getContext(), MDStr);
    I->setMetadata("secret_shared", node);
  }

  inline bool checkNot(Value *v1, Value *v2);
  llvm::Instruction *getFirstNonPhiAfter(llvm::Instruction *inst);
  inline PHINode *createPhi(Instruction &I, BasicBlock *BB, BasicBlock *lastBB,
                            BasicBlock *newBB);
  inline Value *createAnd(Value *v1, Value *v2,
                          Instruction *insertBefore = nullptr,
                          DominatorTree *domTree = nullptr,
                          bool isAnd = true);
  std::pair<Value *, Value *> *getValForTree(
      ConditionTree *tree,
      std::map<ConditionTree *, std::pair<Value *, Value *>> &conditionValMap,
      DominatorTree &domTree, BasicBlock *bb);

  void updateSecretStoreInst(BasicBlock &bb, Value *cond);
  void updatePhi(BasicBlock *BB, PostDominatorTree *PDT, DominatorTree *DT,
                 SmallVector<BasicBlock *> &SortedBBs,
                 SmallVector<Value *> &PHICondSelIntrs, LoopInfo *LI);
  void updatePhiLoopHeader(BasicBlock *BB, SmallVector<BasicBlock *> &oldBBs,
                           SmallVector<BasicBlock *> &newBBs);
  // ---- Hierarchical-specific helpers ----

  void getConditionVals(Function &F);

BasicBlock * visit_loop(BasicBlock* BB, SmallVector<BasicBlock*>& oldBBs, SmallVector<BasicBlock*>& newBBs,
                                     BasicBlock* lastVisited, PostDominatorTree* PDT, DominatorTree* DT,
                                     LoopInfo* LI, Loop* L = nullptr);

  SmallVector<BasicBlock *> getDirectBlocks(Loop *L);
  void topologicalOrderForLoop(Loop *L, SmallVector<BasicBlock *> &order);
  void topologicalOrderFunctionLevel(Function &F,
                                     SmallVector<BasicBlock *> &order, LoopInfo *LI);
  void transformLoop(Loop *L, DominatorTree *DT, PostDominatorTree *PDT);
  void runPhiCondSelRelocate(SmallVector<Value *> &PHICondSelIntrs,
                             DominatorTree *DT);

  // Main implementation
  void runImpl(Function &F, LoopInfo *LI, DominatorTree *DT,
               PostDominatorTree *PDT);

public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};
