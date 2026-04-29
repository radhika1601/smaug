//===- LoopVectorize.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the LLVM loop vectorizer. This pass modifies 'vectorizable' loops
// and generates target-independent LLVM-IR.
// The vectorizer uses the TargetTransformInfo analysis to estimate the costs
// of instructions in order to estimate the profitability of vectorization.
//
// The loop vectorizer combines consecutive loop iterations into a single
// 'wide' iteration. After this transformation the index is incremented
// by the SIMD vector width, and not by one.
//
// This pass has four parts:
// 1. The main loop pass that drives the different parts.
// 2. LoopVectorizationLegality - A unit that checks for the legality
//    of the vectorization.
// 3. InnerLoopVectorizer - A unit that performs the actual
//    widening of instructions.
// 4. LoopVectorizationCostModel - A unit that checks for the profitability
//    of vectorization. It decides on the optimal vector width, which
//    can be one, if vectorization is not profitable.
//
// There is a development effort going on to migrate loop vectorizer to the
// VPlan infrastructure and to introduce outer loop vectorization support (see
// docs/VectorizationPlan.rst and
// http://lists.llvm.org/pipermail/llvm-dev/2017-December/119523.html). For this
// purpose, we temporarily introduced the VPlan-native vectorization path: an
// alternative vectorization path that is natively implemented on top of the
// VPlan infrastructure. See EnableVPlanNativePath for enabling.
//
//===----------------------------------------------------------------------===//
//
// The reduction-variable vectorization is based on the paper:
//  D. Nuzman and R. Henderson. Multi-platform Auto-vectorization.
//
// Variable uniformity checks are inspired by:
//  Karrenberg, R. and Hack, S. Whole Function Vectorization.
//
// The interleaved access vectorization is based on the paper:
//  Dorit Nuzman, Ira Rosen and Ayal Zaks.  Auto-Vectorization of Interleaved
//  Data for SIMD
//
// Other ideas/concepts are from:
//  A. Zaks and D. Nuzman. Autovectorization in GCC-two years later.
//
//  S. Maleki, Y. Gao, M. Garzaran, T. Wong and D. Padua.  An Evaluation of
//  Vectorizing Compilers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_LOOPVECTORIZE_H
#define LLVM_TRANSFORMS_VECTORIZE_LOOPVECTORIZE_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include <functional>

namespace llvm {

class AssumptionCache;
class BlockFrequencyInfo;
class DemandedBits;
class DominatorTree;
class Function;
class Loop;
class LoopAccessInfoManager;
class LoopInfo;
class OptimizationRemarkEmitter;
class ProfileSummaryInfo;
class ScalarEvolution;
class TargetLibraryInfo;
class TargetTransformInfo;

extern cl::opt<bool> EnableLoopInterleaving;
extern cl::opt<bool> EnableLoopVectorization;

} // end namespace llvm

namespace smaug {
using namespace llvm;

/// A marker to determine if extra passes after loop vectorization should be
/// run.
struct ShouldRunExtraVectorPasses
    : public AnalysisInfoMixin<ShouldRunExtraVectorPasses> {
  static AnalysisKey Key;
  struct Result {
    bool invalidate(Function &F, const PreservedAnalyses &PA,
                    FunctionAnalysisManager::Invalidator &) {
      // Check whether the analysis has been explicitly invalidated. Otherwise,
      // it remains preserved.
      auto PAC = PA.getChecker<ShouldRunExtraVectorPasses>();
      return !PAC.preservedWhenStateless();
    }
  };

  Result run(Function &F, FunctionAnalysisManager &FAM) { return Result(); }
};

/// A pass manager to run a set of extra function simplification passes after
/// vectorization, if requested. LoopVectorize caches the
/// ShouldRunExtraVectorPasses analysis to request extra simplifications, if
/// they could be beneficial.
struct ExtraVectorPassManager : public FunctionPassManager {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) {
    auto PA = PreservedAnalyses::all();
    if (AM.getCachedResult<ShouldRunExtraVectorPasses>(F))
      PA.intersect(FunctionPassManager::run(F, AM));
    PA.abandon<ShouldRunExtraVectorPasses>();
    return PA;
  }
};

/// Reports a vectorization failure: print \p DebugMsg for debugging
/// purposes along with the corresponding optimization remark \p RemarkName.
/// If \p I is passed, it is an instruction that prevents vectorization.
/// Otherwise, the loop \p TheLoop is used for the location of the remark.
void reportVectorizationFailure(const StringRef DebugMsg,
    const StringRef OREMsg, const StringRef ORETag,
    OptimizationRemarkEmitter *ORE, Loop *TheLoop, Instruction *I = nullptr);

/// Reports an informative message: print \p Msg for debugging purposes as well
/// as an optimization remark. Uses either \p I as location of the remark, or
/// otherwise \p TheLoop.
void reportVectorizationInfo(const StringRef OREMsg, const StringRef ORETag,
                             OptimizationRemarkEmitter *ORE, Loop *TheLoop,
                             Instruction *I = nullptr);

} // end namespace smaug

namespace smaug {

struct LoopVectorizeOptions {
  /// If false, consider all loops for interleaving.
  /// If true, only loops that explicitly request interleaving are considered.
  bool InterleaveOnlyWhenForced;

  /// If false, consider all loops for vectorization.
  /// If true, only loops that explicitly request vectorization are considered.
  bool VectorizeOnlyWhenForced;

  /// The current defaults when creating the pass with no arguments are:
  /// EnableLoopInterleaving = true and EnableLoopVectorization = true. This
  /// means that interleaving default is consistent with the cl::opt flag, while
  /// vectorization is not.
  /// FIXME: The default for EnableLoopVectorization in the cl::opt should be
  /// set to true, and the corresponding change to account for this be made in
  /// opt.cpp. The initializations below will become:
  /// InterleaveOnlyWhenForced(!EnableLoopInterleaving)
  /// VectorizeOnlyWhenForced(!EnableLoopVectorization).
  LoopVectorizeOptions()
      : InterleaveOnlyWhenForced(false), VectorizeOnlyWhenForced(false) {}
  LoopVectorizeOptions(bool InterleaveOnlyWhenForced,
                       bool VectorizeOnlyWhenForced)
      : InterleaveOnlyWhenForced(InterleaveOnlyWhenForced),
        VectorizeOnlyWhenForced(VectorizeOnlyWhenForced) {}

  LoopVectorizeOptions &setInterleaveOnlyWhenForced(bool Value) {
    InterleaveOnlyWhenForced = Value;
    return *this;
  }

  LoopVectorizeOptions &setVectorizeOnlyWhenForced(bool Value) {
    VectorizeOnlyWhenForced = Value;
    return *this;
  }
};

/// Storage for information about made changes.
struct LoopVectorizeResult {
  bool MadeAnyChange;
  bool MadeCFGChange;

  LoopVectorizeResult(bool MadeAnyChange, bool MadeCFGChange)
      : MadeAnyChange(MadeAnyChange), MadeCFGChange(MadeCFGChange) {}
};

/// The LoopVectorize Pass.
struct LoopVectorizePass : public llvm::PassInfoMixin<LoopVectorizePass> {
private:
  /// If false, consider all loops for interleaving.
  /// If true, only loops that explicitly request interleaving are considered.
  bool InterleaveOnlyWhenForced;

  /// If false, consider all loops for vectorization.
  /// If true, only loops that explicitly request vectorization are considered.
  bool VectorizeOnlyWhenForced;

public:
  LoopVectorizePass(LoopVectorizeOptions Opts = {});

  llvm::ScalarEvolution *SE;
  llvm::LoopInfo *LI;
  llvm::TargetTransformInfo *TTI;
  llvm::DominatorTree *DT;
  llvm::BlockFrequencyInfo *BFI;
  llvm::TargetLibraryInfo *TLI;
  llvm::DemandedBits *DB;
  llvm::AssumptionCache *AC;
  llvm::LoopAccessInfoManager *LAIs;
  llvm::OptimizationRemarkEmitter *ORE;
  llvm::ProfileSummaryInfo *PSI;

  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
  void printPipeline(llvm::raw_ostream &OS,
                     llvm::function_ref<llvm::StringRef(llvm::StringRef)> MapClassName2PassName);

  // Shim for old PM.
  LoopVectorizeResult runImpl(llvm::Function &F, llvm::ScalarEvolution &SE_,
                              llvm::LoopInfo &LI_, llvm::TargetTransformInfo &TTI_,
                              llvm::DominatorTree &DT_, llvm::BlockFrequencyInfo *BFI_,
                              llvm::TargetLibraryInfo *TLI_, llvm::DemandedBits &DB_,
                              llvm::AssumptionCache &AC_, llvm::LoopAccessInfoManager &LAIs_,
                              llvm::OptimizationRemarkEmitter &ORE_,
                              llvm::ProfileSummaryInfo *PSI_);

  bool processLoop(llvm::Loop *L);
};

} // end namespace smaug

#endif // LLVM_TRANSFORMS_VECTORIZE_LOOPVECTORIZE_H
