#pragma once
#include "llvm/IR/PassManager.h"

namespace smaug {

// Lowers every function with secret arguments (from --metadata-path) to the
// runtime ABI in runtime/mpc/smaug_abi.def. Unsupported constructs stop
// compilation with a diagnostic naming the instruction and its location.
class MPCLowerPass : public llvm::PassInfoMixin<MPCLowerPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

// Narrows i8 buffers that only hold 0 or 1 to i1 buffers before mpc-lower.
// Not implemented yet: it leaves the module unchanged.
class MPCNarrowBoolPass : public llvm::PassInfoMixin<MPCNarrowBoolPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &, llvm::ModuleAnalysisManager &) {
    return llvm::PreservedAnalyses::all();
  }
};

} // namespace smaug
