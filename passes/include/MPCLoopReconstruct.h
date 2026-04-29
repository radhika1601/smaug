#pragma once
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "Vectorize/LoopVectorizationLegality.h"

using namespace llvm;

class MPCLoopReconstructPass : public PassInfoMixin<MPCLoopReconstructPass> {
private:
  ScalarEvolution *SE;
  LoopInfo *LI;
  TargetTransformInfo *TTI;
  DominatorTree *DT;
  BlockFrequencyInfo *BFI;
  TargetLibraryInfo *TLI;
  DemandedBits *DB;
  AssumptionCache *AC;
  LoopAccessInfoManager *LAIs;
  OptimizationRemarkEmitter *ORE;
  ProfileSummaryInfo *PSI;
  bool runImpl(Function &F, ScalarEvolution &SE_, LoopInfo &LI_,
               TargetTransformInfo &TTI_, DominatorTree &DT_,
               BlockFrequencyInfo *BFI_, TargetLibraryInfo *TLI_,
               DemandedBits &DB_, AssumptionCache &AC_,
               LoopAccessInfoManager &LAIs_, OptimizationRemarkEmitter &ORE_,
               ProfileSummaryInfo *PSI_);
  bool processLoop(Loop *L);

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};
