#pragma once
#include "llvm/IR/PassManager.h"
// #include "Common.h"
using namespace llvm;
class SecSharedMetadataHelperPass
    : public llvm::PassInfoMixin<SecSharedMetadataHelperPass> {
private:
public:
  void allocaVsInputMap(Function &F, std::ofstream &outputFile);
  llvm::PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
