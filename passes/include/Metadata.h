#pragma once
#include "llvm/IR/PassManager.h"
#include <set>
// #include "Common.h"
using namespace llvm;
class SecSharedMetadataPass
    : public llvm::PassInfoMixin<SecSharedMetadataPass> {
private:
  void setSecretShared(Instruction *v, LLVMContext &context,
                       std::set<llvm::Function *> &PubOutputs);
  void removeMetadata(Function &F);
  void setPtrSecretShared(Instruction *I, LLVMContext &context,
                          std::set<llvm::Function *> &PubOutputs);

public:
  void runImpl(Function &F, SmallVector<Argument *> &Args,
               std::set<llvm::Function *> &PubOutputs,
               std::set<llvm::Function *> &PrivOutputs);
  llvm::PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
