
#include "llvm/Support/FormatVariadic.h"

#include "../include/MPCLoopFlatten.h"
#include "MPCLoopFlatten.h"

PreservedAnalyses MPCLoopFlattenPass::run(llvm::Function &F,
                                          llvm::FunctionAnalysisManager &FAM) {
  bool hasSecretShared = false;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (I.hasMetadata("secret_shared")) {
        hasSecretShared = true;
      }
    }
  }
  if (!hasSecretShared)
    return PreservedAnalyses::all();

  auto &LI = FAM.getResult<LoopAnalysis>(F);

  auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);

  LoopAccessInfoManager &LAIs = FAM.getResult<LoopAccessAnalysis>(F);

  MPCLoopFlatten flatten(F, SE, LI, DT, AC, LAIs);
  bool Changed = flatten.runImpl();

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
