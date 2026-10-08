#include "VectorLoops.h"

#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;
using namespace llvm::PatternMatch;

namespace smaug {

namespace {

bool isVScale(const Value *V) {
  if (const auto *Cast = dyn_cast<CastInst>(V))
    V = Cast->getOperand(0);
  if (const auto *II = dyn_cast<IntrinsicInst>(V))
    return II->getIntrinsicID() == Intrinsic::vscale;
  return false;
}

bool hasScalable(const Instruction &I) {
  if (isa<ScalableVectorType>(I.getType()))
    return true;
  for (const Value *Op : I.operands())
    if (isa<ScalableVectorType>(Op->getType()))
      return true;
  return false;
}

struct Candidate {
  Loop *L = nullptr;
  VectorRegion R;
  BasicBlock *ScalarPh = nullptr;
  SmallPtrSet<BasicBlock *, 8> Blocks; // every block where vscale means TC
};

class Canonicalizer {
public:
  Canonicalizer(Function &F, std::vector<Diagnostic> &Diags)
      : F(F), Diags(Diags), DT(F), LI(DT) {}

  std::vector<VectorRegion> run();

private:
  bool fail(const Instruction &I, const Twine &Msg) {
    Diags.push_back({&I, Msg.str()});
    return false;
  }
  bool match(Loop *L, Candidate &C);
  void collectBlocks(Candidate &C);
  bool replaceVScale(const std::vector<Candidate> &Cands);
  void rewrite(Candidate &C);
  bool fold(const std::vector<Candidate> &Cands);

  Function &F;
  std::vector<Diagnostic> &Diags;
  DominatorTree DT;
  LoopInfo LI;
};

bool Canonicalizer::match(Loop *L, Candidate &C) {
  BasicBlock *Body = L->getHeader();
  Instruction &First = *Body->getFirstNonPHIOrDbg();
  if (L->getNumBlocks() != 1 || L->getLoopLatch() != Body)
    return fail(First, "V2: a vectorized loop body must be a single block");
  BasicBlock *PH = L->getLoopPreheader(), *Middle = L->getExitBlock();
  if (!PH || !Middle || !Middle->getSinglePredecessor())
    return fail(First, "V2: a vectorized loop needs a unique preheader and "
                       "middle block");

  // V4: br (icmp eq index.next, n.vec), middle, body.
  auto *Br = dyn_cast<BranchInst>(Body->getTerminator());
  Value *IndexNext, *NVec;
  ICmpInst::Predicate Pred;
  if (!Br || !Br->isConditional() || Br->getSuccessor(0) != Middle ||
      !PatternMatch::match(Br->getCondition(),
                           m_ICmp(Pred, m_Value(IndexNext), m_Value(NVec))) ||
      Pred != ICmpInst::ICMP_EQ)
    return fail(*Body->getTerminator(),
                "V4: the latch must be br (icmp eq index.next, n.vec), "
                "middle, body");

  // V3: index = phi [0, ph], [index.next, body]; index.next = index + vscale.
  PHINode *Index = nullptr;
  for (PHINode &PN : Body->phis()) {
    if (isa<ScalableVectorType>(PN.getType()) || PN.getType()->isPointerTy())
      continue;
    if (Index)
      return fail(PN, "V3: a vectorized loop may have only one scalar "
                      "induction variable");
    Index = &PN;
  }
  Value *Step;
  if (!Index || !PatternMatch::match(Index->getIncomingValueForBlock(PH), m_Zero()) ||
      Index->getIncomingValueForBlock(Body) != IndexNext ||
      !PatternMatch::match(IndexNext, m_c_Add(m_Specific(Index), m_Value(Step))) ||
      !isVScale(Step))
    return fail(First, "V3: the induction variable must start at 0 and step "
                       "by vscale");

  // V5: n.vec = sub TC, (urem TC, vscale).
  Value *TC, *Rem;
  if (!PatternMatch::match(NVec, m_Sub(m_Value(TC), m_Value(Rem))) ||
      !PatternMatch::match(Rem, m_URem(m_Specific(TC), m_Value(Step))) ||
      !isVScale(Step))
    return fail(*Br, "V5: n.vec must be TC - TC % vscale");

  // V6: TC is defined outside the loop.
  if (auto *TCI = dyn_cast<Instruction>(TC); TCI && L->contains(TCI))
    return fail(*TCI, "V6: the trip count must be defined outside the loop");

  C.L = L;
  C.R.Preheader = PH;
  C.R.Body = Body;
  C.R.Middle = Middle;
  C.R.TripCount = TC;
  if (auto *MBr = dyn_cast<BranchInst>(Middle->getTerminator());
      MBr && MBr->isConditional())
    C.ScalarPh = MBr->getSuccessor(1);
  return true;
}

// The blocks where vscale stands for this loop's trip count: the vector
// preheader, body and middle block, and the guard blocks above the
// preheader that branch to the scalar loop instead.
void Canonicalizer::collectBlocks(Candidate &C) {
  C.Blocks.insert(C.R.Preheader);
  C.Blocks.insert(C.R.Body);
  C.Blocks.insert(C.R.Middle);
  SmallVector<BasicBlock *> Work{C.R.Preheader};
  while (!Work.empty()) {
    BasicBlock *BB = Work.pop_back_val();
    for (BasicBlock *P : predecessors(BB)) {
      if (C.Blocks.count(P) || !C.ScalarPh)
        continue;
      bool Guard = false;
      for (BasicBlock *S : successors(P))
        Guard |= S == C.ScalarPh;
      if (Guard && C.Blocks.insert(P).second)
        Work.push_back(P);
    }
  }
}

bool Canonicalizer::replaceVScale(const std::vector<Candidate> &Cands) {
  SmallVector<IntrinsicInst *> Calls;
  for (Instruction &I : instructions(F))
    if (auto *II = dyn_cast<IntrinsicInst>(&I);
        II && II->getIntrinsicID() == Intrinsic::vscale)
      Calls.push_back(II);
  bool Ok = true;
  for (IntrinsicInst *VS : Calls) {
    SmallVector<Use *> Uses;
    for (Use &U : VS->uses())
      Uses.push_back(&U);
    for (Use *U : Uses) {
      auto *User = cast<Instruction>(U->getUser());
      BasicBlock *BB = isa<PHINode>(User)
                           ? cast<PHINode>(User)->getIncomingBlock(*U)
                           : User->getParent();
      const Candidate *Owner = nullptr;
      for (const Candidate &C : Cands)
        if (C.Blocks.count(BB))
          Owner = &C;
      if (!Owner) {
        Ok = fail(*User, "vscale used outside a vectorized loop");
        continue;
      }
      Value *TC = Owner->R.TripCount;
      if (auto *TCI = dyn_cast<Instruction>(TC); TCI && !DT.dominates(TCI, *U)) {
        Ok = fail(*User, "V6: the trip count does not dominate a use of "
                         "vscale");
        continue;
      }
      Instruction *At = isa<PHINode>(User)
                            ? cast<PHINode>(User)->getIncomingBlock(*U)->getTerminator()
                            : User;
      IRBuilder<> B(At);
      U->set(B.CreateZExtOrTrunc(TC, VS->getType()));
    }
  }
  return Ok;
}

void Canonicalizer::rewrite(Candidate &C) {
  BasicBlock *Body = C.R.Body, *PH = C.R.Preheader;
  // The body runs once, so every header phi has its start value.
  SmallVector<PHINode *> Phis;
  for (PHINode &PN : Body->phis())
    Phis.push_back(&PN);
  for (PHINode *PN : Phis) {
    PN->replaceAllUsesWith(PN->getIncomingValueForBlock(PH));
    PN->eraseFromParent();
  }
  Instruction *Old = Body->getTerminator();
  BranchInst::Create(C.R.Middle, Old);
  Value *Cond = cast<BranchInst>(Old)->getCondition();
  Old->eraseFromParent();
  RecursivelyDeleteTriviallyDeadInstructions(Cond);

  // Split the preheader so the trip count and the region's allocations sit
  // in a block of their own ahead of all of the region's code.
  C.R.Setup = PH;
  C.R.Preheader = SplitBlock(PH, &*PH->getFirstInsertionPt());
  C.R.Preheader->setName("vector.ph.body");
  IRBuilder<> B(PH->getTerminator());
  C.R.TripCount = B.CreateZExtOrTrunc(C.R.TripCount, B.getInt64Ty(), "tc");
}

bool Canonicalizer::fold(const std::vector<Candidate> &Cands) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  SimplifyQuery SQ(DL);
  bool Changed = true;
  while (Changed) {
    Changed = false;
    SmallVector<Instruction *> Insts;
    for (Instruction &I : instructions(F))
      Insts.push_back(&I);
    for (Instruction *I : Insts) {
      if (I->getParent() == nullptr || I->isTerminator() || I->use_empty())
        continue;
      if (Value *V = simplifyInstruction(I, SQ)) {
        I->replaceAllUsesWith(V);
        Changed = true;
      }
    }
    // Collect first: deleting one instruction can delete others in Insts.
    SmallVector<WeakTrackingVH> Dead;
    for (Instruction *I : Insts)
      if (isInstructionTriviallyDead(I))
        Dead.push_back(I);
    RecursivelyDeleteTriviallyDeadInstructions(Dead);
    for (BasicBlock &BB : F)
      Changed |= ConstantFoldTerminator(&BB, /*DeleteDeadConditions=*/true);
  }
  removeUnreachableBlocks(F);
  bool Ok = true;
  for (const Candidate &C : Cands) {
    auto *MBr = dyn_cast<BranchInst>(C.R.Middle->getTerminator());
    if (!MBr || MBr->isConditional())
      Ok = fail(*C.R.Middle->getTerminator(),
                "V7: the scalar remainder after a vectorized loop is not "
                "provably dead");
  }
  return Ok;
}

std::vector<VectorRegion> Canonicalizer::run() {
  // V1: every scalable type has one element per vscale.
  bool Any = false, Ok = true;
  for (Instruction &I : instructions(F)) {
    if (!hasScalable(I))
      continue;
    Any = true;
    auto check = [&](Type *T) {
      if (auto *VT = dyn_cast<ScalableVectorType>(T); VT && VT->getMinNumElements() != 1)
        Ok = fail(I, "V1: only <vscale x 1 x T> vectors are supported");
    };
    check(I.getType());
    for (Value *Op : I.operands())
      check(Op->getType());
  }
  if (!Any || !Ok)
    return {};

  std::vector<Candidate> Cands;
  for (Loop *L : LI.getLoopsInPreorder()) {
    bool Vector = false;
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB)
        Vector |= hasScalable(I);
    if (!Vector || !L->isInnermost())
      continue;
    Candidate C;
    if (!match(L, C))
      return {};
    collectBlocks(C);
    Cands.push_back(std::move(C));
  }
  // Scalable values outside a vectorized loop region.
  for (Instruction &I : instructions(F)) {
    if (!hasScalable(I))
      continue;
    bool Inside = false;
    for (const Candidate &C : Cands)
      Inside |= C.Blocks.count(I.getParent()) > 0;
    if (!Inside)
      Ok = fail(I, "vector code outside a vectorized loop");
  }
  if (!Ok || !replaceVScale(Cands))
    return {};
  for (Candidate &C : Cands)
    rewrite(C);
  if (!fold(Cands))
    return {};
  std::vector<VectorRegion> Regions;
  for (Candidate &C : Cands)
    Regions.push_back(C.R);
  return Regions;
}

} // namespace

std::vector<VectorRegion> canonicalizeVectorLoops(Function &F,
                                                  std::vector<Diagnostic> &Diags) {
  return Canonicalizer(F, Diags).run();
}

} // namespace smaug
