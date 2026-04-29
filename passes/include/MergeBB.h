#pragma once
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
using namespace llvm;

class MergeBBPass : public llvm::PassInfoMixin<MergeBBPass> {
private:
  bool mergeStraightLineBlocks(llvm::Function &F) {
    bool Changed = false;

    SmallVector<BasicBlock *> mergableBBs;

    for (auto &BB : llvm::make_early_inc_range(F)) {
      // Continue merging the block into its predecessor as long as it's
      // possible
      while (BB.hasNPredecessors(1) && !BB.isEntryBlock()) {
        llvm::BasicBlock *Pred = BB.getSinglePredecessor();
        if (!Pred)
          break;

        // Check if the predecessor has only this block as its successor
        if (Pred->getTerminator()->getNumSuccessors() == 1) {
          llvm::MergeBlockIntoPredecessor(&BB);
          Changed = true;
          break; // Break after merging to avoid using invalidated iterator
        } else {
          break; // Can't merge if the predecessor has multiple successors
        }
      }
    }

    return Changed;
  }

public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) {
    bool changed = mergeStraightLineBlocks(F);
    if (!changed) {
      return PreservedAnalyses::all();
    }
    return PreservedAnalyses::none();
  }
};
