#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

#include "MPCHierarchical.h"
#include "MPCLink.h"
#include "../include/MPCLoopFlatten.h"
#include "MPCLoopMemAlign.h"
#include "MPCLoopReconstruct.h"
#include "MPCRemoveOps.h"
#include "Options.h"
#include "Metadata.h"
#include "MetadataHelper.h"
#include "ParallelizeReductions.h"
#include "TopologicalTraversal.h"
#include "VectorMPCLink.h"
#include "VectorizeHelper.h"
#include "Vectorize/LoopVectorize.h"
#include "lower/MPCLower.h"

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
    if (Name == "mpc-lower") {
        MPM.addPass(smaug::MPCLowerPass());
        return true;
    }
    if (Name == "mpc-narrow-bool") {
        MPM.addPass(smaug::MPCNarrowBoolPass());
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

// The stages shared by every pipeline, up to the point where the code is
// lowered to MPC calls. flatten adds mpc-loop-flatten and
// mpc-loop-reconstruct around vec-help; vectorize adds mpc-loop-vectorize.
static std::string buildPrefix(bool flatten, bool vectorize) {
    std::string p =
        MpcO1 + ",loop-flatten,"
        "instcombine,dce,loop-simplify,unify-loop-exits,lcssa,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "loop-simplify,"
        "secshared-metadata,"
        "mpc-hierarchical,"
        + MpcO3 + ","
        "secshared-metadata,";
    if (flatten)
        p += "function(mpc-loop-flatten),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
             "secshared-metadata,";
    p += "function(vec-help),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
         "secshared-metadata,";
    if (flatten)
        p += "function(mpc-loop-reconstruct),instcombine,dce,loop-deletion,simplifycfg,loop-simplify,"
             "secshared-metadata,";
    p += "function(mpc-mem-align),instcombine,dce,loop-deletion,"
         "function(simplifycfg,sroa,early-cse,gvn-hoist,memcpyopt,sccp,bdce,"
         "loop-mssa(licm,loop-rotate),mem2reg,lcssa,indvars,loop-idiom,"
         "loop-simplifycfg,loop-sink,loop-load-elim,loop-interchange,"
         "loop-reduce,loop-unroll),"
         "secshared-metadata,";
    if (vectorize)
        p += "function(mpc-loop-vectorize),";
    p += "function(instcombine<max-iterations=10;>,dce),"
         "secshared-metadata";
    return p;
}

// The stage that lowers the code to MPC runtime calls, chosen by
// --mpc-lowering.
static std::string buildLinkStage() {
    if (Lowering == MPCLowering::New)
        return "mpc-narrow-bool,mpc-lower,default<O1>";
    // No optimization between vec-mpc-link and mpc-link. vec-mpc-link moves
    // vector loops onto new MPC buffers, and the scalar loops that fill those
    // buffers still write the originals until mpc-link redirects them. An
    // optimization pass in between deletes those scalar loops as dead stores.
    return "vec-mpc-link,"
           "secshared-metadata,"
           "mpc-remove-ops,dce,"
           "secshared-metadata,"
           "mpc-link,"
           "default<O1>";
}

static std::optional<std::string> namedPipeline(StringRef Name) {
    if (Name == "smaug-pipeline")
        return buildPrefix(false, true) + "," + buildLinkStage();
    if (Name == "smaug-pipeline-novec")
        return buildPrefix(false, false) + "," + buildLinkStage();
    if (Name == "loop-flatten-pipeline")
        return buildPrefix(true, true) + "," + buildLinkStage();
    if (Name == "loop-flatten-pipeline-novec")
        return buildPrefix(true, false) + "," + buildLinkStage();
    if (Name == "no-link-pipeline")
        return buildPrefix(false, true);
    if (Name == "no-link-loop-flatten-pipeline")
        return buildPrefix(true, true);
    if (Name == "smaug-link")
        return buildLinkStage();
    return std::nullopt;
}

bool registerNamedPipelines(StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
    std::optional<std::string> pipeline = namedPipeline(Name);
    if (!pipeline)
        return false;

    PassBuilder LocalPB(/*TM=*/nullptr);
    LocalPB.registerPipelineParsingCallback(registerModulePasses);
    LocalPB.registerPipelineParsingCallback(registerFunctionPasses);
    if (auto Err = LocalPB.parsePassPipeline(MPM, *pipeline)) {
        logAllUnhandledErrors(std::move(Err), errs(), Name + ": ");
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
                PB.registerPipelineParsingCallback(registerNamedPipelines);
            }};
}
