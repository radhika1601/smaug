#include "MPCLower.h"

#include "Abi.h"
#include "Lowering.h"
#include "SecretSpec.h"
#include "VectorLoops.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

extern cl::opt<std::string> MetadataFilePath;

namespace smaug {

namespace {

bool callsPrefix(Function &F, StringRef Prefix) {
  for (Instruction &I : instructions(F))
    if (auto *CB = dyn_cast<CallBase>(&I))
      if (Function *Callee = CB->getCalledFunction())
        if (Callee->getName().starts_with(Prefix))
          return true;
  return false;
}

// main must set up the protocol before anything is computed and finish it at
// the end. Benchmarks call MPC::setup themselves; add the calls if missing.
void addSetupFinish(Module &M) {
  Function *Main = M.getFunction("main");
  if (!Main || Main->isDeclaration())
    return;
  LLVMContext &Ctx = M.getContext();
  FunctionType *VoidFn = FunctionType::get(Type::getVoidTy(Ctx), false);
  if (!callsPrefix(*Main, "_ZN3MPC5setup")) {
    IRBuilder<> B(&*Main->getEntryBlock().getFirstInsertionPt());
    B.CreateCall(M.getOrInsertFunction("_ZN3MPC5setupEv", VoidFn));
  }
  if (!callsPrefix(*Main, "_ZN3MPC6finish")) {
    SmallVector<ReturnInst *> Rets;
    for (BasicBlock &BB : *Main)
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
        Rets.push_back(RI);
    for (ReturnInst *RI : Rets)
      IRBuilder<>(RI).CreateCall(M.getOrInsertFunction("_ZN3MPC6finishEv", VoidFn));
  }
}

// mpc-loop-reconstruct fills the storage of flattened phis with a call to the
// legacy runtime: MPC::store(dst, src, n, elemBytes, shared, consecutive)
// writes *src to dst[0..n). Expand it into an IR loop so that the fill is
// ordinary memory traffic. The shared flag describes the legacy runtime's
// representation and has no meaning before lowering.
void expandLegacyFill(Module &M) {
  Function *Fill = M.getFunction("_ZN3MPC5storeEPvS0_iibb");
  if (!Fill)
    return;
  LLVMContext &Ctx = M.getContext();
  for (User *U : make_early_inc_range(Fill->users())) {
    auto *CI = dyn_cast<CallInst>(U);
    if (!CI || CI->getCalledFunction() != Fill)
      report_fatal_error("mpc-lower: unexpected use of the legacy fill call",
                         false);
    auto *ElemBytes = dyn_cast<ConstantInt>(CI->getArgOperand(3));
    auto *Consecutive = dyn_cast<ConstantInt>(CI->getArgOperand(5));
    if (!ElemBytes || !Consecutive || !Consecutive->isZero())
      report_fatal_error("mpc-lower: legacy fill call with a non-constant "
                         "element size or consecutive values",
                         false);
    Type *T = IntegerType::get(Ctx, 8 * ElemBytes->getZExtValue());
    Value *Dst = CI->getArgOperand(0), *Src = CI->getArgOperand(1);

    // src is a one-element alloca written once just before the call. Use the
    // stored value directly.
    Value *V = nullptr;
    auto *Slot = dyn_cast<AllocaInst>(Src);
    StoreInst *SlotStore = nullptr;
    if (Slot && Slot->hasNUses(2))
      for (User *SU : Slot->users())
        if (auto *SI = dyn_cast<StoreInst>(SU))
          if (SI->getPointerOperand() == Slot &&
              SI->getValueOperand()->getType() == T &&
              SI->getParent() == CI->getParent() && SI->comesBefore(CI))
            SlotStore = SI;
    IRBuilder<> B(CI);
    if (SlotStore)
      V = SlotStore->getValueOperand();
    else
      V = B.CreateLoad(T, Src);
    Value *N = B.CreateZExt(CI->getArgOperand(2), B.getInt64Ty());

    BasicBlock *Before = CI->getParent();
    BasicBlock *After = SplitBlock(Before, CI);
    BasicBlock *Loop =
        BasicBlock::Create(Ctx, "smaug.fill", Before->getParent(), After);
    Before->getTerminator()->eraseFromParent();
    B.SetInsertPoint(Before);
    B.CreateCondBr(B.CreateICmpSGT(N, B.getInt64(0)), Loop, After);
    B.SetInsertPoint(Loop);
    PHINode *I = B.CreatePHI(B.getInt64Ty(), 2);
    I->addIncoming(B.getInt64(0), Before);
    B.CreateStore(V, B.CreateGEP(T, Dst, I));
    Value *Next = B.CreateAdd(I, B.getInt64(1));
    I->addIncoming(Next, Loop);
    B.CreateCondBr(B.CreateICmpSLT(Next, N), Loop, After);

    CI->eraseFromParent();
    if (SlotStore) {
      SlotStore->eraseFromParent();
      Slot->eraseFromParent();
    }
  }
  Fill->eraseFromParent();
}

// instcombine turns c ? a[i] : b[i] into a load through select(c, a, b)
// plus GEPs. With a secret c that address is secret. Access both pointers,
// each with the GEPs rebuilt on top, and select the value instead:
//   load (select c, p, q)     -> select c, (load p), (load q)
//   store v, (select c, p, q) -> store (select c, v, load p), p
//                                store (select c, load q, v), q
// q is loaded after the first store, so the result is right when p == q.
// Both pointers are dereferenced, so neither may be null.
void unfoldPointerSelects(Function &F) {
  // The access, the select under its pointer, and the GEPs in between from
  // the select up.
  struct Item {
    Instruction *I;
    SelectInst *S;
    SmallVector<GetElementPtrInst *> GEPs;
  };
  SmallVector<Item> Work;
  for (Instruction &I : instructions(F)) {
    Value *Ptr = getLoadStorePointerOperand(&I);
    if (!Ptr)
      continue;
    SmallVector<GetElementPtrInst *> GEPs;
    while (auto *G = dyn_cast<GetElementPtrInst>(Ptr)) {
      GEPs.push_back(G);
      Ptr = G->getPointerOperand();
    }
    auto *S = dyn_cast<SelectInst>(Ptr);
    if (!S || isa<ConstantPointerNull>(S->getTrueValue()) ||
        isa<ConstantPointerNull>(S->getFalseValue()) ||
        S->getCondition()->getType()->isVectorTy())
      continue;
    std::reverse(GEPs.begin(), GEPs.end());
    Work.push_back({&I, S, std::move(GEPs)});
  }
  for (Item &W : Work) {
    IRBuilder<> B(W.I);
    auto rebuild = [&](Value *Base) {
      for (GetElementPtrInst *G : W.GEPs) {
        SmallVector<Value *> Idx(G->indices());
        Base = B.CreateGEP(G->getSourceElementType(), Base, Idx, "",
                           G->isInBounds());
      }
      return Base;
    };
    Value *C = W.S->getCondition();
    Value *P = rebuild(W.S->getTrueValue()), *Q = rebuild(W.S->getFalseValue());
    if (auto *LI = dyn_cast<LoadInst>(W.I)) {
      Type *T = LI->getType();
      Value *V = B.CreateSelect(C, B.CreateLoad(T, P), B.CreateLoad(T, Q));
      LI->replaceAllUsesWith(V);
    } else {
      auto *SI = cast<StoreInst>(W.I);
      Value *V = SI->getValueOperand();
      Type *T = V->getType();
      B.CreateStore(B.CreateSelect(C, V, B.CreateLoad(T, P)), P);
      B.CreateStore(B.CreateSelect(C, B.CreateLoad(T, Q), V), Q);
    }
    W.I->eraseFromParent();
  }
  // The old address chains, if nothing else uses them. Accesses can share
  // them, so each instruction is erased once.
  SmallSetVector<Instruction *, 8> Old;
  for (Item &W : Work) {
    Old.insert(W.GEPs.begin(), W.GEPs.end());
    Old.insert(W.S);
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction *I : Old.takeVector())
      if (I->use_empty()) {
        I->eraseFromParent();
        Changed = true;
      } else {
        Old.insert(I);
      }
  }
}

std::string location(const Instruction *I) {
  if (!I)
    return "";
  if (const DILocation *Loc = I->getDebugLoc().get())
    return (Loc->getFilename() + ":" + Twine(Loc->getLine()) + ":" +
            Twine(Loc->getColumn()) + ": ")
        .str();
  return "";
}

} // namespace

PreservedAnalyses MPCLowerPass::run(Module &M, ModuleAnalysisManager &) {
  Expected<SecretSpec> Spec = SecretSpec::load(MetadataFilePath, M);
  if (!Spec)
    report_fatal_error(Twine("mpc-lower: ") + toString(Spec.takeError()),
                       false);
  addSetupFinish(M);
  expandLegacyFill(M);

  Abi A(M);
  std::vector<Diagnostic> Diags;
  for (auto &[Fn, FS] : Spec->Funcs) {
    if (!FS.hasSecretArgs())
      continue;
    Function &F = const_cast<Function &>(*Fn);
    size_t Before = Diags.size();
    unfoldPointerSelects(F);
    std::vector<VectorRegion> Regions = canonicalizeVectorLoops(F, Diags);
    if (Diags.size() != Before)
      continue;
    FunctionLowering(F, FS, A, Diags, std::move(Regions)).run();
  }

  if (!Diags.empty()) {
    for (const Diagnostic &D : Diags) {
      errs() << "mpc-lower: error: " << location(D.I);
      if (D.I)
        errs() << "in " << D.I->getFunction()->getName() << ": ";
      errs() << D.Message;
      if (D.I)
        errs() << ":" << *D.I;
      errs() << "\n";
    }
    report_fatal_error("mpc-lower: " + Twine(Diags.size()) +
                           " unsupported construct(s)",
                       false);
  }
  return PreservedAnalyses::none();
}

} // namespace smaug
