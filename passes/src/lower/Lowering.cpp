#include "Lowering.h"

#include "mpc/smaug_abi.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace smaug {

unsigned secretWidth(Type *T) {
  if (auto *VT = dyn_cast<VectorType>(T))
    T = VT->getElementType();
  if (T->isIntegerTy()) {
    unsigned W = T->getIntegerBitWidth();
    return W == 1 || W == 8 || W == 16 || W == 32 || W == 64 ? W : 0;
  }
  if (T->isFloatTy())
    return 32;
  return 0;
}

static unsigned elemBytes(unsigned W) { return W == 1 ? 1 : W / 8; }

static bool isVector(const Value *V) {
  return isa<ScalableVectorType>(V->getType());
}

static bool touchesVector(const Instruction &I) {
  if (isVector(&I))
    return true;
  for (const Value *Op : I.operands())
    if (isVector(Op))
      return true;
  return false;
}

FunctionLowering::FunctionLowering(Function &F, const FunctionSpec &Spec,
                                   Abi &A, std::vector<Diagnostic> &Diags,
                                   std::vector<VectorRegion> Rs)
    : F(F), Spec(Spec), A(A), Diags(Diags), SA(F, Spec),
      DL(F.getParent()->getDataLayout()), Ctx(F.getContext()),
      Regions(std::move(Rs)) {
  for (const VectorRegion &R : Regions)
    for (BasicBlock *BB : {R.Setup, R.Preheader, R.Body, R.Middle})
      RegionOf[BB] = &R;
}

void FunctionLowering::unsupported(const Instruction &I, const Twine &Why) {
  Diags.push_back({&I, Why.str()});
}

// --- Setup ------------------------------------------------------------------

void FunctionLowering::splitPhiEdges() {
  SmallVector<std::pair<Instruction *, unsigned>> Edges;
  for (BasicBlock &BB : F) {
    bool HasSecretPhi = false;
    for (PHINode &PN : BB.phis())
      HasSecretPhi |= SA.isSecret(&PN);
    if (!HasSecretPhi)
      continue;
    for (BasicBlock *P : predecessors(&BB)) {
      Instruction *TI = P->getTerminator();
      for (unsigned S = 0; S < TI->getNumSuccessors(); ++S)
        if (TI->getSuccessor(S) == &BB && isCriticalEdge(TI, S))
          Edges.push_back({TI, S});
    }
  }
  for (auto [TI, S] : Edges)
    SplitCriticalEdge(TI, S);
}

bool FunctionLowering::assignRootTypes() {
  // The element width of each secret root: from the specification for
  // arguments, otherwise from the accesses, shared across memcpy partners.
  DenseMap<Value *, unsigned> Width;
  for (unsigned I = 0; I < F.arg_size(); ++I)
    if (SA.isSecretRoot(F.getArg(I)))
      Width[F.getArg(I)] = *Spec.Args[I].Bits;

  bool Ok = true;
  auto note = [&](Value *RootV, Type *T, Instruction &At) {
    if (!RootV || !SA.isSecretRoot(RootV))
      return;
    unsigned W = secretWidth(T);
    if (W == 0) {
      unsupported(At, "secret memory accessed with an unsupported type");
      Ok = false;
      return;
    }
    auto [It, New] = Width.insert({RootV, W});
    if (!New && It->second != W) {
      unsupported(At, "secret memory accessed as i" + Twine(W) +
                          " but elsewhere as i" + Twine(It->second));
      Ok = false;
    }
  };
  for (Instruction &I : instructions(F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      note(SA.rootOf(LI->getPointerOperand()), LI->getType(), I);
    else if (auto *SI = dyn_cast<StoreInst>(&I))
      note(SA.rootOf(SI->getPointerOperand()), SI->getValueOperand()->getType(),
           I);
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : instructions(F)) {
      auto *MT = dyn_cast<MemTransferInst>(&I);
      if (!MT)
        continue;
      Value *D = SA.rootOf(MT->getRawDest()), *S = SA.rootOf(MT->getRawSource());
      if (!D || !S || !SA.isSecretRoot(D) || !SA.isSecretRoot(S))
        continue;
      auto DI = Width.find(D), SI = Width.find(S);
      if (DI != Width.end() && SI == Width.end()) {
        Width[S] = DI->second;
        Changed = true;
      } else if (SI != Width.end() && DI == Width.end()) {
        Width[D] = SI->second;
        Changed = true;
      } else if (DI != Width.end() && DI->second != SI->second) {
        unsupported(I, "memcpy between secret buffers of different widths");
        Ok = false;
      }
    }
  }

  for (Value *RootV : SA.secretRoots()) {
    // An argument the function never uses may be passed as poison.
    if (isa<Argument>(RootV) && RootV->use_empty())
      continue;
    auto It = Width.find(RootV);
    if (It == Width.end()) {
      if (auto *RI = dyn_cast<Instruction>(RootV))
        unsupported(*RI, "cannot determine the element type of this secret "
                         "buffer");
      Ok = false;
      continue;
    }
    Root R;
    R.W = It->second;
    R.ElemBytes = elemBytes(R.W);
    Roots.insert({RootV, R});
  }
  return Ok;
}

void FunctionLowering::createRoots() {
  // Resolve every pointer before any root is replaced by its buffer.
  for (Instruction &I : instructions(F))
    for (Value *Op : I.operands())
      if (Op->getType()->isPointerTy())
        SA.rootOf(Op);

  Type *I32 = Type::getInt32Ty(Ctx), *I64 = Type::getInt64Ty(Ctx);
  for (auto &[RootV, R] : Roots) {
    if (auto *Arg = dyn_cast<Argument>(RootV)) {
      IRBuilder<> B(EntryPoint);
      const ArgSpec &AS = Spec.Args[Arg->getArgNo()];
      R.Arg = Arg;
      R.Base = Arg;
      R.Len = B.CreateZExtOrTrunc(F.getArg(*AS.LenArg), I64);
      R.Storage = B.CreateCall(A.get("smaug_alloc"),
                               {ConstantInt::get(I32, R.W), R.Len},
                               Arg->getName() + ".mpc");
      B.CreateCall(A.get("smaug_import"),
                   {ConstantInt::get(I32, R.W), R.Storage, Arg, R.Len});
      R.Export = AS.Write;
      R.FreeAtReturn = true;
      continue;
    }
    auto *I = cast<Instruction>(RootV);
    IRBuilder<> B(I);
    Value *Bytes;
    if (auto *AI = dyn_cast<AllocaInst>(I)) {
      Value *Count = B.CreateZExtOrTrunc(AI->getArraySize(), I64);
      Bytes = B.CreateMul(
          Count, ConstantInt::get(I64, DL.getTypeAllocSize(AI->getAllocatedType())));
      R.FreeAtReturn = true;
    } else {
      auto *CI = cast<CallInst>(I);
      Bytes = B.CreateZExtOrTrunc(CI->getArgOperand(0), I64);
      if (CI->getCalledFunction()->getName() == "calloc")
        Bytes = B.CreateMul(Bytes, B.CreateZExtOrTrunc(CI->getArgOperand(1), I64));
    }
    R.Len = B.CreateUDiv(Bytes, ConstantInt::get(I64, R.ElemBytes));
    R.Storage = B.CreateCall(A.get("smaug_alloc"),
                             {ConstantInt::get(I32, R.W), R.Len},
                             I->getName() + ".mpc");
    R.Base = R.Storage;
    SA.setRoot(R.Storage, RootV);
    I->replaceAllUsesWith(R.Storage);
    Lowered.insert(I);
  }
}

void FunctionLowering::importScalarArgs() {
  for (Argument &Arg : F.args()) {
    if (!SA.isSecret(&Arg) || Arg.use_empty())
      continue;
    unsigned W = secretWidth(Arg.getType());
    if (!W || isVector(&Arg)) {
      Diags.push_back({nullptr, "in " + F.getName().str() + ": secret argument " +
                                    std::to_string(Arg.getArgNo()) +
                                    " has an unsupported type"});
      continue;
    }
    IRBuilder<> B(EntryPoint);
    Value *Tmp = B.CreateAlloca(Arg.getType());
    B.CreateStore(&Arg, Tmp);
    B.CreateCall(A.get("smaug_import"),
                 {B.getInt32(W), slot(&Arg), Tmp, B.getInt64(1)});
  }
}

// --- Values -----------------------------------------------------------------

const VectorRegion *FunctionLowering::regionOf(Value *V) {
  if (auto *I = dyn_cast<Instruction>(V)) {
    auto It = RegionOf.find(I->getParent());
    if (It != RegionOf.end())
      return It->second;
  }
  return nullptr;
}

// The number of elements an instruction works on: 1 for scalar code, the
// trip count of its vector loop for vector code.
Value *FunctionLowering::count(Value *V, IRBuilder<> &B) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I || !touchesVector(*I))
    return B.getInt64(1);
  const VectorRegion *R = regionOf(I);
  if (!R) {
    unsupported(*I, "vector code outside a vectorized loop");
    return B.getInt64(1);
  }
  return R->TripCount;
}

Value *FunctionLowering::newSlot(unsigned W, const Twine &Name) {
  IRBuilder<> B(EntryPoint);
  Value *S = B.CreateCall(A.get("smaug_alloc"), {B.getInt32(W), B.getInt64(1)},
                          Name);
  AllocatedSlots.push_back({S, W});
  return S;
}

// The secret storage of V: a slot for a scalar, a buffer of the trip count
// for a vector.
Value *FunctionLowering::slot(Value *V) {
  auto It = Slots.find(V);
  if (It != Slots.end())
    return It->second;
  unsigned W = secretWidth(V->getType());
  auto *I = dyn_cast<Instruction>(V);
  if (!W) {
    if (I)
      unsupported(*I, "secret value of an unsupported type");
    W = 64; // keep going so that all problems are reported
  }
  Value *S;
  if (isVector(V)) {
    const VectorRegion *R = I ? regionOf(I) : nullptr;
    if (!R) {
      if (I)
        unsupported(*I, "vector value outside a vectorized loop");
      return newSlot(W, "bad");
    }
    IRBuilder<> B(R->Setup->getTerminator());
    S = B.CreateCall(A.get("smaug_alloc"), {B.getInt32(W), R->TripCount},
                     V->getName() + ".v");
    RegionBuffers[R].push_back({S, W});
  } else {
    S = newSlot(W, V->getName() + ".s");
  }
  Slots[V] = S;
  return S;
}

Value *FunctionLowering::publicTemp(Value *V, IRBuilder<> &B) {
  IRBuilder<> EB(EntryPoint);
  Value *Tmp = EB.CreateAlloca(V->getType());
  B.CreateStore(V, Tmp);
  return Tmp;
}

// The scalar of a public splat vector, or nullptr.
Value *FunctionLowering::splatScalar(Value *V) {
  if (auto *C = dyn_cast<Constant>(V))
    return C->getSplatValue();
  using namespace PatternMatch;
  Value *X;
  if (match(V, m_Shuffle(m_InsertElt(m_Value(), m_Value(X), m_Zero()),
                         m_Value(), m_ZeroMask())))
    return X;
  return nullptr;
}

FunctionLowering::Operand FunctionLowering::operand(Value *V, IRBuilder<> &B) {
  if (SA.isSecret(V))
    return {slot(V), true, 0};
  if (!isVector(V))
    return {publicTemp(V, B), false, 0};
  if (Value *S = splatScalar(V))
    return {publicTemp(S, B), false, 0};
  auto It = PublicArrays.find(V);
  if (It != PublicArrays.end())
    return {It->second, false, 1};
  if (Value *Buf = insertedVector(V, B))
    return {Buf, true, 0};
  if (auto *I = dyn_cast<Instruction>(V))
    unsupported(*I, "public vector value with no lowering");
  else
    unsupported(*B.GetInsertPoint(), "public vector constant with no lowering");
  return {Constant::getNullValue(B.getPtrTy()), false, 0};
}

// A public vector built by insertelement into a splat, as a reduction's start
// value is: insertelement(splat(identity), start, 0). Sharing public values
// costs nothing, so it becomes a secret buffer holding the splat with the
// inserted lanes set.
Value *FunctionLowering::insertedVector(Value *V, IRBuilder<> &B) {
  Value *Base, *Elt, *Idx;
  if (auto *IE = dyn_cast<InsertElementInst>(V)) {
    Base = IE->getOperand(0), Elt = IE->getOperand(1), Idx = IE->getOperand(2);
  } else if (auto *CE = dyn_cast<ConstantExpr>(V);
             CE && CE->getOpcode() == Instruction::InsertElement) {
    Base = CE->getOperand(0), Elt = CE->getOperand(1), Idx = CE->getOperand(2);
  } else {
    return nullptr;
  }
  if (!isa<ConstantInt>(Idx) || SA.isSecret(Elt))
    return nullptr;
  const VectorRegion *R = regionOf(&*B.GetInsertPoint());
  if (!R)
    return nullptr;
  unsigned W = secretWidth(V->getType());
  Value *Buf;
  if (Value *S = splatScalar(Base)) {
    IRBuilder<> SB(R->Setup->getTerminator());
    Buf = SB.CreateCall(A.get("smaug_alloc"), {SB.getInt32(W), R->TripCount},
                        "ins.v");
    RegionBuffers[R].push_back({Buf, W});
    B.CreateCall(A.get("smaug_share_public"),
                 {B.getInt32(W), Buf, publicTemp(S, B), B.getInt64(0),
                  R->TripCount});
  } else if (!(Buf = insertedVector(Base, B))) {
    return nullptr;
  }
  Value *Elem = B.CreateCall(A.get("smaug_elem"),
                             {Buf, B.getInt32(W), B.CreateZExtOrTrunc(Idx, B.getInt64Ty())});
  B.CreateCall(A.get("smaug_share_public"),
               {B.getInt32(W), Elem, publicTemp(Elt, B), B.getInt64(0),
                B.getInt64(1)});
  return Buf;
}

// V as secret storage; a public value is shared into a fresh slot or buffer.
Value *FunctionLowering::secretOperand(Value *V, IRBuilder<> &B) {
  if (SA.isSecret(V))
    return slot(V);
  unsigned W = secretWidth(V->getType());
  Operand Op = operand(V, B);
  if (!isVector(V)) {
    Value *Tmp = newSlot(W, "pub.s");
    B.CreateCall(A.get("smaug_share_public"),
                 {B.getInt32(W), Tmp, Op.Ptr, B.getInt64(0), B.getInt64(1)});
    return Tmp;
  }
  const VectorRegion *R = regionOf(B.GetInsertBlock()->getTerminator());
  IRBuilder<> PB(R->Setup->getTerminator());
  Value *Buf = PB.CreateCall(A.get("smaug_alloc"), {PB.getInt32(W), R->TripCount},
                             "pub.v");
  RegionBuffers[R].push_back({Buf, W});
  B.CreateCall(A.get("smaug_share_public"),
               {B.getInt32(W), Buf, Op.Ptr, B.getInt64(Op.Stride), R->TripCount});
  return Buf;
}

void FunctionLowering::copyInto(Value *Dst, Value *V, unsigned W,
                                IRBuilder<> &B) {
  if (SA.isSecret(V))
    B.CreateCall(A.get("smaug_copy"), {B.getInt32(W), Dst, slot(V), B.getInt64(1)});
  else
    B.CreateCall(A.get("smaug_share_public"),
                 {B.getInt32(W), Dst, publicTemp(V, B), B.getInt64(0),
                  B.getInt64(1)});
}

void FunctionLowering::callBinop(StringRef Name, Instruction &I, Value *L,
                                 Value *R, unsigned W) {
  IRBuilder<> B(&I);
  Value *N = count(&I, B);
  Value *Out = slot(&I);
  Operand OL = operand(L, B), OR = operand(R, B);
  if (OL.Secret && OR.Secret) {
    B.CreateCall(A.get(Name), {B.getInt32(W), Out, OL.Ptr, OR.Ptr, N});
  } else {
    Operand Sec = OL.Secret ? OL : OR, Pub = OL.Secret ? OR : OL;
    B.CreateCall(A.get((Name + "_sp").str()),
                 {B.getInt32(W), Out, Sec.Ptr, Pub.Ptr, B.getInt64(Pub.Stride),
                  B.getInt32(OL.Secret ? 0 : 1), N});
  }
  Lowered.insert(&I);
}

// --- Rules ------------------------------------------------------------------

void FunctionLowering::lowerBinary(BinaryOperator &I) {
  unsigned W = secretWidth(I.getType());
  if (!W)
    return unsupported(I, "secret value of an unsupported type");
  const char *Name = nullptr;
  switch (I.getOpcode()) {
  case Instruction::Add: Name = "smaug_add"; break;
  case Instruction::Sub: Name = "smaug_sub"; break;
  case Instruction::Mul: Name = "smaug_mul"; break;
  case Instruction::SDiv: Name = "smaug_sdiv"; break;
  case Instruction::UDiv: Name = "smaug_udiv"; break;
  case Instruction::SRem: Name = "smaug_srem"; break;
  case Instruction::URem: Name = "smaug_urem"; break;
  case Instruction::And: Name = "smaug_and"; break;
  case Instruction::Or: Name = "smaug_or"; break;
  case Instruction::Xor: Name = "smaug_xor"; break;
  case Instruction::Shl: Name = "smaug_shl"; break;
  case Instruction::LShr: Name = "smaug_lshr"; break;
  case Instruction::AShr: Name = "smaug_ashr"; break;
  case Instruction::FAdd: Name = "smaug_fadd"; break;
  case Instruction::FSub: Name = "smaug_fsub"; break;
  case Instruction::FMul: Name = "smaug_fmul"; break;
  case Instruction::FDiv: Name = "smaug_fdiv"; break;
  default:
    return unsupported(I, "no lowering for this operation on secret values");
  }
  callBinop(Name, I, I.getOperand(0), I.getOperand(1), W);
}

// smaug_icmp/smaug_fcmp and their _sp forms.
static void emitCompare(Abi &A, StringRef Name, IRBuilder<> &B, unsigned Pred,
                        unsigned W, Value *Out,
                        const FunctionLowering::Operand &OL,
                        const FunctionLowering::Operand &OR, Value *N) {
  if (OL.Secret && OR.Secret) {
    B.CreateCall(A.get(Name), {B.getInt32(Pred), B.getInt32(W), Out, OL.Ptr,
                               OR.Ptr, N});
  } else {
    auto Sec = OL.Secret ? OL : OR, Pub = OL.Secret ? OR : OL;
    B.CreateCall(A.get((Name + "_sp").str()),
                 {B.getInt32(Pred), B.getInt32(W), Out, Sec.Ptr, Pub.Ptr,
                  B.getInt64(Pub.Stride), B.getInt32(OL.Secret ? 0 : 1), N});
  }
}

void FunctionLowering::lowerICmp(ICmpInst &I) {
  Value *L = I.getOperand(0), *R = I.getOperand(1);
  unsigned W = secretWidth(L->getType());
  if (!W)
    return unsupported(I, "comparison of an unsupported type");
  IRBuilder<> B(&I);
  Value *N = count(&I, B);
  emitCompare(A, "smaug_icmp", B, I.getPredicate(), W, slot(&I), operand(L, B),
              operand(R, B), N);
  Lowered.insert(&I);
}

void FunctionLowering::lowerFCmp(FCmpInst &I) {
  Value *L = I.getOperand(0), *R = I.getOperand(1);
  if (!L->getType()->getScalarType()->isFloatTy())
    return unsupported(I, "only float (not double) comparisons are supported");
  switch (I.getPredicate()) {
  case FCmpInst::FCMP_OEQ:
  case FCmpInst::FCMP_OGT:
  case FCmpInst::FCMP_OGE:
  case FCmpInst::FCMP_OLT:
  case FCmpInst::FCMP_OLE:
  case FCmpInst::FCMP_ONE:
    break;
  default:
    return unsupported(I, "only ordered float comparisons are supported");
  }
  IRBuilder<> B(&I);
  Value *N = count(&I, B);
  emitCompare(A, "smaug_fcmp", B, I.getPredicate(), 32, slot(&I), operand(L, B),
              operand(R, B), N);
  Lowered.insert(&I);
}

void FunctionLowering::lowerSelect(SelectInst &I) {
  Value *C = I.getCondition();
  if (I.getType()->isPointerTy()) {
    if (SA.isSecret(C))
      unsupported(I, "pointer selected by a secret condition");
    return;
  }
  unsigned W = secretWidth(I.getType());
  if (!W)
    return unsupported(I, "secret value of an unsupported type");
  IRBuilder<> B(&I);
  Value *N = count(&I, B);
  Value *T = secretOperand(I.getTrueValue(), B);
  Value *Fv = secretOperand(I.getFalseValue(), B);
  if (SA.isSecret(C)) {
    B.CreateCall(A.get("smaug_select"),
                 {B.getInt32(W), slot(&I), slot(C), T, Fv, N});
  } else if (!isVector(C)) {
    // A public scalar condition picks one of the slots or buffers. The
    // chosen one is not written again before this value's uses, by SSA
    // dominance.
    Slots[&I] = B.CreateSelect(C, T, Fv, I.getName() + ".s");
  } else {
    Operand PC = operand(C, B);
    B.CreateCall(A.get("smaug_select_pubcond"),
                 {B.getInt32(W), slot(&I), PC.Ptr, B.getInt64(PC.Stride), T, Fv, N});
  }
  Lowered.insert(&I);
}

void FunctionLowering::lowerCast(CastInst &I) {
  Value *Src = I.getOperand(0);
  switch (I.getOpcode()) {
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc: {
    unsigned WD = secretWidth(I.getType()), WS = secretWidth(Src->getType());
    if (!WD || !WS)
      return unsupported(I, "width change of an unsupported type");
    const char *Name = I.getOpcode() == Instruction::ZExt   ? "smaug_zext"
                       : I.getOpcode() == Instruction::SExt ? "smaug_sext"
                                                            : "smaug_trunc";
    IRBuilder<> B(&I);
    B.CreateCall(A.get(Name), {B.getInt32(WD), B.getInt32(WS), slot(&I),
                               slot(Src), count(&I, B)});
    Lowered.insert(&I);
    return;
  }
  case Instruction::BitCast:
    if (secretWidth(I.getType()) == secretWidth(Src->getType()) &&
        secretWidth(I.getType())) {
      // Same bits, so the same storage.
      Slots[&I] = slot(Src);
      Lowered.insert(&I);
      return;
    }
    return unsupported(I, "bitcast of a secret value to a different width");
  default:
    return unsupported(I, "no lowering for this conversion of a secret value");
  }
}

void FunctionLowering::lowerReduce(IntrinsicInst &I, unsigned Op) {
  Value *Vec = I.getArgOperand(0);
  unsigned W = secretWidth(Vec->getType());
  if (!W)
    return unsupported(I, "reduction of an unsupported type");
  IRBuilder<> B(&I);
  B.CreateCall(A.get("smaug_reduce"), {B.getInt32(Op), B.getInt32(W), slot(&I),
                                        slot(Vec), count(&I, B)});
  Lowered.insert(&I);
}

void FunctionLowering::lowerIntrinsic(IntrinsicInst &I) {
  bool AnySecret = SA.isSecret(&I);
  for (Value *Op : I.args())
    AnySecret |= SA.isSecret(Op);
  switch (I.getIntrinsicID()) {
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end: {
    Value *RootV = SA.rootOf(I.getArgOperand(1));
    if (RootV && SA.isSecretRoot(RootV))
      Lowered.insert(&I);
    return;
  }
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_assign:
    return;
  default:
    break;
  }
  if (!AnySecret) {
    if (touchesVector(I) && I.getIntrinsicID() != Intrinsic::experimental_stepvector)
      unsupported(I, "public vector intrinsic with no lowering");
    return;
  }
  unsigned W = secretWidth(I.getType());
  switch (I.getIntrinsicID()) {
  case Intrinsic::smax:
    return callBinop("smaug_smax", I, I.getArgOperand(0), I.getArgOperand(1), W);
  case Intrinsic::smin:
    return callBinop("smaug_smin", I, I.getArgOperand(0), I.getArgOperand(1), W);
  case Intrinsic::umax:
    return callBinop("smaug_umax", I, I.getArgOperand(0), I.getArgOperand(1), W);
  case Intrinsic::umin:
    return callBinop("smaug_umin", I, I.getArgOperand(0), I.getArgOperand(1), W);
  case Intrinsic::abs: {
    // abs(a) = (a ^ s) - s, with s = a >> (w - 1) arithmetic.
    IRBuilder<> B(&I);
    Value *N = count(&I, B);
    Value *Src = slot(I.getArgOperand(0));
    Value *Sign = secretOperand(Constant::getNullValue(I.getType()), B);
    Value *Flip = secretOperand(Constant::getNullValue(I.getType()), B);
    Value *Shift = publicTemp(ConstantInt::get(I.getType()->getScalarType(), W - 1), B);
    B.CreateCall(A.get("smaug_ashr_sp"),
                 {B.getInt32(W), Sign, Src, Shift, B.getInt64(0), B.getInt32(0), N});
    B.CreateCall(A.get("smaug_xor"), {B.getInt32(W), Flip, Src, Sign, N});
    B.CreateCall(A.get("smaug_sub"), {B.getInt32(W), slot(&I), Flip, Sign, N});
    Lowered.insert(&I);
    return;
  }
  case Intrinsic::vector_reduce_add: return lowerReduce(I, SMAUG_RED_ADD);
  case Intrinsic::vector_reduce_mul: return lowerReduce(I, SMAUG_RED_MUL);
  case Intrinsic::vector_reduce_and: return lowerReduce(I, SMAUG_RED_AND);
  case Intrinsic::vector_reduce_or: return lowerReduce(I, SMAUG_RED_OR);
  case Intrinsic::vector_reduce_xor: return lowerReduce(I, SMAUG_RED_XOR);
  case Intrinsic::vector_reduce_smax: return lowerReduce(I, SMAUG_RED_SMAX);
  case Intrinsic::vector_reduce_smin: return lowerReduce(I, SMAUG_RED_SMIN);
  case Intrinsic::vector_reduce_umax: return lowerReduce(I, SMAUG_RED_UMAX);
  case Intrinsic::vector_reduce_umin: return lowerReduce(I, SMAUG_RED_UMIN);
  case Intrinsic::experimental_vector_reverse: {
    IRBuilder<> B(&I);
    B.CreateCall(A.get("smaug_reverse"),
                 {B.getInt32(W), slot(&I), slot(I.getArgOperand(0)), count(&I, B)});
    Lowered.insert(&I);
    return;
  }
  case Intrinsic::assume:
    Lowered.insert(&I);
    return;
  default:
    return unsupported(I, "no lowering for this intrinsic on secret values");
  }
}

void FunctionLowering::lowerExtract(ExtractElementInst &I) {
  Value *Vec = I.getVectorOperand(), *Idx = I.getIndexOperand();
  if (SA.isSecret(Idx))
    return unsupported(I, "extractelement at a secret index");
  IRBuilder<> B(&I);
  Value *Idx64 = B.CreateZExtOrTrunc(Idx, B.getInt64Ty());
  if (SA.isSecret(Vec)) {
    unsigned W = secretWidth(I.getType());
    Value *Elem = B.CreateCall(A.get("smaug_elem"), {slot(Vec), B.getInt32(W), Idx64});
    B.CreateCall(A.get("smaug_copy"), {B.getInt32(W), slot(&I), Elem, B.getInt64(1)});
    Lowered.insert(&I);
    return;
  }
  // A public element: the splat scalar, or a load from the public array.
  Value *Elem;
  if (Value *S = splatScalar(Vec)) {
    Elem = S;
  } else {
    auto It = PublicArrays.find(Vec);
    if (It == PublicArrays.end())
      return unsupported(I, "extractelement from a public vector with no "
                            "lowering");
    Elem = B.CreateLoad(I.getType(), B.CreateGEP(I.getType(), It->second, Idx64));
  }
  I.replaceAllUsesWith(Elem);
  Lowered.insert(&I);
}

// A splat of a secret scalar: every element is the scalar.
void FunctionLowering::lowerSplat(Instruction &I, Value *Scalar) {
  IRBuilder<> B(&I);
  B.CreateCall(A.get("smaug_fill"), {B.getInt32(secretWidth(I.getType())),
                                      slot(&I), slot(Scalar), count(&I, B)});
  Lowered.insert(&I);
}

// insertelement of a secret value, or into a secret vector: the base vector
// is copied or shared into the result's buffer, then the lane is set.
void FunctionLowering::lowerInsert(InsertElementInst &I) {
  Value *Base = I.getOperand(0), *Elt = I.getOperand(1), *Idx = I.getOperand(2);
  if (SA.isSecret(Idx))
    return unsupported(I, "insertelement at a secret index");
  const VectorRegion *R = regionOf(&I);
  if (!R)
    return unsupported(I, "vector value outside a vectorized loop");
  unsigned W = secretWidth(I.getType());
  IRBuilder<> B(&I);
  Value *Buf = slot(&I);
  if (SA.isSecret(Base)) {
    B.CreateCall(A.get("smaug_copy"), {B.getInt32(W), Buf, slot(Base), R->TripCount});
  } else if (!isa<UndefValue>(Base)) {
    Operand Op = operand(Base, B);
    if (Op.Secret)
      B.CreateCall(A.get("smaug_copy"), {B.getInt32(W), Buf, Op.Ptr, R->TripCount});
    else
      B.CreateCall(A.get("smaug_share_public"),
                   {B.getInt32(W), Buf, Op.Ptr, B.getInt64(Op.Stride), R->TripCount});
  }
  Value *Elem = B.CreateCall(A.get("smaug_elem"),
                             {Buf, B.getInt32(W), B.CreateZExtOrTrunc(Idx, B.getInt64Ty())});
  copyInto(Elem, Elt, W, B);
  Lowered.insert(&I);
}

// Element K of a public vector, or nullptr.
Value *FunctionLowering::publicElement(Value *V, Value *K, IRBuilder<> &B) {
  if (Value *S = splatScalar(V))
    return S;
  auto It = PublicArrays.find(V);
  if (It == PublicArrays.end())
    return nullptr;
  Type *ET = cast<VectorType>(V->getType())->getElementType();
  return B.CreateLoad(ET, B.CreateGEP(ET, It->second, K));
}

// Public vector arithmetic: a scalar loop over the trip count writes the
// elements to a public array of the value's own. Returns false if I is not
// such an operation.
bool FunctionLowering::lowerPublicVector(Instruction &I) {
  auto *II = dyn_cast<IntrinsicInst>(&I);
  bool Step = II && II->getIntrinsicID() == Intrinsic::experimental_stepvector;
  bool MinMax = II && (II->getIntrinsicID() == Intrinsic::smax ||
                       II->getIntrinsicID() == Intrinsic::smin ||
                       II->getIntrinsicID() == Intrinsic::umax ||
                       II->getIntrinsicID() == Intrinsic::umin);
  if (!isVector(&I) || SA.isSecret(&I) ||
      !(Step || MinMax || isa<BinaryOperator>(I) || isa<CastInst>(I) ||
        isa<CmpInst>(I) || isa<SelectInst>(I)))
    return false;
  Lowered.insert(&I);
  if (I.use_empty())
    return true;
  const VectorRegion *R = regionOf(&I);
  if (!R) {
    unsupported(I, "vector value outside a vectorized loop");
    return true;
  }
  Type *ET = cast<VectorType>(I.getType())->getElementType();
  IRBuilder<> B(&I);
  Value *Arr = B.CreateCall(
      F.getParent()->getOrInsertFunction("malloc", B.getPtrTy(), B.getInt64Ty()),
      {B.CreateMul(R->TripCount, B.getInt64(DL.getTypeAllocSize(ET)))},
      I.getName() + ".pub");
  PublicArrays[&I] = Arr;
  RegionArrays[R].push_back(Arr);

  // Head -> loop -> tail, with I at the start of the tail.
  BasicBlock *Head = I.getParent();
  BasicBlock *Tail = SplitBlock(Head, &I);
  BasicBlock *Loop = BasicBlock::Create(Ctx, "smaug.pubvec", &F, Tail);
  RegionOf[Tail] = RegionOf[Loop] = R;
  Head->getTerminator()->eraseFromParent();
  B.SetInsertPoint(Head);
  B.CreateCondBr(B.CreateICmpSGT(R->TripCount, B.getInt64(0)), Loop, Tail);
  B.SetInsertPoint(Loop);
  PHINode *K = B.CreatePHI(B.getInt64Ty(), 2);
  K->addIncoming(B.getInt64(0), Head);

  Value *E = nullptr;
  if (Step) {
    E = B.CreateZExtOrTrunc(K, ET);
  } else {
    SmallVector<Value *> Ops;
    for (Value *Op : II ? II->args() : I.operands()) {
      Value *X = publicElement(Op, K, B);
      if (!X) {
        unsupported(I, "public vector operand with no lowering");
        X = PoisonValue::get(Op->getType()->getScalarType());
      }
      Ops.push_back(X);
    }
    if (MinMax) {
      E = B.CreateBinaryIntrinsic(II->getIntrinsicID(), Ops[0], Ops[1]);
    } else {
      Instruction *C = I.clone();
      C->mutateType(ET);
      for (unsigned J = 0; J < Ops.size(); ++J)
        C->setOperand(J, Ops[J]);
      C->setMetadata("secret_shared", nullptr);
      E = B.Insert(C);
    }
  }
  B.CreateStore(E, B.CreateGEP(ET, Arr, K));
  Value *Next = B.CreateAdd(K, B.getInt64(1));
  K->addIncoming(Next, Loop);
  B.CreateCondBr(B.CreateICmpSLT(Next, R->TripCount), Loop, Tail);
  return true;
}

FunctionLowering::Root *FunctionLowering::rootFor(Value *Ptr, Instruction &I) {
  Value *RootV = SA.rootOf(Ptr);
  if (!RootV) {
    unsupported(I, "cannot tell which buffer this pointer points into");
    return nullptr;
  }
  auto It = Roots.find(RootV);
  if (It == Roots.end()) {
    unsupported(I, "no buffer for this secret memory");
    return nullptr;
  }
  return &It->second;
}

Value *FunctionLowering::elementIndex(Root &R, Value *Ptr, IRBuilder<> &B) {
  if (Ptr == R.Base)
    return B.getInt64(0);
  Value *Diff = B.CreateSub(B.CreatePtrToInt(Ptr, B.getInt64Ty()),
                            B.CreatePtrToInt(R.Base, B.getInt64Ty()));
  if (R.ElemBytes == 1)
    return Diff;
  return B.CreateLShr(Diff, Log2_32(R.ElemBytes), "", /*isExact=*/true);
}

Value *FunctionLowering::elementPtr(Root &R, Value *Ptr, IRBuilder<> &B) {
  return B.CreateCall(A.get("smaug_elem"),
                      {R.Storage, B.getInt32(R.W), elementIndex(R, Ptr, B)});
}

// The secret index of an access through gep T, base, idx with a secret idx
// and T the size of one element. A public offset of base is added.
Value *FunctionLowering::secretIndexSlot(Root &R, Value *Ptr, Instruction &I,
                                         unsigned &IdxW) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
  if (!GEP || GEP->getNumIndices() != 1 ||
      DL.getTypeAllocSize(GEP->getSourceElementType()) != R.ElemBytes ||
      SA.hasSecretOffset(GEP->getPointerOperand())) {
    unsupported(I, "secret-index access must be gep elem, base, index");
    return nullptr;
  }
  Value *Idx = GEP->getOperand(1);
  IdxW = secretWidth(Idx->getType());
  if (!IdxW || isVector(Idx)) {
    unsupported(I, "secret index of an unsupported type");
    return nullptr;
  }
  IRBuilder<> B(&I);
  Value *Base = elementIndex(R, GEP->getPointerOperand(), B);
  if (auto *C = dyn_cast<ConstantInt>(Base); C && C->isZero())
    return slot(Idx);
  Value *Sum = newSlot(IdxW, "oidx");
  Value *Off = B.CreateZExtOrTrunc(Base, Idx->getType());
  B.CreateCall(A.get("smaug_add_sp"),
               {B.getInt32(IdxW), Sum, slot(Idx), publicTemp(Off, B),
                B.getInt64(0), B.getInt32(0), B.getInt64(1)});
  return Sum;
}

void FunctionLowering::lowerLoad(LoadInst &I) {
  Value *Ptr = I.getPointerOperand();
  Value *RootV = SA.rootOf(Ptr);
  if (!RootV || !SA.isSecretRoot(RootV)) {
    if (SA.hasSecretOffset(Ptr))
      return unsupported(I, "secret-index read of public memory");
    if (isVector(&I)) {
      // A public vector: copy the elements into an array of its own.
      const VectorRegion *R = regionOf(&I);
      if (!R)
        return unsupported(I, "vector load outside a vectorized loop");
      IRBuilder<> B(&I);
      Value *Bytes = B.CreateMul(
          R->TripCount,
          B.getInt64(DL.getTypeAllocSize(cast<VectorType>(I.getType())->getElementType())));
      Value *Arr = B.CreateCall(
          F.getParent()->getOrInsertFunction("malloc", B.getPtrTy(), B.getInt64Ty()),
          {Bytes}, I.getName() + ".pub");
      B.CreateMemCpy(Arr, MaybeAlign(), Ptr, MaybeAlign(), Bytes);
      PublicArrays[&I] = Arr;
      RegionArrays[R].push_back(Arr);
      Lowered.insert(&I);
    }
    return;
  }
  Root *R = rootFor(Ptr, I);
  if (!R)
    return;
  if (secretWidth(I.getType()) != R->W)
    return unsupported(I, "access type does not match the buffer's element "
                          "type i" + Twine(R->W));
  IRBuilder<> B(&I);
  if (SA.hasSecretOffset(Ptr)) {
    if (isVector(&I))
      return unsupported(I, "vector access at a secret offset");
    unsigned IdxW;
    Value *Idx = secretIndexSlot(*R, Ptr, I, IdxW);
    if (!Idx)
      return;
    B.CreateCall(A.get("smaug_oload"), {B.getInt32(R->W), slot(&I), R->Storage,
                                         R->Len, Idx, B.getInt32(IdxW)});
  } else {
    B.CreateCall(A.get("smaug_copy"), {B.getInt32(R->W), slot(&I),
                                        elementPtr(*R, Ptr, B), count(&I, B)});
  }
  Lowered.insert(&I);
}

void FunctionLowering::lowerStore(StoreInst &I) {
  Value *Ptr = I.getPointerOperand(), *V = I.getValueOperand();
  Value *RootV = SA.rootOf(Ptr);
  if (!RootV || !SA.isSecretRoot(RootV)) {
    if (SA.isSecret(V))
      unsupported(I, RootV ? "secret value stored to public memory"
                           : "secret value stored through a pointer whose "
                             "buffer is unknown");
    else if (SA.hasSecretOffset(Ptr))
      unsupported(I, "secret-index write to public memory");
    else if (isVector(V))
      unsupported(I, "public vector stored to public memory");
    return;
  }
  Root *R = rootFor(Ptr, I);
  if (!R)
    return;
  if (secretWidth(V->getType()) != R->W)
    return unsupported(I, "access type does not match the buffer's element "
                          "type i" + Twine(R->W));
  IRBuilder<> B(&I);
  if (SA.hasSecretOffset(Ptr)) {
    if (isVector(V))
      return unsupported(I, "vector access at a secret offset");
    unsigned IdxW;
    Value *Idx = secretIndexSlot(*R, Ptr, I, IdxW);
    if (!Idx)
      return;
    B.CreateCall(A.get("smaug_ostore"), {B.getInt32(R->W), R->Storage, R->Len,
                                          Idx, B.getInt32(IdxW),
                                          secretOperand(V, B)});
  } else {
    Value *N = count(&I, B);
    Operand Op = operand(V, B);
    Value *Elem = elementPtr(*R, Ptr, B);
    if (Op.Secret)
      B.CreateCall(A.get("smaug_copy"), {B.getInt32(R->W), Elem, Op.Ptr, N});
    else
      B.CreateCall(A.get("smaug_share_public"),
                   {B.getInt32(R->W), Elem, Op.Ptr, B.getInt64(Op.Stride), N});
  }
  Lowered.insert(&I);
}

void FunctionLowering::lowerMemTransfer(MemTransferInst &I) {
  Value *DRoot = SA.rootOf(I.getRawDest()), *SRoot = SA.rootOf(I.getRawSource());
  bool DS = DRoot && SA.isSecretRoot(DRoot), SS = SRoot && SA.isSecretRoot(SRoot);
  if (!DS && !SS)
    return;
  if (!DS)
    return unsupported(I, "copy from secret memory to public memory");
  if (SA.hasSecretOffset(I.getRawDest()) || SA.hasSecretOffset(I.getRawSource()))
    return unsupported(I, "copy at a secret offset");
  Root *D = rootFor(I.getRawDest(), I);
  if (!D)
    return;
  IRBuilder<> B(&I);
  Value *Count = B.CreateUDiv(B.CreateZExtOrTrunc(I.getLength(), B.getInt64Ty()),
                              B.getInt64(D->ElemBytes));
  Value *Dst = elementPtr(*D, I.getRawDest(), B);
  if (SS) {
    Root *S = rootFor(I.getRawSource(), I);
    if (!S)
      return;
    B.CreateCall(A.get("smaug_copy"),
                 {B.getInt32(D->W), Dst, elementPtr(*S, I.getRawSource(), B), Count});
  } else {
    B.CreateCall(A.get("smaug_share_public"),
                 {B.getInt32(D->W), Dst, I.getRawSource(), B.getInt64(1), Count});
  }
  Lowered.insert(&I);
}

void FunctionLowering::lowerMemSet(MemSetInst &I) {
  Value *RootV = SA.rootOf(I.getRawDest());
  if (!RootV || !SA.isSecretRoot(RootV))
    return;
  if (SA.isSecret(I.getValue()))
    return unsupported(I, "memset with a secret value");
  Root *R = rootFor(I.getRawDest(), I);
  if (!R)
    return;
  IRBuilder<> B(&I);
  // Every element holds the byte repeated: multiply by 0x0101...01.
  Type *ET = B.getIntNTy(R->W == 1 ? 8 : R->W);
  Value *Byte = B.CreateZExt(I.getValue(), ET);
  uint64_t Ones = 0;
  for (unsigned K = 0; K < R->ElemBytes; ++K)
    Ones |= 1ull << (8 * K);
  Value *Pattern = B.CreateMul(Byte, ConstantInt::get(ET, Ones));
  Value *Count = B.CreateUDiv(B.CreateZExtOrTrunc(I.getLength(), B.getInt64Ty()),
                              B.getInt64(R->ElemBytes));
  B.CreateCall(A.get("smaug_share_public"),
               {B.getInt32(R->W), elementPtr(*R, I.getRawDest(), B),
                publicTemp(Pattern, B), B.getInt64(0), Count});
  Lowered.insert(&I);
}

void FunctionLowering::lowerCall(CallBase &I) {
  Function *Callee = I.getCalledFunction();
  StringRef Name = Callee ? Callee->getName() : "";
  if (Name == "malloc" || Name == "calloc")
    return; // secret allocations are replaced in createRoots
  if (Name == "free") {
    Value *RootV = SA.rootOf(I.getArgOperand(0));
    if (!RootV || !SA.isSecretRoot(RootV))
      return;
    Root *R = rootFor(I.getArgOperand(0), I);
    if (!R)
      return;
    if (R->Arg)
      return unsupported(I, "free of a secret argument buffer");
    IRBuilder<> B(&I);
    B.CreateCall(A.get("smaug_free"), {R->Storage, B.getInt32(R->W), R->Len});
    Lowered.insert(&I);
    return;
  }
  for (Value *Op : I.args()) {
    if (SA.isSecret(Op))
      return unsupported(I, "secret value passed to a call");
    if (Op->getType()->isPointerTy())
      if (Value *RootV = SA.rootOf(Op); RootV && SA.isSecretRoot(RootV))
        return unsupported(I, "secret buffer passed to a call");
  }
}

void FunctionLowering::lowerReturn(ReturnInst &I) {
  IRBuilder<> B(&I);
  Value *V = I.getReturnValue();
  if (V && SA.isSecret(V)) {
    if (!Spec.Output)
      return unsupported(I, "secret return value but the metadata has no "
                            "\"output\"");
    unsigned W = secretWidth(V->getType());
    IRBuilder<> EB(EntryPoint);
    Value *Tmp = EB.CreateAlloca(V->getType());
    B.CreateCall(A.get(*Spec.Output == 0 ? "smaug_reveal" : "smaug_export"),
                 {B.getInt32(W), Tmp, slot(V), B.getInt64(1)});
    I.setOperand(0, B.CreateLoad(V->getType(), Tmp));
  }
  for (auto &[RootV, R] : Roots) {
    if (R.Export)
      B.CreateCall(A.get("smaug_export"),
                   {B.getInt32(R.W), R.Arg, R.Storage, R.Len});
    if (R.FreeAtReturn)
      B.CreateCall(A.get("smaug_free"), {R.Storage, B.getInt32(R.W), R.Len});
  }
  for (auto [S, W] : AllocatedSlots)
    B.CreateCall(A.get("smaug_free"), {S, B.getInt32(W), B.getInt64(1)});
}

void FunctionLowering::finishPhis() {
  // Copy each edge's incoming values into the phi slots. The copies must act
  // in parallel: each reads the values from before any of them. With more
  // than one phi in a block, copy through temporaries. Checking only whether
  // an incoming value is another phi is not enough, since a value can share
  // a phi's slot: freeze and same-width bitcast reuse their operand's slot,
  // and a select on a public scalar condition is a pointer to one of its
  // operands' slots. The extra copies are local in both backends.
  MapVector<BasicBlock *, SmallVector<PHINode *>> ByBlock;
  for (PHINode *PN : SecretPhis)
    ByBlock[PN->getParent()].push_back(PN);
  for (auto &[BB, Phis] : ByBlock) {
    bool Parallel = Phis.size() > 1;
    SmallPtrSet<BasicBlock *, 8> Seen;
    for (BasicBlock *P : predecessors(BB)) {
      if (!Seen.insert(P).second)
        continue;
      Instruction *At = P->getSingleSuccessor()
                            ? P->getTerminator()
                            : &*BB->getFirstInsertionPt();
      IRBuilder<> B(At);
      SmallVector<std::pair<PHINode *, Value *>> Temps;
      for (PHINode *PN : Phis) {
        unsigned W = secretWidth(PN->getType());
        Value *In = PN->getIncomingValueForBlock(P);
        if (Parallel) {
          Value *T = newSlot(W, "phi.tmp");
          copyInto(T, In, W, B);
          Temps.push_back({PN, T});
        } else {
          copyInto(slot(PN), In, W, B);
        }
      }
      for (auto [PN, T] : Temps)
        B.CreateCall(A.get("smaug_copy"),
                     {B.getInt32(secretWidth(PN->getType())), slot(PN), T,
                      B.getInt64(1)});
    }
  }
}

void FunctionLowering::lower(Instruction &I) {
  if (Lowered.count(&I))
    return;
  if (auto *PN = dyn_cast<PHINode>(&I)) {
    if (isVector(PN))
      return unsupported(I, "vector phi outside a vectorized loop header");
    if (SA.isSecret(PN)) {
      slot(PN);
      SecretPhis.push_back(PN);
      Lowered.insert(PN);
    }
    return;
  }
  if (lowerPublicVector(I))
    return;
  if (auto *LI = dyn_cast<LoadInst>(&I))
    return lowerLoad(*LI);
  if (auto *SI = dyn_cast<StoreInst>(&I))
    return lowerStore(*SI);
  if (auto *MT = dyn_cast<MemTransferInst>(&I))
    return lowerMemTransfer(*MT);
  if (auto *MS = dyn_cast<MemSetInst>(&I))
    return lowerMemSet(*MS);
  if (auto *II = dyn_cast<IntrinsicInst>(&I))
    return lowerIntrinsic(*II);
  if (auto *CB = dyn_cast<CallBase>(&I))
    return lowerCall(*CB);
  if (auto *RI = dyn_cast<ReturnInst>(&I)) {
    Returns.push_back(RI);
    return;
  }
  if (auto *BI = dyn_cast<BranchInst>(&I)) {
    if (BI->isConditional() && SA.isSecret(BI->getCondition()))
      unsupported(I, "branch on a secret condition");
    return;
  }
  if (auto *SwI = dyn_cast<SwitchInst>(&I)) {
    if (SA.isSecret(SwI->getCondition()))
      unsupported(I, "switch on a secret value");
    return;
  }
  if (auto *SelI = dyn_cast<SelectInst>(&I)) {
    if (SA.isSecret(SelI) || SelI->getType()->isPointerTy())
      return lowerSelect(*SelI);
    if (isVector(SelI))
      unsupported(I, "public vector select with no lowering");
    return;
  }
  if (auto *EE = dyn_cast<ExtractElementInst>(&I))
    return lowerExtract(*EE);
  if (isa<InsertElementInst>(I) || isa<ShuffleVectorInst>(I)) {
    // Splats. A splat of a public scalar is used as a stride-0 operand; one
    // of a secret scalar fills a buffer. The insertelement feeding a splat
    // shuffle needs no code of its own.
    if (isa<InsertElementInst>(I)) {
      // Public insertelements are materialized where they are used.
      if (!SA.isSecret(&I))
        return;
      for (User *U : I.users())
        if (!splatScalar(U))
          return lowerInsert(cast<InsertElementInst>(I));
      Lowered.insert(&I);
      return;
    }
    Value *S = splatScalar(&I);
    if (!S)
      return unsupported(I, "shufflevector that is not a splat");
    if (SA.isSecret(S))
      lowerSplat(I, S);
    return;
  }
  if (isa<GetElementPtrInst>(I)) {
    if (isVector(&I))
      unsupported(I, "vector of pointers");
    return; // addresses; a secret index is used by the access
  }
  if (auto *PI = dyn_cast<PtrToIntInst>(&I)) {
    if (SA.hasSecretOffset(PI->getPointerOperand()))
      unsupported(I, "address computed from a secret index");
    return;
  }
  if (!SA.isSecret(&I)) {
    for (Value *Op : I.operands())
      if (SA.isSecret(Op))
        return unsupported(I, "secret operand in an instruction with no "
                              "lowering");
    if (touchesVector(I))
      unsupported(I, "public vector operation with no lowering");
    return;
  }
  if (auto *BO = dyn_cast<BinaryOperator>(&I))
    return lowerBinary(*BO);
  if (auto *IC = dyn_cast<ICmpInst>(&I))
    return lowerICmp(*IC);
  if (auto *FC = dyn_cast<FCmpInst>(&I))
    return lowerFCmp(*FC);
  if (auto *CI = dyn_cast<CastInst>(&I))
    return lowerCast(*CI);
  if (auto *FI = dyn_cast<FreezeInst>(&I)) {
    Slots[FI] = slot(FI->getOperand(0));
    Lowered.insert(FI);
    return;
  }
  if (auto *UO = dyn_cast<UnaryOperator>(&I)) {
    if (UO->getOpcode() == Instruction::FNeg &&
        UO->getType()->getScalarType()->isFloatTy()) {
      // -x = 0 - x.
      IRBuilder<> B(&I);
      B.CreateCall(A.get("smaug_fsub_sp"),
                   {B.getInt32(32), slot(&I), slot(UO->getOperand(0)),
                    publicTemp(ConstantFP::get(UO->getType()->getScalarType(), 0.0), B),
                    B.getInt64(0), B.getInt32(1), count(&I, B)});
      Lowered.insert(&I);
      return;
    }
  }
  unsupported(I, "no lowering for this instruction on secret values");
}

// --- Cleanup ----------------------------------------------------------------

void FunctionLowering::eraseLowered() {
  // Free each vector loop's buffers and public arrays after its middle
  // block's last use.
  for (const VectorRegion &R : Regions) {
    IRBuilder<> B(R.Middle->getTerminator());
    for (auto [Buf, W] : RegionBuffers[&R])
      B.CreateCall(A.get("smaug_free"), {Buf, B.getInt32(W), R.TripCount});
    for (Value *Arr : RegionArrays[&R])
      B.CreateCall(F.getParent()->getOrInsertFunction("free", B.getVoidTy(),
                                                      B.getPtrTy()),
                   {Arr});
  }

  SetVector<Instruction *> Erase(Lowered.begin(), Lowered.end());
  // Address arithmetic and public vector values used only by lowered
  // instructions go too.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Instruction &I : instructions(F)) {
      if (Erase.count(&I) || I.use_empty() || I.mayHaveSideEffects())
        continue;
      if (!I.getType()->isPointerTy() && !isa<PtrToIntInst>(I) && !isVector(&I))
        continue;
      bool AllErased = true;
      for (User *U : I.users())
        AllErased &= Erase.count(cast<Instruction>(U)) > 0;
      if (AllErased)
        Changed |= Erase.insert(&I);
    }
  }
  bool Ok = true;
  for (Instruction *I : Erase)
    for (User *U : I->users())
      if (auto *UI = dyn_cast<Instruction>(U); UI && !Erase.count(UI)) {
        unsupported(*UI, "uses a secret value that was lowered (internal "
                         "error)");
        Ok = false;
      }
  if (!Ok)
    return;
  for (Instruction *I : Erase)
    I->replaceAllUsesWith(PoisonValue::get(I->getType()));
  for (Instruction *I : Erase)
    I->eraseFromParent();
  // Unused vector instructions, e.g. splats of public scalars.
  Changed = true;
  while (Changed) {
    Changed = false;
    SmallVector<Instruction *> Dead;
    for (Instruction &I : instructions(F))
      if (touchesVector(I) && I.use_empty() && !I.mayHaveSideEffects())
        Dead.push_back(&I);
    for (Instruction *I : Dead)
      I->eraseFromParent();
    Changed = !Dead.empty();
  }
  for (Instruction &I : instructions(F))
    if (touchesVector(I))
      unsupported(I, "vector value left after lowering (internal error)");
}

void FunctionLowering::run() {
  size_t DiagsBefore = Diags.size();
  splitPhiEdges();
  // Slots, imported arguments and public temporaries are created in a block
  // of their own before the original entry block.
  BasicBlock *OldEntry = &F.getEntryBlock();
  BasicBlock *Setup = BasicBlock::Create(Ctx, "smaug.entry", &F, OldEntry);
  EntryPoint = BranchInst::Create(OldEntry, Setup);

  std::vector<Instruction *> Order;
  ReversePostOrderTraversal<Function *> RPOT(&F);
  for (BasicBlock *BB : RPOT)
    if (BB != Setup)
      for (Instruction &I : *BB)
        Order.push_back(&I);

  if (!assignRootTypes())
    return;
  createRoots();
  importScalarArgs();
  for (Instruction *I : Order)
    lower(*I);
  if (Diags.size() != DiagsBefore)
    return;
  finishPhis();
  for (ReturnInst *RI : Returns)
    lowerReturn(*RI);
  if (Diags.size() != DiagsBefore)
    return;
  eraseLowered();
}

} // namespace smaug
