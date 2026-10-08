#pragma once
// Which values in a function are secret, and which memory holds secret data.
//
// Pointers are always public addresses. A pointer has a root, the object it
// points into (a pointer argument, malloc/calloc or alloca), and the root is
// secret or public. A pointer whose offset depends on a secret value has a
// secret offset; loads and stores through it are oblivious accesses.
//
// Rules, run to a fixpoint:
// - Secret arguments from the specification are secret scalars or roots.
// - A load from a secret root, or through a secret offset, is secret.
// - Storing a secret value makes the pointer's root secret.
// - memcpy/memmove from a secret root makes the destination root secret.
// - Any other non-pointer value is secret if a non-pointer operand is.
// - A GEP has a secret offset if an index is secret or its base has one.
// - Calls to non-intrinsics return public values.

#include "SecretSpec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Function.h"

namespace smaug {

class SecretAnalysis {
public:
  SecretAnalysis(llvm::Function &F, const FunctionSpec &Spec);

  // A non-pointer value that holds secret data.
  bool isSecret(const llvm::Value *V) const { return Secret.count(V); }
  // The unique object a pointer points into, or nullptr if there is none or
  // more than one.
  llvm::Value *rootOf(const llvm::Value *Ptr) const;
  bool isSecretRoot(const llvm::Value *Root) const {
    return SecretRoots.count(const_cast<llvm::Value *>(Root));
  }
  bool hasSecretOffset(const llvm::Value *Ptr) const {
    return SecretOffset.count(Ptr);
  }
  // Records that Ptr points into Root. Used when the lowering replaces a
  // root with its buffer, after resolving every pointer with rootOf.
  void setRoot(const llvm::Value *Ptr, llvm::Value *Root) {
    RootCache[Ptr] = Root;
  }
  // Secret roots in a deterministic order (arguments first, then by
  // position in the function).
  const llvm::SetVector<llvm::Value *> &secretRoots() const {
    return SecretRoots;
  }

private:
  void run(llvm::Function &F, const FunctionSpec &Spec);
  bool markSecret(const llvm::Value *V);
  bool markRoot(const llvm::Value *Ptr);

  llvm::DenseSet<const llvm::Value *> Secret;
  llvm::DenseSet<const llvm::Value *> SecretOffset;
  llvm::SetVector<llvm::Value *> SecretRoots;
  mutable llvm::DenseMap<const llvm::Value *, llvm::Value *> RootCache;
};

// Whether V is an object that can be a root: a pointer argument, a call to
// malloc or calloc, or an alloca.
bool isRootObject(const llvm::Value *V);

} // namespace smaug
