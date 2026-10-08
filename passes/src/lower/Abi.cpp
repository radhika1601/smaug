#include "Abi.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace smaug {

namespace {

// name -> "ret:param,param,..." from smaug_abi.def.
const StringMap<const char *> &signatures() {
  static const StringMap<const char *> Sigs = {
#define SMAUG_FN(ret, name, params, sig) {#name, sig},
#include "mpc/smaug_abi.def"
#undef SMAUG_FN
  };
  return Sigs;
}

Type *typeFor(char C, LLVMContext &Ctx) {
  switch (C) {
  case 'v':
    return Type::getVoidTy(Ctx);
  case 'i':
    return Type::getInt32Ty(Ctx);
  case 'l':
    return Type::getInt64Ty(Ctx);
  case 'p':
    return PointerType::getUnqual(Ctx);
  }
  report_fatal_error(Twine("smaug_abi.def: bad type letter '") + Twine(C) +
                     "'");
}

} // namespace

FunctionCallee Abi::get(StringRef Name) {
  auto It = signatures().find(Name);
  if (It == signatures().end())
    report_fatal_error("mpc-lower: '" + Name + "' is not in smaug_abi.def");
  StringRef Sig = It->second;
  auto [Ret, Params] = Sig.split(':');
  LLVMContext &Ctx = M.getContext();
  SmallVector<Type *> ParamTys;
  for (char C : Params)
    if (C != ',')
      ParamTys.push_back(typeFor(C, Ctx));
  return M.getOrInsertFunction(
      Name, FunctionType::get(typeFor(Ret[0], Ctx), ParamTys, false));
}

} // namespace smaug
