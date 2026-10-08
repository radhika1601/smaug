#pragma once
// Vectorized loops from mpc-loop-vectorize, rewritten to run their body once
// over the whole trip count.
//
// mpc-loop-vectorize emits <vscale x 1 x T> loops. Each one has a guard that
// skips it when vscale exceeds the trip count TC, a vector body stepping an
// induction variable by vscale up to n.vec = TC - TC % vscale, and a middle
// block that runs the scalar remainder loop when TC % vscale is not 0.
// Replacing vscale by TC makes the guard and the remainder dead and leaves
// one execution of the body with index 0; the vector values then hold TC
// elements each.
//
// Each step below checks the shape it relies on and reports a diagnostic
// naming the failed invariant:
//   V1 every scalable type is <vscale x 1 x T>
//   V2 the body is one block with a unique preheader and a unique exit (the
//      middle block)
//   V3 one scalar induction variable phi [0, preheader], stepping by vscale
//   V4 the latch is br (icmp eq index.next, n.vec), middle, body
//   V5 n.vec = sub TC, (urem TC, vscale)
//   V6 TC is defined outside the loop
//   V7 the middle block ends in br (icmp eq (urem ...), 0), exit, scalar.ph
//      or br exit, and the remainder branch folds away

#include "Diagnostic.h"

#include "llvm/IR/Function.h"

#include <vector>

namespace smaug {

struct VectorRegion {
  // A block before the preheader that holds the trip count and the region's
  // buffer allocations, which then dominate all of the region's code.
  llvm::BasicBlock *Setup = nullptr;
  llvm::BasicBlock *Preheader = nullptr; // vector.ph
  llvm::BasicBlock *Body = nullptr;      // vector.body, now run once
  llvm::BasicBlock *Middle = nullptr;    // middle.block
  llvm::Value *TripCount = nullptr;      // i64 TC
};

// Rewrites every vectorized loop in F. Returns the regions, or reports
// diagnostics and returns an empty list.
std::vector<VectorRegion> canonicalizeVectorLoops(llvm::Function &F,
                                                  std::vector<Diagnostic> &Diags);

} // namespace smaug
