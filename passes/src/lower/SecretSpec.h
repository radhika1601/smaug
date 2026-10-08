#pragma once
// The per-function secrecy specification from the --metadata-path JSON file.
//
// {
//   "<mangled function name>": {
//     "input": [1, 0, ...],             // 1 = secret argument
//     "readAccess": {"<arg>": <len arg>},  // secret pointer args and the
//     "writeAccess": {"<arg>": <len arg>}, //   argument holding their length
//     "sizes": {"<arg>": <bits>},       // element width of pointer args
//     "output": 0 | 1                   // 0 = reveal the return value,
//   }                                   // 1 = return this party's share
// }
//
// "input" may also be a list of lists; the one with as many entries as the
// function has arguments is used.

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <optional>

namespace smaug {

struct ArgSpec {
  bool Secret = false;
  bool Read = false;
  bool Write = false;
  std::optional<unsigned> LenArg;
  std::optional<unsigned> Bits;
};

struct FunctionSpec {
  llvm::SmallVector<ArgSpec> Args;
  std::optional<int64_t> Output;

  bool hasSecretArgs() const;
};

class SecretSpec {
public:
  // Reads and checks the file. Functions named in the file but missing from
  // the module (e.g. inlined away) are ignored.
  static llvm::Expected<SecretSpec> load(llvm::StringRef Path,
                                         llvm::Module &M);

  const FunctionSpec *get(const llvm::Function &F) const;

  llvm::MapVector<const llvm::Function *, FunctionSpec> Funcs;
};

} // namespace smaug
