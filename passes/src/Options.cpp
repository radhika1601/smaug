#include "Options.h"

using namespace llvm;

cl::opt<bool> UseGCMode("gc", cl::desc("Lower to the garbled circuit backend"),
                        cl::init(true));

cl::opt<MPCLowering> Lowering(
    "mpc-lowering", cl::desc("Lowering run by the smaug-link stage"),
    cl::values(clEnumValN(MPCLowering::Legacy, "legacy",
                          "vec-mpc-link, mpc-remove-ops and mpc-link"),
               clEnumValN(MPCLowering::New, "new",
                          "mpc-narrow-bool and mpc-lower")),
    cl::init(MPCLowering::Legacy));
