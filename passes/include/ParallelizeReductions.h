#pragma once
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "Vectorize/LoopVectorizationLegality.h"

using namespace llvm;

class ParallelizeReductionsPass
    : public llvm::PassInfoMixin<ParallelizeReductionsPass> {
private:
  // Function *F;
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

  bool runImpl(Function &F_, ScalarEvolution &SE_, LoopInfo &LI_,
               TargetTransformInfo &TTI_, DominatorTree &DT_,
               BlockFrequencyInfo *BFI_, TargetLibraryInfo *TLI_,
               DemandedBits &DB_, AssumptionCache &AC_,
               LoopAccessInfoManager &LAIs_, OptimizationRemarkEmitter &ORE_,
               ProfileSummaryInfo *PSI_);
  bool processLoop(Loop *L);

  void getReductionInsts(
      Loop *L, SmallVector<Instruction *> &instrs, SmallVector<Value *> &arrays,
      smaug::LoopVectorizationLegality::ReductionList &reductionVars);
  // Value *getTripCount(Loop *L);

public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};
