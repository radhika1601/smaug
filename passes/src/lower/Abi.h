#pragma once
// Declarations of the runtime ABI functions (runtime/mpc/smaug_abi.def).

#include "llvm/IR/Module.h"

namespace smaug {

class Abi {
public:
  explicit Abi(llvm::Module &M) : M(M) {}

  // The declaration of an ABI function. Aborts on a name that is not in
  // smaug_abi.def, which is a bug in the pass.
  llvm::FunctionCallee get(llvm::StringRef Name);

private:
  llvm::Module &M;
};

} // namespace smaug
