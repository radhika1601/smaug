#include "SecretAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

namespace smaug {

bool isRootObject(const Value *V) {
  if (isa<Argument>(V))
    return V->getType()->isPointerTy();
  if (isa<AllocaInst>(V))
    return true;
  if (const auto *CI = dyn_cast<CallInst>(V))
    if (const Function *Fn = CI->getCalledFunction())
      return Fn->getName() == "malloc" || Fn->getName() == "calloc";
  return false;
}

SecretAnalysis::SecretAnalysis(Function &F, const FunctionSpec &Spec) {
  run(F, Spec);
}

Value *SecretAnalysis::rootOf(const Value *Ptr) const {
  auto It = RootCache.find(Ptr);
  if (It != RootCache.end())
    return It->second;
  SmallVector<const Value *> Objs;
  getUnderlyingObjects(Ptr, Objs, nullptr, /*MaxLookup=*/0);
  Value *Root = nullptr;
  if (Objs.size() == 1 && isRootObject(Objs[0]))
    Root = const_cast<Value *>(Objs[0]);
  RootCache[Ptr] = Root;
  return Root;
}

bool SecretAnalysis::markSecret(const Value *V) {
  if (V->getType()->isPointerTy())
    return false;
  return Secret.insert(V).second;
}

bool SecretAnalysis::markRoot(const Value *Ptr) {
  Value *Root = rootOf(Ptr);
  // A secret store to an unknown root is reported by the lowering.
  return Root && SecretRoots.insert(Root);
}

static bool anySecretOperand(const Instruction &I,
                             const DenseSet<const Value *> &Secret) {
  for (const Value *Op : I.operands())
    if (Secret.count(Op))
      return true;
  return false;
}

void SecretAnalysis::run(Function &F, const FunctionSpec &Spec) {
  for (unsigned I = 0; I < F.arg_size(); ++I) {
    if (!Spec.Args[I].Secret)
      continue;
    Argument *A = F.getArg(I);
    if (A->getType()->isPointerTy())
      SecretRoots.insert(A);
    else
      Secret.insert(A);
  }

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : instructions(F)) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *Root = rootOf(LI->getPointerOperand());
        if ((Root && isSecretRoot(Root)) ||
            hasSecretOffset(LI->getPointerOperand()))
          Changed |= markSecret(LI);
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        if (isSecret(SI->getValueOperand()))
          Changed |= markRoot(SI->getPointerOperand());
      } else if (auto *MT = dyn_cast<MemTransferInst>(&I)) {
        Value *Src = rootOf(MT->getRawSource());
        if (Src && isSecretRoot(Src))
          Changed |= markRoot(MT->getRawDest());
      } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        if (hasSecretOffset(GEP->getPointerOperand()) ||
            anySecretOperand(*GEP, Secret))
          Changed |= SecretOffset.insert(GEP).second;
      } else if (I.getType()->isPointerTy()) {
        // Pointer phis and selects carry a secret offset from any input.
        if (isa<PHINode>(I) || isa<SelectInst>(I))
          for (const Value *Op : I.operands())
            if (Op->getType()->isPointerTy() && hasSecretOffset(Op))
              Changed |= SecretOffset.insert(&I).second;
      } else if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (isa<IntrinsicInst>(CB) && anySecretOperand(I, Secret))
          Changed |= markSecret(&I);
      } else if (isa<PtrToIntInst>(I) ||
                 (isa<ICmpInst>(I) &&
                  I.getOperand(0)->getType()->isPointerTy())) {
        // Addresses are public.
      } else if (!I.getType()->isVoidTy() && anySecretOperand(I, Secret)) {
        Changed |= markSecret(&I);
      }
    }
  }
}

} // namespace smaug
