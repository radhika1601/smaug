#include "llvm/IR/Type.h"
// #include "llvm/IR/Module.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Support/CommandLine.h"
#include "MetadataHelper.h"

#include <fstream>
#include <map>
static cl::opt<std::string> OutputFilePath("output-path",
                                           cl::desc("Specify output filepath"),
                                           cl::value_desc("filepath"));

using namespace llvm;

void SecSharedMetadataHelperPass::allocaVsInputMap(Function &F,
                                                   std::ofstream &outputFile) {
  // std::map<Value *, bool> args;
  // for (auto &arg : F.args()) {
  //   args.insert(std::make_pair(&arg, false));
  // }
  SmallVector<bool> args(F.arg_size(), false);
  outputFile << F.getName().str() << "\n";

  // Iterate over each argument in the function
  for (auto &Arg : F.args()) {

    // Check all uses of the argument
    for (auto *User : Arg.users()) {
      if (auto *Store = dyn_cast<StoreInst>(User)) {
        if (Store->getValueOperand() == &Arg) {
          auto *Alloca = dyn_cast<AllocaInst>(Store->getPointerOperand());
          if (Alloca) {
            if (Alloca->hasMetadata("secret_shared")) {
              args[Arg.getArgNo()] = true;
            }
            break;
          }
        }
      }
    }
  }

  for (bool b : args) {
    outputFile << b << "\n";
  }
}

llvm::PreservedAnalyses
SecSharedMetadataHelperPass::run(Module &M, ModuleAnalysisManager &MAM) {
  std::ofstream outputFile;

  outputFile.open(
      formatv("{0}/{1}_SecSharedInfo.txt", OutputFilePath, M.getName()));
  if (!outputFile.is_open()) {
    errs() << "could not open file\n";
    return PreservedAnalyses::all();
  }

  for (Function &F : M) {
    if (!F.isDeclaration())
      allocaVsInputMap(F, outputFile);
  }

  outputFile.close();
  return PreservedAnalyses::all();
}
