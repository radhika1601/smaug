#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "MPCHierarchical.h"
#include "MPCLink.h"
#include "../include/MPCLoopFlatten.h"
#include "MPCLoopMemAlign.h"
#include "MPCLoopReconstruct.h"
#include "MPCRemoveOps.h"
#include "Metadata.h"
#include "MetadataHelper.h"
#include "ParallelizeReductions.h"
#include "TopologicalTraversal.h"
#include "VectorMPCLink.h"
#include "VectorizeHelper.h"
#include "Vectorize/LoopVectorize.h"

using namespace llvm;

namespace {
bool registerModulePasses(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name == "secshared-metadata") {
        MPM.addPass(SecSharedMetadataPass());
        return true;
    }
    if (Name == "secshared-metadata-helper") {
        MPM.addPass(SecSharedMetadataHelperPass());
        return true;
    }
    if (Name == "mpc-hierarchical") {
        MPM.addPass(createModuleToFunctionPassAdaptor(MPCHierarchicalPass()));
        return true;
    }
    if (Name == "topological-traversal") {
        MPM.addPass(createModuleToFunctionPassAdaptor(TopologicalTraversal()));
        return true;
    }
    if (Name == "mpc-remove-ops") {
        MPM.addPass(MPCRemoveOpsPass());
        return true;
    }
    if (Name == "mpc-link") {
        MPM.addPass(MPCLinkPass());
        return true;
    }
    if (Name == "vec-mpc-link") {
        MPM.addPass(VectorMPCLinkPass());
        return true;
    }
    return false;
}

bool registerFunctionPasses(StringRef Name, FunctionPassManager& FPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name == "mpc-hierarchical") {
        FPM.addPass(MPCHierarchicalPass());
        return true;
    }
    if (Name == "topological-traversal") {
        FPM.addPass(TopologicalTraversal());
        return true;
    }
    if (Name == "mpc-loop-flatten") {
        FPM.addPass(MPCLoopFlattenPass());
        return true;
    }
    if (Name == "mpc-mem-align") {
        FPM.addPass(MPCLoopMemAlignPass());
        return true;
    }
    if (Name == "mpc-loop-reconstruct") {
        FPM.addPass(MPCLoopReconstructPass());
        return true;
    }
    if (Name == "vec-help") {
        FPM.addPass(VectorizeHelperPass());
        return true;
    }
    if (Name == "parallelize-reductions") {
        FPM.addPass(ParallelizeReductionsPass());
        return true;
    }
    if (Name == "mpc-loop-vectorize") {
        FPM.addPass(smaug::LoopVectorizePass());
        return true;
    }
    return false;
}

// Explicit O1 pipeline with all vectorization passes removed:
//   loop-distribute, inject-tli-mappings, loop-vectorize, vector-combine
static const std::string MpcO1 =
    "annotation2metadata,forceattrs,inferattrs,coro-early,"
    "function<eager-inv>("
      "lower-expect,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;no-switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
      "sroa<modify-cfg>,early-cse<>),"
    "openmp-opt,ipsccp,called-value-propagation,globalopt,"
    "function<eager-inv>("
      "mem2reg,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>),"
    "always-inline,require<globals-aa>,function(invalidate<aa>),require<profile-summary>,"
    "cgscc(devirt<4>("
      "inline,function-attrs<skip-non-recursive-function-attrs>,"
      "function<eager-inv;no-rerun>("
        "sroa<modify-cfg>,early-cse<memssa>,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "libcalls-shrinkwrap,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "reassociate,"
        "loop-mssa(loop-instsimplify,loop-simplifycfg,licm<no-allowspeculation>,loop-rotate<header-duplication;no-prepare-for-lto>,licm<allowspeculation>,simple-loop-unswitch<no-nontrivial;trivial>),"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "loop(loop-idiom,indvars,loop-deletion,loop-unroll-full),"
        "sroa<modify-cfg>,memcpyopt,sccp,bdce,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "coro-elide,adce,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>),"
      "function-attrs,function(require<should-not-run-function-passes>),coro-split)),"
    "deadargelim,coro-cleanup,globalopt,globaldce,elim-avail-extern,"
    "rpo-function-attrs,recompute-globalsaa,"
    "function<eager-inv>("
      "float2int,lower-constant-intrinsics,"
      "loop(loop-rotate<header-duplication;no-prepare-for-lto>,loop-deletion),"
      "infer-alignment,loop-load-elim,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "simplifycfg<bonus-inst-threshold=1;forward-switch-cond;switch-range-to-icmp;switch-to-lookup;no-keep-loops;hoist-common-insts;sink-common-insts;speculate-blocks;simplify-cond-branch>,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "loop-unroll<O1>,transform-warning,sroa<preserve-cfg>,infer-alignment,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "loop-mssa(licm<allowspeculation>),alignment-from-assumptions,"
      "loop-sink,instsimplify,div-rem-pairs,tailcallelim,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>),"
    "globaldce,constmerge,cg-profile,rel-lookup-table-converter,"
    "function(annotation-remarks)";

// Explicit O3 pipeline with all vectorization passes removed:
//   loop-distribute, inject-tli-mappings, loop-vectorize, slp-vectorizer, vector-combine
static const std::string MpcO3 =
    "annotation2metadata,forceattrs,inferattrs,coro-early,"
    "function<eager-inv>("
      "lower-expect,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;no-switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
      "sroa<modify-cfg>,early-cse<>,callsite-splitting),"
    "openmp-opt,ipsccp,called-value-propagation,globalopt,"
    "function<eager-inv>("
      "mem2reg,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>),"
    "always-inline,require<globals-aa>,function(invalidate<aa>),require<profile-summary>,"
    "cgscc(devirt<4>("
      "inline,function-attrs<skip-non-recursive-function-attrs>,argpromotion,openmp-opt-cgscc,"
      "function<eager-inv;no-rerun>("
        "sroa<modify-cfg>,early-cse<memssa>,"
        "speculative-execution<only-if-divergent-target>,jump-threading,correlated-propagation,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "aggressive-instcombine,libcalls-shrinkwrap,tailcallelim,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "reassociate,constraint-elimination,"
        "loop-mssa(loop-instsimplify,loop-simplifycfg,licm<no-allowspeculation>,loop-rotate<header-duplication;no-prepare-for-lto>,licm<allowspeculation>,simple-loop-unswitch<nontrivial;trivial>),"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "loop(loop-idiom,indvars,loop-deletion,loop-unroll-full),"
        "sroa<modify-cfg>,mldst-motion<no-split-footer-bb>,gvn<>,sccp,bdce,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
        "jump-threading,correlated-propagation,adce,memcpyopt,dse,move-auto-init,"
        "loop-mssa(licm<allowspeculation>),coro-elide,"
        "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;hoist-common-insts;sink-common-insts;speculate-blocks;simplify-cond-branch>,"
        "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>),"
      "function-attrs,function(require<should-not-run-function-passes>),coro-split)),"
    "deadargelim,coro-cleanup,globalopt,globaldce,elim-avail-extern,"
    "rpo-function-attrs,recompute-globalsaa,"
    "function<eager-inv>("
      "float2int,lower-constant-intrinsics,chr,"
      "loop(loop-rotate<header-duplication;no-prepare-for-lto>,loop-deletion),"
      "infer-alignment,loop-load-elim,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "simplifycfg<bonus-inst-threshold=1;forward-switch-cond;switch-range-to-icmp;switch-to-lookup;no-keep-loops;hoist-common-insts;sink-common-insts;speculate-blocks;simplify-cond-branch>,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "loop-unroll<O3>,transform-warning,sroa<preserve-cfg>,infer-alignment,"
      "instcombine<max-iterations=1;no-use-loop-info;no-verify-fixpoint>,"
      "loop-mssa(licm<allowspeculation>),alignment-from-assumptions,"
      "loop-sink,instsimplify,div-rem-pairs,tailcallelim,"
      "simplifycfg<bonus-inst-threshold=1;no-forward-switch-cond;switch-range-to-icmp;no-switch-to-lookup;keep-loops;no-hoist-common-insts;no-sink-common-insts;speculate-blocks;simplify-cond-branch>),"
    "globaldce,constmerge,cg-profile,rel-lookup-table-converter,"
    "function(annotation-remarks)";

static std::string buildSmaugPipeline() {
    return
        MpcO1 + ",loop-flatten,"
        "instcombine,dce,loop-simplify,unify-loop-exits,lcssa,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "loop-simplify,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "secshared-metadata,"
        "function(vec-help),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-mem-align),instcombine,dce,loop-deletion,"
        "function(simplifycfg,sroa,early-cse,gvn-hoist,memcpyopt,sccp,bdce,"
        "loop-mssa(licm,loop-rotate),mem2reg,lcssa,indvars,loop-idiom,"
        "loop-simplifycfg,loop-sink,loop-load-elim,loop-interchange,"
        "loop-reduce,loop-unroll),"
        "secshared-metadata,"
        "function(mpc-loop-vectorize),"
        "function(instcombine<max-iterations=10;>,dce),"
        "secshared-metadata,"
        "vec-mpc-link,"
        "default<O1>,"
        "secshared-metadata,"
        "mpc-remove-ops,dce,"
        "secshared-metadata,"
        "mpc-link,"
        "default<O1>";
}

static std::string buildNoLinkPipeline() {
    return
        MpcO1 + ",loop-flatten,"
        "instcombine,dce,loop-simplify,unify-loop-exits,lcssa,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "loop-simplify,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "secshared-metadata,"
        "function(vec-help),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-mem-align),instcombine,dce,loop-deletion,"
        "function(simplifycfg,sroa,early-cse,gvn-hoist,memcpyopt,sccp,bdce,"
        "loop-mssa(licm,loop-rotate),mem2reg,lcssa,indvars,loop-idiom,"
        "loop-simplifycfg,loop-sink,loop-load-elim,loop-interchange,"
        "loop-reduce,loop-unroll),"
        "secshared-metadata,"
        "function(mpc-loop-vectorize),"
        "function(instcombine<max-iterations=10;>,dce),"
        "secshared-metadata";
}

bool registerSmaugPipeline(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name != "smaug-pipeline")
        return false;

    PassBuilder LocalPB(/*TM=*/nullptr);
    LocalPB.registerPipelineParsingCallback(registerModulePasses);
    LocalPB.registerPipelineParsingCallback(registerFunctionPasses);

    std::string pipeline = buildSmaugPipeline();
    if (auto Err = LocalPB.parsePassPipeline(MPM, pipeline)) {
        logAllUnhandledErrors(std::move(Err), errs(), "smaug-pipeline: ");
        return false;
    }
    return true;
}

bool registerNoLinkPipeline(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name != "no-link-pipeline")
        return false;

    PassBuilder LocalPB(/*TM=*/nullptr);
    LocalPB.registerPipelineParsingCallback(registerModulePasses);
    LocalPB.registerPipelineParsingCallback(registerFunctionPasses);

    std::string pipeline = buildNoLinkPipeline();
    if (auto Err = LocalPB.parsePassPipeline(MPM, pipeline)) {
        logAllUnhandledErrors(std::move(Err), errs(), "no-link-pipeline: ");
        return false;
    }
    return true;
}

static std::string buildNoLinkLoopFlattenPipeline() {
    return
        MpcO1 + ",loop-flatten,"
        "instcombine,dce,loop-simplify,unify-loop-exits,lcssa,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "loop-simplify,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "secshared-metadata,"
        "function(mpc-loop-flatten),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(vec-help),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-loop-reconstruct),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-mem-align),instcombine,dce,loop-deletion,"
        "function(simplifycfg,sroa,early-cse,gvn-hoist,memcpyopt,sccp,bdce,"
        "loop-mssa(licm,loop-rotate),mem2reg,lcssa,indvars,loop-idiom,"
        "loop-simplifycfg,loop-sink,loop-load-elim,loop-interchange,"
        "loop-reduce,loop-unroll),"
        "secshared-metadata,"
        "function(mpc-loop-vectorize),"
        "function(instcombine<max-iterations=10;>,dce),"
        "secshared-metadata";
}

// smaug-pipeline with mpc-loop-flatten and mpc-loop-reconstruct added around
// vec-help, matching the with-loop-flatten target in the legacy Makefile.
static std::string buildLoopFlattenPipeline() {
    return
        MpcO1 + ",loop-flatten,"
        "instcombine,dce,loop-simplify,unify-loop-exits,lcssa,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "loop-simplify,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "secshared-metadata,"
        "function(mpc-loop-flatten),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(vec-help),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-loop-reconstruct),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
        "secshared-metadata,"
        "function(mpc-mem-align),instcombine,dce,loop-deletion,"
        "function(simplifycfg,sroa,early-cse,gvn-hoist,memcpyopt,sccp,bdce,"
        "loop-mssa(licm,loop-rotate),mem2reg,lcssa,indvars,loop-idiom,"
        "loop-simplifycfg,loop-sink,loop-load-elim,loop-interchange,"
        "loop-reduce,loop-unroll),"
        "secshared-metadata,"
        "function(mpc-loop-vectorize),"
        "function(instcombine<max-iterations=10;>,dce),"
        "secshared-metadata,"
        "vec-mpc-link,"
        "default<O1>,"
        "secshared-metadata,"
        "mpc-remove-ops,dce,"
        "secshared-metadata,"
        "mpc-link,"
        "default<O1>";
}

bool registerNoLinkLoopFlattenPipeline(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name != "no-link-loop-flatten-pipeline")
        return false;

    PassBuilder LocalPB(/*TM=*/nullptr);
    LocalPB.registerPipelineParsingCallback(registerModulePasses);
    LocalPB.registerPipelineParsingCallback(registerFunctionPasses);

    std::string pipeline = buildNoLinkLoopFlattenPipeline();
    if (auto Err = LocalPB.parsePassPipeline(MPM, pipeline)) {
        logAllUnhandledErrors(std::move(Err), errs(), "no-link-loop-flatten-pipeline: ");
        return false;
    }
    return true;
}

bool registerLoopFlattenPipeline(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    if (Name != "loop-flatten-pipeline")
        return false;

    PassBuilder LocalPB(/*TM=*/nullptr);
    LocalPB.registerPipelineParsingCallback(registerModulePasses);
    LocalPB.registerPipelineParsingCallback(registerFunctionPasses);

    std::string pipeline = buildLoopFlattenPipeline();
    if (auto Err = LocalPB.parsePassPipeline(MPM, pipeline)) {
        logAllUnhandledErrors(std::move(Err), errs(), "loop-flatten-pipeline: ");
        return false;
    }
    return true;
}
}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "mpc", LLVM_VERSION_STRING, [](PassBuilder& PB) {
                PB.registerAnalysisRegistrationCallback([](FunctionAnalysisManager &FAM) {
                    FAM.registerPass([&] { return smaug::ShouldRunExtraVectorPasses(); });
                });
                PB.registerPipelineParsingCallback(registerModulePasses);
                PB.registerPipelineParsingCallback(registerFunctionPasses);
                PB.registerPipelineParsingCallback(registerSmaugPipeline);
                PB.registerPipelineParsingCallback(registerNoLinkPipeline);
                PB.registerPipelineParsingCallback(registerNoLinkLoopFlattenPipeline);
                PB.registerPipelineParsingCallback(registerLoopFlattenPipeline);
            }};
}
