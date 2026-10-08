// mpc-narrow-bool: secret i8 buffers that only ever hold 0 or 1 become i1
// accesses, so that mpc-lower stores them as Bits and lowers their uses as
// one-bit operations.
//
// Candidates are the secret malloc, calloc and alloca buffers of functions
// with secret arguments. Their layout does not matter after lowering, since
// mpc-lower replaces them with ABI storage; public buffers are left alone
// because an i1 vector is bit-packed in memory and an i8 array is not.
//
// A candidate is narrowed when, at a fixpoint over all candidates:
// - the pointer does not escape: through GEPs, phis and selects it only
//   reaches loads, stores (as the address), memset, memcpy, free, lifetime
//   markers, compares and ptrtoint;
// - it is only accessed as i8 or <vscale x 1 x i8>;
// - every stored value is 0 or 1 (see isBool);
// - memset writes 0 or 1, and memcpy partners are narrowed too.
//
// Rewrite: an i8 load becomes zext(load i1) and a store writes the i1 form of
// its value. Local folds then remove the conversions where the value is used
// as a boolean.

#include "MPCLower.h"
#include "SecretAnalysis.h"
#include "SecretSpec.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace llvm::PatternMatch;

extern cl::opt<std::string> MetadataFilePath;

namespace smaug {

namespace {

// i8 or a vector of i8.
bool isByteType(Type *T) { return T->getScalarType()->isIntegerTy(8); }

Type *boolTypeLike(Type *T) {
  Type *I1 = Type::getInt1Ty(T->getContext());
  if (auto *VT = dyn_cast<VectorType>(T))
    return VectorType::get(I1, VT->getElementCount());
  return I1;
}

// A constant whose elements are all 0 or 1 (undef and poison count too).
bool isBoolConstant(Constant *C) {
  if (isa<UndefValue>(C) || C->isNullValue())
    return true;
  if (auto *CI = dyn_cast<ConstantInt>(C))
    return CI->getValue().ule(1);
  if (Constant *S = C->getSplatValue())
    return isBoolConstant(S);
  if (auto *VT = dyn_cast<FixedVectorType>(C->getType())) {
    for (unsigned I = 0; I < VT->getNumElements(); ++I)
      if (!isBoolConstant(C->getAggregateElement(I)))
        return false;
    return true;
  }
  return false;
}

// The i1 form of a constant isBoolConstant accepted. Splats, including
// scalable ones, stay splats, so that mpc-lower sees a splat constant.
Constant *boolConstant(Constant *C, Type *BT) {
  if (isa<PoisonValue>(C))
    return PoisonValue::get(BT);
  if (isa<UndefValue>(C))
    return UndefValue::get(BT);
  if (C->isNullValue())
    return Constant::getNullValue(BT);
  if (auto *CI = dyn_cast<ConstantInt>(C))
    return ConstantInt::get(BT, CI->getZExtValue());
  auto *VT = cast<VectorType>(BT);
  if (Constant *S = C->getSplatValue())
    return ConstantVector::getSplat(VT->getElementCount(),
                                    boolConstant(S, VT->getElementType()));
  SmallVector<Constant *> Elts;
  for (unsigned I = 0; I < cast<FixedVectorType>(VT)->getNumElements(); ++I)
    Elts.push_back(
        boolConstant(C->getAggregateElement(I), VT->getElementType()));
  return ConstantVector::get(Elts);
}

// x & true, x & false, x | false and x ^ false on i1 values, or nullptr.
Value *foldBoolOp(Instruction::BinaryOps Op, Value *L, Value *R) {
  for (int K = 0; K < 2; ++K, std::swap(L, R)) {
    auto *C = dyn_cast<Constant>(R);
    if (!C)
      continue;
    if (Op == Instruction::And && C->isAllOnesValue())
      return L;
    if (Op == Instruction::And && C->isNullValue())
      return C;
    if ((Op == Instruction::Or || Op == Instruction::Xor) && C->isNullValue())
      return L;
  }
  return nullptr;
}

class BoolNarrowing {
public:
  BoolNarrowing(Function &F, const FunctionSpec &Spec) : F(F), SA(F, Spec) {}

  bool run() {
    collect();
    // Drop candidates until every remaining one is legal given the others.
    // Each round checks against the previous round's set.
    while (true) {
      SetVector<Value *> Next;
      for (Value *Root : Candidates)
        if (legal(Root))
          Next.insert(Root);
      if (Next.size() == Candidates.size())
        break;
      Candidates = std::move(Next);
    }
    if (Candidates.empty())
      return false;
    rewrite();
    return true;
  }

private:
  struct Uses {
    SmallVector<LoadInst *> Loads;
    SmallVector<StoreInst *> Stores;
    SmallVector<MemSetInst *> MemSets;
    SmallVector<MemTransferInst *> Copies;
    bool Escapes = false;
  };

  // Follows each secret local buffer's pointer to its accesses.
  void collect() {
    for (Value *Root : SA.secretRoots()) {
      if (isa<Argument>(Root))
        continue;
      Uses &U = RootUses[Root];
      SmallVector<Value *> Work{Root};
      SmallPtrSet<Value *, 16> Seen{Root};
      while (!Work.empty() && !U.Escapes) {
        Value *P = Work.pop_back_val();
        for (User *Us : P->users()) {
          auto *I = dyn_cast<Instruction>(Us);
          if (!I) {
            U.Escapes = true;
            break;
          }
          if (isa<GetElementPtrInst>(I) || isa<PHINode>(I) ||
              isa<SelectInst>(I)) {
            if (isa<GetElementPtrInst>(I) &&
                cast<GetElementPtrInst>(I)->getPointerOperand() != P) {
              U.Escapes = true; // the pointer used as an index
              break;
            }
            // A phi or select of pointers into different buffers.
            if (SA.rootOf(I) != Root) {
              U.Escapes = true;
              break;
            }
            if (Seen.insert(I).second)
              Work.push_back(I);
          } else if (auto *LI = dyn_cast<LoadInst>(I)) {
            U.Loads.push_back(LI);
          } else if (auto *SI = dyn_cast<StoreInst>(I)) {
            if (SI->getValueOperand() == P) {
              U.Escapes = true;
              break;
            }
            U.Stores.push_back(SI);
          } else if (auto *MS = dyn_cast<MemSetInst>(I)) {
            U.MemSets.push_back(MS);
          } else if (auto *MT = dyn_cast<MemTransferInst>(I)) {
            U.Copies.push_back(MT);
          } else if (auto *II = dyn_cast<IntrinsicInst>(I);
                     II && II->isLifetimeStartOrEnd()) {
          } else if (auto *CI = dyn_cast<CallInst>(I);
                     CI && CI->getCalledFunction() &&
                     CI->getCalledFunction()->getName() == "free") {
          } else if (isa<ICmpInst>(I) || isa<PtrToIntInst>(I)) {
          } else {
            U.Escapes = true;
            break;
          }
        }
      }
      Candidates.insert(Root);
    }
  }

  bool legal(Value *Root) {
    Uses &U = RootUses[Root];
    if (U.Escapes)
      return false;
    for (LoadInst *LI : U.Loads)
      if (!isByteType(LI->getType()) || LI->isVolatile())
        return false;
    for (StoreInst *SI : U.Stores) {
      Value *V = SI->getValueOperand();
      SmallPtrSet<Value *, 16> InProgress;
      if (!isByteType(V->getType()) || SI->isVolatile() ||
          !isBool(V, InProgress))
        return false;
    }
    for (MemSetInst *MS : U.MemSets) {
      auto *C = dyn_cast<ConstantInt>(MS->getValue());
      if (!C || C->getValue().ugt(1))
        return false;
    }
    for (MemTransferInst *MT : U.Copies) {
      Value *Other = SA.rootOf(MT->getRawDest()) == Root
                         ? SA.rootOf(MT->getRawSource())
                         : SA.rootOf(MT->getRawDest());
      if (!Other || !Candidates.contains(Other))
        return false;
    }
    return true;
  }

  // Whether V is always 0 or 1, element-wise. Phis being checked are assumed
  // to be, which is what the fixpoint over a loop needs.
  bool isBool(Value *V, SmallPtrSetImpl<Value *> &InProgress) {
    if (auto *C = dyn_cast<Constant>(V))
      return isBoolConstant(C);
    if (match(V, m_ZExt(m_Value())))
      return cast<ZExtInst>(V)->getSrcTy()->getScalarType()->isIntegerTy(1);
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      Value *Root = SA.rootOf(LI->getPointerOperand());
      return Root && Candidates.contains(Root);
    }
    if (!InProgress.insert(V).second)
      return true;
    if (auto *PN = dyn_cast<PHINode>(V)) {
      for (Value *In : PN->incoming_values())
        if (!isBool(In, InProgress))
          return false;
      return true;
    }
    if (auto *Sel = dyn_cast<SelectInst>(V))
      return isBool(Sel->getTrueValue(), InProgress) &&
             isBool(Sel->getFalseValue(), InProgress);
    if (auto *BO = dyn_cast<BinaryOperator>(V)) {
      bool L = isBool(BO->getOperand(0), InProgress);
      bool R = isBool(BO->getOperand(1), InProgress);
      switch (BO->getOpcode()) {
      case Instruction::And:
        return L || R; // x & b is 0 or 1 when b is
      case Instruction::Or:
      case Instruction::Xor:
        return L && R;
      default:
        return false;
      }
    }
    if (auto *SV = dyn_cast<ShuffleVectorInst>(V))
      return isBool(SV->getOperand(0), InProgress) &&
             isBool(SV->getOperand(1), InProgress);
    if (auto *IE = dyn_cast<InsertElementInst>(V))
      return isBool(IE->getOperand(0), InProgress) &&
             isBool(IE->getOperand(1), InProgress);
    return false;
  }

  // The i1 form of a value isBool accepted.
  Value *asBool(Value *V) {
    auto It = BoolOf.find(V);
    if (It != BoolOf.end())
      return It->second;
    Type *BT = boolTypeLike(V->getType());
    Value *R = nullptr;
    if (auto *C = dyn_cast<Constant>(V)) {
      R = boolConstant(C, BT);
    } else if (auto *Z = dyn_cast<ZExtInst>(V)) {
      R = Z->getOperand(0);
    } else if (auto *PN = dyn_cast<PHINode>(V)) {
      PHINode *NP = PHINode::Create(BT, PN->getNumIncomingValues(),
                                    PN->getName() + ".b", PN);
      BoolOf[V] = NP; // before the operands, for loops
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I)
        NP->addIncoming(asBool(PN->getIncomingValue(I)),
                        PN->getIncomingBlock(I));
      return NP;
    } else {
      auto *I = cast<Instruction>(V);
      IRBuilder<> B(I->getNextNode());
      if (auto *Sel = dyn_cast<SelectInst>(I)) {
        Value *T = asBool(Sel->getTrueValue()),
              *Fv = asBool(Sel->getFalseValue());
        B.SetInsertPoint(I->getNextNode());
        R = B.CreateSelect(Sel->getCondition(), T, Fv, I->getName() + ".b");
      } else if (auto *BO = dyn_cast<BinaryOperator>(I)) {
        if (BO->getOpcode() == Instruction::And &&
            !isBoolKnown(BO->getOperand(0))) {
          // x & b: only bit 0 of x matters.
          Value *Bb = asBool(BO->getOperand(1));
          B.SetInsertPoint(I->getNextNode());
          R = B.CreateAnd(B.CreateTrunc(BO->getOperand(0), BT), Bb,
                          I->getName() + ".b");
        } else if (BO->getOpcode() == Instruction::And &&
                   !isBoolKnown(BO->getOperand(1))) {
          Value *Ab = asBool(BO->getOperand(0));
          B.SetInsertPoint(I->getNextNode());
          R = B.CreateAnd(Ab, B.CreateTrunc(BO->getOperand(1), BT),
                          I->getName() + ".b");
        } else {
          Value *L = asBool(BO->getOperand(0)), *Rt = asBool(BO->getOperand(1));
          B.SetInsertPoint(I->getNextNode());
          R = foldBoolOp(BO->getOpcode(), L, Rt);
          if (!R)
            R = B.CreateBinOp(BO->getOpcode(), L, Rt, I->getName() + ".b");
        }
      } else if (auto *SV = dyn_cast<ShuffleVectorInst>(I)) {
        Value *A = asBool(SV->getOperand(0)), *Bv = asBool(SV->getOperand(1));
        B.SetInsertPoint(I->getNextNode());
        R = B.CreateShuffleVector(A, Bv, SV->getShuffleMask(),
                                  I->getName() + ".b");
      } else if (auto *IE = dyn_cast<InsertElementInst>(I)) {
        Value *A = asBool(IE->getOperand(0)), *E = asBool(IE->getOperand(1));
        B.SetInsertPoint(I->getNextNode());
        R = B.CreateInsertElement(A, E, IE->getOperand(2), I->getName() + ".b");
      } else {
        // A load from a narrowed buffer was rewritten to zext(load i1) and
        // is reached through the zext case; nothing else passes isBool.
        llvm_unreachable("asBool of a value isBool rejected");
      }
      if (auto *RI = dyn_cast<Instruction>(R))
        RI->copyMetadata(*I, {LLVMContext::MD_dbg});
    }
    BoolOf[V] = R;
    return R;
  }

  bool isBoolKnown(Value *V) {
    SmallPtrSet<Value *, 16> InProgress;
    return isBool(V, InProgress);
  }

  void rewrite() {
    SmallVector<ZExtInst *> Widened;
    // Loads first, so that stored loads are reached as zext(load i1).
    for (Value *Root : Candidates)
      for (LoadInst *LI : RootUses[Root].Loads) {
        IRBuilder<> B(LI);
        LoadInst *NL = B.CreateAlignedLoad(
            boolTypeLike(LI->getType()), LI->getPointerOperand(),
            LI->getAlign(), LI->getName() + ".b");
        NL->copyMetadata(*LI);
        auto *Z = cast<ZExtInst>(B.CreateZExt(NL, LI->getType()));
        LI->replaceAllUsesWith(Z);
        LI->eraseFromParent();
        Widened.push_back(Z);
      }
    for (Value *Root : Candidates)
      for (StoreInst *SI : RootUses[Root].Stores) {
        Value *BV = asBool(SI->getValueOperand());
        IRBuilder<> B(SI);
        StoreInst *NS =
            B.CreateAlignedStore(BV, SI->getPointerOperand(), SI->getAlign());
        NS->copyMetadata(*SI);
        SI->eraseFromParent();
      }
    fold(Widened);
  }

  // Removes conversions around the narrowed loads where the value is used as
  // a boolean. Z = zext b to iN with b an i1 (or vector of i1).
  void fold(SmallVectorImpl<ZExtInst *> &Work) {
    while (!Work.empty()) {
      ZExtInst *Z = Work.pop_back_val();
      Value *Bv = Z->getOperand(0);
      // A fold can give Z new users (z & 1 becomes z), so repeat until none
      // applies.
      bool Changed = true;
      while (Changed) {
        Changed = false;
        for (User *U : make_early_inc_range(Z->users())) {
          auto *I = dyn_cast<Instruction>(U);
          if (!I)
            continue;
          IRBuilder<> B(I);
          Value *New = nullptr;
          const APInt *C;
          CmpInst::Predicate P;
          if (match(I, m_c_And(m_Specific(Z), m_APInt(C)))) {
            // z & c with z in {0, 1}.
            New = (*C)[0] ? static_cast<Value *>(Z)
                          : Constant::getNullValue(I->getType());
          } else if (match(I, m_c_ICmp(P, m_Specific(Z), m_APInt(C))) &&
                     ICmpInst::isEquality(P) && C->ule(1)) {
            // z == 1 and z != 0 are b; z == 0 and z != 1 are !b.
            bool IsB = (P == ICmpInst::ICMP_NE) == C->isZero();
            New = IsB ? Bv : B.CreateNot(Bv);
          } else if (auto *T = dyn_cast<TruncInst>(I)) {
            New = T->getType()->getScalarType()->isIntegerTy(1)
                      ? Bv
                      : B.CreateZExt(Bv, T->getType());
          } else if (isa<ZExtInst>(I) || isa<SExtInst>(I)) {
            // z is 0 or 1, so its sign bit is clear.
            New = B.CreateZExt(Bv, I->getType());
          }
          if (!New)
            continue;
          I->replaceAllUsesWith(New);
          I->eraseFromParent();
          Changed = true;
          if (auto *NZ = dyn_cast<ZExtInst>(New); NZ && NZ != Z)
            Work.push_back(NZ);
        }
      }
      if (Z->use_empty())
        Z->eraseFromParent();
    }
  }

  Function &F;
  SecretAnalysis SA;
  DenseMap<Value *, Uses> RootUses;
  SetVector<Value *> Candidates;
  DenseMap<Value *, Value *> BoolOf;
};

} // namespace

PreservedAnalyses MPCNarrowBoolPass::run(Module &M, ModuleAnalysisManager &) {
  Expected<SecretSpec> Spec = SecretSpec::load(MetadataFilePath, M);
  if (!Spec)
    report_fatal_error(Twine("mpc-narrow-bool: ") + toString(Spec.takeError()),
                       false);
  expandLegacyFill(M);
  bool Changed = false;
  for (auto &[Fn, FS] : Spec->Funcs)
    if (FS.hasSecretArgs())
      Changed |= BoolNarrowing(const_cast<Function &>(*Fn), FS).run();
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace smaug
