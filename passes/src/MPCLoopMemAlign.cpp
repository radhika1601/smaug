#include "MPCLoopMemAlign.h"
#include "MemoryAlign.h"

using namespace llvm;
#define DEBUG_TYPE "mem-align"

PreservedAnalyses MPCLoopMemAlignPass::run(llvm::Function &F,
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
  auto &TTI = FAM.getResult<TargetIRAnalysis>(F);
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
  auto &AC = FAM.getResult<AssumptionAnalysis>(F);
  auto &DB = FAM.getResult<DemandedBitsAnalysis>(F);
  auto &ORE = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  LoopAccessInfoManager &LAIs = FAM.getResult<LoopAccessAnalysis>(F);
  auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);
  ProfileSummaryInfo *PSI =
      MAMProxy.getCachedResult<ProfileSummaryAnalysis>(*F.getParent());
  BlockFrequencyInfo *BFI = nullptr;
  if (PSI && PSI->hasProfileSummary())
    BFI = &FAM.getResult<BlockFrequencyAnalysis>(F);

  MemoryAlign align(F, SE, LI, TTI, DT, BFI, &TLI, DB, AC, LAIs, ORE, PSI);
  bool Changed = align.runImpl();

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}
