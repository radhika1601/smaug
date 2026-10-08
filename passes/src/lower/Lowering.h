#pragma once
// Lowers one function to the runtime ABI.
//
// Representation:
// - A secret scalar lives in a slot, a one-element ABI buffer. Each static
//   SSA value gets its own slot, allocated at entry and freed at returns. A
//   non-phi use always reads the latest execution of its definition, so one
//   slot per value is enough; phis get parallel copies on their edges.
// - Secret memory is an ABI buffer per root. Pointer arithmetic stays public
//   IR, and an access through p becomes element (p - root) / element size of
//   the root's buffer. A secret malloc or alloca is replaced by its buffer,
//   which then serves as its own base address. A secret pointer argument is
//   imported into a buffer at entry and, if written, exported at returns.
// - A vector value inside a vectorized loop (see VectorLoops.h) has the
//   loop's trip count TC elements. A secret vector lives in an ABI buffer of
//   TC elements, allocated in the vector preheader and freed at the end of
//   the middle block. A public vector is a splat of a scalar (passed with
//   stride 0) or a copy of public memory (stride 1).
//
// Every instruction that touches secret data is lowered by a rule or
// reported as unsupported; nothing is skipped.

#include "Abi.h"
#include "Diagnostic.h"
#include "SecretAnalysis.h"
#include "SecretSpec.h"
#include "VectorLoops.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"

#include <string>
#include <vector>

namespace smaug {

class FunctionLowering {
public:
  // Regions are the canonicalized vector loops of F.
  FunctionLowering(llvm::Function &F, const FunctionSpec &Spec, Abi &A,
                   std::vector<Diagnostic> &Diags,
                   std::vector<VectorRegion> Regions);

  // Lowers the function. Problems are added to the diagnostics list; the
  // function is left unusable if there are any.
  void run();

  // An operand of an ABI call: a secret buffer, or a public array read with
  // the given stride (0 for a scalar or splat).
  struct Operand {
    llvm::Value *Ptr = nullptr;
    bool Secret = false;
    uint64_t Stride = 0;
  };


private:
  struct Root {
    llvm::Value *Base = nullptr;    // address that offsets are taken from
    llvm::Value *Storage = nullptr; // the ABI buffer
    llvm::Value *Len = nullptr;     // i64 element count
    unsigned W = 0;                 // element width in bits
    unsigned ElemBytes = 0;
    llvm::Argument *Arg = nullptr;  // set for argument roots
    bool Export = false;            // write back at returns
    bool FreeAtReturn = false;
  };

  // Setup.
  void splitPhiEdges();
  bool assignRootTypes();
  void createRoots();
  void importScalarArgs();

  // The rule table.
  void lower(llvm::Instruction &I);
  void lowerBinary(llvm::BinaryOperator &I);
  void lowerICmp(llvm::ICmpInst &I);
  void lowerFCmp(llvm::FCmpInst &I);
  void lowerSelect(llvm::SelectInst &I);
  void lowerCast(llvm::CastInst &I);
  void lowerIntrinsic(llvm::IntrinsicInst &I);
  void lowerLoad(llvm::LoadInst &I);
  void lowerStore(llvm::StoreInst &I);
  void lowerMemTransfer(llvm::MemTransferInst &I);
  void lowerMemSet(llvm::MemSetInst &I);
  void lowerCall(llvm::CallBase &I);
  void lowerReturn(llvm::ReturnInst &I);
  void finishPhis();

  // Vector rules.
  void lowerExtract(llvm::ExtractElementInst &I);
  void lowerSplat(llvm::Instruction &I, llvm::Value *Scalar);
  void lowerInsert(llvm::InsertElementInst &I);
  bool lowerPublicVector(llvm::Instruction &I);
  llvm::Value *publicElement(llvm::Value *V, llvm::Value *K,
                             llvm::IRBuilder<> &B);
  void lowerReduce(llvm::IntrinsicInst &I, unsigned Op);

  // Values.
  const VectorRegion *regionOf(llvm::Value *V);
  llvm::Value *count(llvm::Value *V, llvm::IRBuilder<> &B);
  llvm::Value *splatScalar(llvm::Value *V);
  llvm::Value *insertedVector(llvm::Value *V, llvm::IRBuilder<> &B);
  Operand operand(llvm::Value *V, llvm::IRBuilder<> &B);
  llvm::Value *secretOperand(llvm::Value *V, llvm::IRBuilder<> &B);
  llvm::Value *slot(llvm::Value *V);
  llvm::Value *newSlot(unsigned W, const llvm::Twine &Name);
  llvm::Value *publicTemp(llvm::Value *V, llvm::IRBuilder<> &B);
  void copyInto(llvm::Value *Dst, llvm::Value *V, unsigned W,
                llvm::IRBuilder<> &B);
  void callBinop(llvm::StringRef Name, llvm::Instruction &I, llvm::Value *A,
                 llvm::Value *B, unsigned W);

  // Memory.
  Root *rootFor(llvm::Value *Ptr, llvm::Instruction &I);
  llvm::Value *elementIndex(Root &R, llvm::Value *Ptr, llvm::IRBuilder<> &B);
  llvm::Value *elementPtr(Root &R, llvm::Value *Ptr, llvm::IRBuilder<> &B);
  llvm::Value *secretIndexSlot(Root &R, llvm::Value *Ptr,
                               llvm::Instruction &I, unsigned &IdxW);

  // Cleanup.
  void eraseLowered();

  void unsupported(const llvm::Instruction &I, const llvm::Twine &Why);

  llvm::Function &F;
  const FunctionSpec &Spec;
  Abi &A;
  std::vector<Diagnostic> &Diags;
  SecretAnalysis SA;  // built after the vector loops are canonicalized
  const llvm::DataLayout &DL;
  llvm::LLVMContext &Ctx;

  // Where slots and public temporaries are allocated: the start of the
  // entry block.
  llvm::Instruction *EntryPoint = nullptr;
  llvm::DenseMap<llvm::Value *, llvm::Value *> Slots;
  // Slots this function allocated, with their widths, freed at returns.
  llvm::SmallVector<std::pair<llvm::Value *, unsigned>> AllocatedSlots;
  llvm::MapVector<llvm::Value *, Root> Roots;
  std::vector<VectorRegion> Regions;
  llvm::DenseMap<const llvm::BasicBlock *, const VectorRegion *> RegionOf;
  // Per region: secret buffers (with widths) and public arrays to free.
  llvm::DenseMap<const VectorRegion *,
                 llvm::SmallVector<std::pair<llvm::Value *, unsigned>>>
      RegionBuffers;
  llvm::DenseMap<const VectorRegion *, llvm::SmallVector<llvm::Value *>>
      RegionArrays;
  // Public vectors copied from memory: value -> array.
  llvm::DenseMap<llvm::Value *, llvm::Value *> PublicArrays;
  llvm::SmallVector<llvm::PHINode *> SecretPhis;
  llvm::SmallVector<llvm::ReturnInst *> Returns;
  llvm::SetVector<llvm::Instruction *> Lowered;
};

// The width in bits of a secret value of type T (i1 -> 1, iN -> N,
// float -> 32), or 0 if T cannot hold secret data.
unsigned secretWidth(llvm::Type *T);

} // namespace smaug
