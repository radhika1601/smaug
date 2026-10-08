#pragma once
#include "llvm/IR/Instruction.h"

#include <string>

namespace smaug {

// An unsupported construct, reported by mpc-lower with the instruction's
// location.
struct Diagnostic {
  const llvm::Instruction *I; // may be null
  std::string Message;
};

} // namespace smaug
