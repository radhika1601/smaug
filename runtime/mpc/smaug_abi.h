#pragma once
// C declarations of the runtime ABI used by the mpc-lower pass. The list and
// its conventions are in smaug_abi.def.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMAUG_FN(ret, name, params, sig) ret name params;
#include "smaug_abi.def"
#undef SMAUG_FN

// smaug_reduce operations.
enum {
  SMAUG_RED_ADD = 1,
  SMAUG_RED_MUL = 2,
  SMAUG_RED_AND = 3,
  SMAUG_RED_OR = 4,
  SMAUG_RED_XOR = 5,
  SMAUG_RED_SMAX = 6,
  SMAUG_RED_SMIN = 7,
  SMAUG_RED_UMAX = 8,
  SMAUG_RED_UMIN = 9,
};

// icmp predicates, numbered as llvm::CmpInst::Predicate.
enum {
  SMAUG_ICMP_EQ = 32,
  SMAUG_ICMP_NE = 33,
  SMAUG_ICMP_UGT = 34,
  SMAUG_ICMP_UGE = 35,
  SMAUG_ICMP_ULT = 36,
  SMAUG_ICMP_ULE = 37,
  SMAUG_ICMP_SGT = 38,
  SMAUG_ICMP_SGE = 39,
  SMAUG_ICMP_SLT = 40,
  SMAUG_ICMP_SLE = 41,
};

// Ordered fcmp predicates, numbered as llvm::CmpInst::Predicate. Unordered
// predicates are rejected by the pass.
enum {
  SMAUG_FCMP_OEQ = 1,
  SMAUG_FCMP_OGT = 2,
  SMAUG_FCMP_OGE = 3,
  SMAUG_FCMP_OLT = 4,
  SMAUG_FCMP_OLE = 5,
  SMAUG_FCMP_ONE = 6,
};

#ifdef __cplusplus
}
#endif
