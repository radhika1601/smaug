
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <map>
#include <set>

using namespace llvm;

class CheckSecretShared {
public:
  std::map<Function *, SmallVector<Argument *> *> args;
  std::set<Function *> secSharedFuncs;
  std::map<Function *, char> outputFuncs;
  // TODO: add outputs of functions here
  std::map<Function *, std::map<Value *, Value *>> readArgAccess,
      writeArgAccess;
  std::map<Function *, std::map<Value *, int>> sizesMap;
  CheckSecretShared(std::string filePath, Module &M) {

    // Reading the file into a buffer
    ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr =
        MemoryBuffer::getFile(filePath);
    if (std::error_code EC = fileOrErr.getError()) {
      errs() << "Error reading file: " << EC.message() << "\t"
                        << filePath << "\n";
      LLVM_DEBUG(dbgs() << "Error reading file: " << EC.message() << "\t"
                        << filePath << "\n");

    } else {
      auto &buffer = *fileOrErr.get();

      // Parsing JSON from the buffer
      Expected<json::Value> parsed = json::parse(buffer.getBuffer());
      if (!parsed) {
        errs() << "failed to parse json\n";
        LLVM_DEBUG(dbgs() << "Failed to parse JSON: "
                          << toString(parsed.takeError()) << "\n");
      } else {

        // Use the parsed JSON object
        json::Object *obj = parsed->getAsObject();

        for (Function &F : M) {
          if (!F.isDeclaration()) {
            SmallVector<Argument *> *Args = new SmallVector<Argument *>(0);
            json::Object *Fobj = obj->getObject(F.getName());
            if (Fobj) {
              json::Array *inputs = Fobj->getArray("input");
              if (inputs->size() > 0) {
                if (inputs->begin()->getAsArray() != nullptr) {
                  for (auto itr = inputs->begin(); itr != inputs->end();
                       ++itr) {
                    json::Array *arr = itr->getAsArray();
                    if (arr->size() == F.arg_size()) {
                      inputs = arr;
                      break;
                    }
                  }
                }
                for (size_t i = 0; i < inputs->size(); ++i) {
                  if ((inputs->begin() + i)->getAsInteger() == 1) {
                    Args->push_back(F.getArg(i));
                  }
                }
              }
              auto o = Fobj->getInteger("output");
              if (o.has_value()) {
                outputFuncs.insert(std::make_pair(&F, o.value()));
              }
              json::Object *readAccess = Fobj->getObject("readAccess");
              if (readAccess) {
                readArgAccess.insert(
                    std::make_pair(&F, std::map<Value *, Value *>()));
                for (const auto &KV : *readAccess) {
                  llvm::StringRef key = KV.first;
                  const llvm::json::Value &value = KV.second;
                  int a;
                  bool b = key.getAsInteger(0, a);
                  auto num = value.getAsInteger();
                  if (!b && num.has_value()) {
                    readArgAccess[&F].insert(
                        std::make_pair(F.getArg(a), F.getArg(num.value())));
                  }
                }
              }
              json::Object *writeAccess = Fobj->getObject("writeAccess");
              if (writeAccess) {
                writeArgAccess.insert(
                    std::make_pair(&F, std::map<Value *, Value *>()));
                for (const auto &KV : *writeAccess) {
                  llvm::StringRef key = KV.first;
                  const llvm::json::Value &value = KV.second;
                  int a;
                  bool b = key.getAsInteger(0, a);
                  auto num = value.getAsInteger();
                  if (!b && num.has_value()) {
                    writeArgAccess[&F].insert(
                        std::make_pair(F.getArg(a), F.getArg(num.value())));
                  }
                }
              }
              json::Object *sizes = Fobj->getObject("sizes");
              if (sizes) {
                sizesMap.insert(std::make_pair(&F, std::map<Value *, int>()));
                for (const auto &KV : *sizes) {
                  llvm::StringRef key = KV.first;
                  const llvm::json::Value &value = KV.second;
                  int a;
                  bool b = key.getAsInteger(0, a);
                  auto num = value.getAsInteger();
                  if (!b && num.has_value()) {
                    sizesMap[&F].insert(
                        std::make_pair(F.getArg(a), num.value()));
                  }
                }
              }
            }
            args.insert(std::make_pair(&F, Args));
          }
        }
      }
    }
  }

  bool isSecretShared(Value *val, Function *F) {
    if (Instruction *I = dyn_cast<Instruction>(val)) {
      if (I->hasMetadata("secret_shared"))
        return true;
    }
    auto Args = args.find(F);
    if (Args == args.end())
      return false;
    if (std::find(Args->second->begin(), Args->second->end(), val) !=
        Args->second->end())
      return true;

    return false;
  }
};
