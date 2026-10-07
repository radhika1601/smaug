#pragma once
#include "llvm/Support/CommandLine.h"

// Command-line options shared by the link passes and the pipeline builders.

// GC (garbled circuit) backend when true, GMW backend when false.
extern llvm::cl::opt<bool> UseGCMode;

// Which lowering the smaug-link stage runs.
enum class MPCLowering { Legacy, New };
extern llvm::cl::opt<MPCLowering> Lowering;
