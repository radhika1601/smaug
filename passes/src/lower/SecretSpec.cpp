#include "SecretSpec.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;

namespace smaug {

bool FunctionSpec::hasSecretArgs() const {
  for (const ArgSpec &A : Args)
    if (A.Secret)
      return true;
  return false;
}

static Error specError(StringRef Fn, const Twine &Msg) {
  return createStringError(inconvertibleErrorCode(),
                           "metadata for " + Fn + ": " + Msg);
}

// Reads {"<arg index>": <integer>} into Out.
static Error readArgMap(StringRef Fn, const json::Object &FObj, StringRef Key,
                        unsigned NumArgs,
                        SmallVectorImpl<std::pair<unsigned, int64_t>> &Out) {
  const json::Value *V = FObj.get(Key);
  if (!V)
    return Error::success();
  const json::Object *Obj = V->getAsObject();
  if (!Obj)
    return specError(Fn, "\"" + Key + "\" must be an object");
  for (const auto &KV : *Obj) {
    unsigned Arg;
    if (StringRef(KV.first).getAsInteger(10, Arg) || Arg >= NumArgs)
      return specError(Fn, "\"" + Key + "\" has an invalid argument index \"" +
                               StringRef(KV.first) + "\"");
    std::optional<int64_t> Num = KV.second.getAsInteger();
    if (!Num)
      return specError(Fn, "\"" + Key + "\"[" + StringRef(KV.first) +
                               "] must be an integer");
    Out.push_back({Arg, *Num});
  }
  return Error::success();
}

static Expected<FunctionSpec> readOptional(const Function &F,
                                           const json::Object &FObj,
                                           FunctionSpec Spec);

static Expected<FunctionSpec> readFunction(const Function &F,
                                           const json::Object &FObj) {
  StringRef Fn = F.getName();
  unsigned NumArgs = F.arg_size();
  FunctionSpec Spec;
  Spec.Args.resize(NumArgs);

  const json::Array *Input = FObj.getArray("input");
  if (!Input)
    return specError(Fn, "missing \"input\"");
  if (!Input->empty() && Input->front().getAsArray()) {
    const json::Array *Match = nullptr;
    for (const json::Value &Alt : *Input)
      if (const json::Array *A = Alt.getAsArray(); A && A->size() == NumArgs)
        Match = A;
    if (!Match)
      return specError(Fn, "no \"input\" list has " + Twine(NumArgs) +
                               " entries");
    Input = Match;
  }
  // An empty list means that no argument is secret.
  if (Input->empty())
    return readOptional(F, FObj, std::move(Spec));
  if (Input->size() != NumArgs)
    return specError(Fn, "\"input\" has " + Twine(Input->size()) +
                             " entries but the function has " +
                             Twine(NumArgs) + " arguments");
  for (unsigned I = 0; I < NumArgs; ++I) {
    std::optional<int64_t> V = (*Input)[I].getAsInteger();
    if (!V || (*V != 0 && *V != 1))
      return specError(Fn, "\"input\" entries must be 0 or 1");
    Spec.Args[I].Secret = *V == 1;
  }
  return readOptional(F, FObj, std::move(Spec));
}

// The keys other than "input".
static Expected<FunctionSpec> readOptional(const Function &F,
                                           const json::Object &FObj,
                                           FunctionSpec Spec) {
  StringRef Fn = F.getName();
  unsigned NumArgs = F.arg_size();
  SmallVector<std::pair<unsigned, int64_t>> Read, Write, Sizes;
  if (Error E = readArgMap(Fn, FObj, "readAccess", NumArgs, Read))
    return std::move(E);
  if (Error E = readArgMap(Fn, FObj, "writeAccess", NumArgs, Write))
    return std::move(E);
  if (Error E = readArgMap(Fn, FObj, "sizes", NumArgs, Sizes))
    return std::move(E);

  auto setLen = [&](unsigned Arg, int64_t Len, bool IsWrite) -> Error {
    if (Len < 0 || Len >= NumArgs ||
        !F.getArg(Len)->getType()->isIntegerTy())
      return specError(Fn, "length of argument " + Twine(Arg) +
                               " must name an integer argument");
    ArgSpec &A = Spec.Args[Arg];
    if (A.LenArg && *A.LenArg != Len)
      return specError(Fn, "argument " + Twine(Arg) +
                               " has two different length arguments");
    A.LenArg = Len;
    (IsWrite ? A.Write : A.Read) = true;
    return Error::success();
  };
  for (auto [Arg, Len] : Read)
    if (Error E = setLen(Arg, Len, false))
      return std::move(E);
  for (auto [Arg, Len] : Write)
    if (Error E = setLen(Arg, Len, true))
      return std::move(E);
  for (auto [Arg, Bits] : Sizes) {
    if (Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64)
      return specError(Fn, "\"sizes\"[" + Twine(Arg) + "] must be 1, 8, 16, "
                                                      "32 or 64");
    Spec.Args[Arg].Bits = Bits;
  }

  for (unsigned I = 0; I < NumArgs; ++I) {
    const ArgSpec &A = Spec.Args[I];
    Type *Ty = F.getArg(I)->getType();
    bool IsPtr = Ty->isPointerTy();
    if (A.LenArg && !IsPtr)
      return specError(Fn, "argument " + Twine(I) +
                               " has an access length but is not a pointer");
    unsigned TyBits = Ty->getPrimitiveSizeInBits();
    if (A.Bits && !IsPtr && TyBits != *A.Bits)
      return specError(Fn, "\"sizes\"[" + Twine(I) + "] is " + Twine(*A.Bits) +
                               " but the argument has " + Twine(TyBits) +
                               " bits");
    if (A.Secret && IsPtr && (!A.LenArg || !A.Bits))
      return specError(Fn, "secret pointer argument " + Twine(I) +
                               " needs \"sizes\" and \"readAccess\" or "
                               "\"writeAccess\"");
  }

  if (const json::Value *O = FObj.get("output")) {
    std::optional<int64_t> V = O->getAsInteger();
    if (!V || (*V != 0 && *V != 1))
      return specError(Fn, "\"output\" must be 0 or 1");
    Spec.Output = *V;
  }
  return Spec;
}

Expected<SecretSpec> SecretSpec::load(StringRef Path, Module &M) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return createStringError(Buf.getError(),
                             "cannot read metadata file " + Path);
  Expected<json::Value> Parsed = json::parse((*Buf)->getBuffer());
  if (!Parsed)
    return Parsed.takeError();
  const json::Object *Top = Parsed->getAsObject();
  if (!Top)
    return createStringError(inconvertibleErrorCode(),
                             "metadata file must hold a JSON object");
  SecretSpec Spec;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    const json::Object *FObj = Top->getObject(F.getName());
    if (!FObj)
      continue;
    Expected<FunctionSpec> FS = readFunction(F, *FObj);
    if (!FS)
      return FS.takeError();
    Spec.Funcs.insert({&F, std::move(*FS)});
  }
  return Spec;
}

const FunctionSpec *SecretSpec::get(const Function &F) const {
  auto It = Funcs.find(&F);
  return It == Funcs.end() ? nullptr : &It->second;
}

} // namespace smaug
