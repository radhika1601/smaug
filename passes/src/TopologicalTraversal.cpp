#include "TopologicalTraversal.h"
#include "llvm/ADT/PostOrderIterator.h"

#include "CustomPostOrder.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#define DEBUG_TYPE "topological-traversal"

llvm::PreservedAnalyses
TopologicalTraversal::run(llvm::Function &F,
                          llvm::FunctionAnalysisManager &FAM) {
  auto &LI = FAM.getResult<LoopAnalysis>(F);

  LLVM_DEBUG(dbgs() << "SCCs for " << F.getName() << " in post-order:\n");
  for (scc_iterator<Function *> I = scc_begin(&F), IE = scc_end(&F); I != IE;
       ++I) {
    // Obtain the vector of BBs in this SCC and print it out.
    const std::vector<BasicBlock *> &SCCBBs = *I;
    LLVM_DEBUG(dbgs() << "  SCC: ");
    for (std::vector<BasicBlock *>::const_iterator BBI = SCCBBs.begin(),
                                                   BBIE = SCCBBs.end();
         BBI != BBIE; ++BBI) {
      LLVM_DEBUG(dbgs() << (*BBI)->getName() << "  ");
    }
    LLVM_DEBUG(dbgs() << "\n");
    if (SCCBBs.size() > 1) {
      auto itr = SCCBBs.end();
      itr--;

      Loop *L = LI.getLoopFor(*itr);
      LLVM_DEBUG(dbgs() << "loop latch "
                        << L->getLoopLatch()->getName() << "\n");

      //   Worklist.push_back(L);
      for (custom_po_iterator LItr = custom_po_iterator::begin(
                                  L->getHeader(), L->getExitBlock()),
                              LIE = custom_po_iterator::end(L->getLoopLatch());
           LItr != LIE; ++LItr) {
        LLVM_DEBUG(dbgs() << (*LItr)->getName() << " ");
      }
      LLVM_DEBUG(dbgs() << "\n");
    }
  }

  return PreservedAnalyses::all();
}
