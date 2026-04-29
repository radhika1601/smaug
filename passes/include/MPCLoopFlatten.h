#pragma once
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

using namespace llvm;

class MPCLoopFlattenPass : public PassInfoMixin<MPCLoopFlattenPass> {
private:
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};
