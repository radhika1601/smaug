#pragma once
#include "llvm/IR/IRBuilder.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Intrinsics.h"

#include <map>

// int64_t mangles as `long` (l) on Linux and `long long` (x) on macOS.
#ifndef MPC_I64
#ifdef __APPLE__
#define MPC_I64 "x"
#else
#define MPC_I64 "l"
#endif
#endif

using namespace llvm;

class CheckSecretShared;

class VectorMPCLinkPass : public llvm::PassInfoMixin<VectorMPCLinkPass> {
private:
  CheckSecretShared *checkSecretShared;
  SmallDenseSet<Instruction *> freeLater;
  SmallDenseMap<Instruction *, Value *> identityInstrs;
  std::map<Value *, Value *> argToMPCtype, writeArgtoMPCtype, PtrtoMPCPtr;
  bool gc = true;
  Instruction *getOperandPtr(Value *op, Instruction *induction, Value *opCount,
                             IRBuilder<> &Builder, BasicBlock *BB,
                             MapVector<Value *, Instruction *> &ItoPtr,
                             Loop *L);

  void updateIcmp(Instruction &I, Instruction *induction, Value *opCount,
                  IRBuilder<> &Builder,
                  SmallVector<Instruction *> &deleteInstructions,
                  MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateIntFuncs(Instruction &I, Instruction *induction, Value *opCount,
                      IRBuilder<> &Builder,
                      SmallVector<Instruction *> &deleteInstructions,
                      MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateDiv(Instruction &I, Instruction *induction, Value *opCount,
                 IRBuilder<> &Builder,
                 SmallVector<Instruction *> &deleteInstructions,
                 MapVector<Value *, Instruction *> &ItoPtr, std::string name,
                 Loop *L);
  void updatePHI(Instruction &I, Instruction *induction, Value *opCount,
                 IRBuilder<> &Builder,
                 SmallVector<Instruction *> &deleteInstructions,
                 MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateType(Instruction &I, Instruction *induction, Value *opCount,
                  IRBuilder<> &Builder,
                  SmallVector<Instruction *> &deleteInstructions,
                  MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateExtract(ExtractElementInst &I, Instruction *induction,
                     Value *opCount, IRBuilder<> &Builder,
                     SmallVector<Instruction *> &deleteInstructions,
                     MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateNot(Instruction &I, Instruction *induction, Value *opCount,
                 IRBuilder<> &Builder,
                 SmallVector<Instruction *> &deleteInstructions,
                 MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateSelect(Instruction &I, Instruction *induction, Value *opCount,
                    IRBuilder<> &Builder,
                    SmallVector<Instruction *> &deleteInstructions,
                    MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  void updateReduction(CallInst *CI, Value *ptr, Value *opCount,
                       IRBuilder<> &Builder,
                       SmallVector<Instruction *> &deleteInstructions,
                       MapVector<Value *, Instruction *> &ItoPtr, Loop *L);
  Instruction *getResPtr(Instruction *I, Instruction *induction, Value *opCount,
                         IRBuilder<> &Builder, Function *F,
                         SmallVector<Instruction *> &deleteInstructions,
                         Loop *L);

  bool isValidIndex(Instruction *index, Instruction *induction, Loop *L,
                    Value *opCount) {
    if (index == induction)
      return true;
    for (auto itr = index->op_begin(); itr != index->op_end(); ++itr) {
      Instruction *op = dyn_cast<Instruction>(*itr);
      if (op == nullptr)
        continue;
      if (op == induction)
        continue;
      if (isa<Constant>(op))
        continue;
      if (CallInst *ci = dyn_cast<CallInst>(op)) {
        std::string name = ci->getCalledFunction()->getName().str();
        if (name.find("llvm.vscale") != std::string::npos) {
          // ci->replaceUsesWithIf(opCount, [ci, L](Use &U) {
          //   Instruction *I = dyn_cast<Instruction>(U);
          //   L->contains(I->getParent());
          //   errs() << formatv("vscale: {0}, user: {1}\n", *ci, *I);
          //   return true;
          // });
          ci->replaceAllUsesWith(opCount);
          continue;
        }
      }
      if (L->contains(op->getParent())) {
        if (isa<PHINode>(op) && op->getParent() == L->getHeader())
          return false;
        if (!isValidIndex(op, induction, L, opCount))
          return false;
      }
    }
    return true;
  }
  //   Value *i1Toi8(Value *v, IRBuilder<> &Builder, Type *boolTy);
  //   Value *i8Toi1(Value *v, IRBuilder<> &Builder, Type *i1Ty);
  //   Value *isAlice(IRBuilder<> &Builder, Module *module, Function *F);
  //   bool findFunc(Instruction &I, std::string name,
  //                 SmallVector<Argument *> &Args);
  //   bool selectCall(Instruction &I);
  //   bool icmpCall(Instruction &I, SmallVector<Argument *> &Args);
  //   void replaceFunc(Function &F, SmallVector<Argument *> &Args);
  //   bool publicXor(Instruction &I, SmallVector<Argument *> &Args);
  //   bool updatePrivateStore(Instruction &I);
  //   bool updatePrivateLoad(Instruction &I);
  //   void setSecretShared(Value *val);
  void updateInst(Instruction &I, Instruction *induction, Value *opCount,
                  IRBuilder<> &Builder,
                  SmallVector<Instruction *> &deleteInstructions,
                  MapVector<Value *, Instruction *> &ItoPtr, Loop *L);

  void deleteInsts(SmallVector<Instruction *> &deleteInstructions,
                   Instruction *induction, Value *opCount, IRBuilder<> &Builder,
                   MapVector<Value *, Instruction *> &ItoPtr, Loop *L);

  void setSecretShared(Value *val) {

    Instruction *I = dyn_cast<Instruction>(val);
    if (!I)
      return;
    Function *F = I->getParent()->getParent();
    auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
    auto *node = MDNode::get(F->getContext(), MDStr);
    I->setMetadata("secret_shared", node);
  }

  Value *makeSecretShared(Value *val, IRBuilder<> &Builder, Module *module,
                          Function *F) {
    if (isa<ScalableVectorType>(val->getType())) {
      if (auto ConstantVector = dyn_cast<Constant>(val)) {
        Value *v1 = ConstantVector->getOperand(0);
        if (v1) {
          if (auto C = dyn_cast<ConstantInt>(v1))
            val = C;
          else if (auto C = dyn_cast<Constant>(v1)) {
            if (auto CI = dyn_cast<ConstantInt>(C->getOperand(1)))
              val = CI;
          } else {
            errs() << "constant vector but could not extract aggregate value "
                   << *(ConstantVector->getAggregateElement(0U)) << "\n";
          }
        }
      }
    }
    if(gc)
      return val;
    auto v = isAlice(Builder, module);
    // errs() << *v << " " << *val << "\n";
    v = Builder.CreateSExt(v, val->getType());
    val = Builder.CreateAnd(val, v);
    setSecretShared(val);
    return val;
  }

  Instruction *isLoadStoreAtInduction(Instruction *Ld, Instruction *induction,
                                      Loop *L, Value *opCount);
  Value *isAlice(IRBuilder<> &Builder, Module *module);
  void replaceLoop(Loop *L, PHINode *induction, Value *opCount);
  std::pair<Value *, Value *> getOpCount(Loop *l, PHINode *induction);
  bool isVectorized(Loop *L) {
    for (BasicBlock *BB : L->blocks())
      for (Instruction &I : *BB) {
        if (I.getType()->isScalableTy())
          return true;
        else if (I.getOpcode() == Instruction::Store &&
                 I.getOperand(0)->getType()->isScalableTy())
          return true;
      }
    return false;
  }

  std::string getTypeName(Type *type, IRBuilder<> &Builder) {
    ScalableVectorType *scalableType = dyn_cast<ScalableVectorType>(type);
    if (scalableType->getElementType() == Builder.getInt1Ty()) {
      return "1";
    } else if (scalableType->getElementType() == Builder.getInt8Ty()) {
      return "8";
    } else if (scalableType->getElementType() == Builder.getInt16Ty()) {
      return "16";
    } else if (scalableType->getElementType() == Builder.getInt32Ty()) {
      return "32";
    } else if (scalableType->getElementType() == Builder.getInt64Ty()) {
      return "64";
    } else if (scalableType->getElementType() == Builder.getFloatTy()) {
      return "f";
    } else {
      return "";
    }
  }
  std::map<int, std::string> MPCTypeFuncNames = {
      {1, "_ZN3MPC9getBitPtrEPbi"},
      {8, "_ZN3MPC9getIntPtrIaEEPN3emp7IntegerEPT_i"},
      {16, "_ZN3MPC9getIntPtrIsEEPN3emp7IntegerEPT_i"},
      {32, "_ZN3MPC9getIntPtrIiEEPN3emp7IntegerEPT_i"},
      {64, "_ZN3MPC9getIntPtrI" MPC_I64 "EEPN3emp7IntegerEPT_i"},
  };

  std::map<std::string, std::string> GCFuncNames = {
      {"createInt", "_ZN3MPC9createIntE" MPC_I64},
      {"createBit", "_ZN3MPC9createBitE" MPC_I64},
      {"xor1", "_ZN3MPC6xorBitEPN3emp3BitES2_S2_i"},
      {"xor8", "_ZN3MPC6xorIntEPN3emp7IntegerES2_S2_i"},
      {"xor16", "_ZN3MPC6xorIntEPN3emp7IntegerES2_S2_i"},
      {"xor32", "_ZN3MPC6xorIntEPN3emp7IntegerES2_S2_i"},
      {"xor64", "_ZN3MPC6xorIntEPN3emp7IntegerES2_S2_i"},
      {"and1", "_ZN3MPC6andBitEPN3emp3BitES2_S2_i"},
      {"and8", "_ZN3MPC6andIntEPN3emp7IntegerES2_S2_i"},
      {"and16", "_ZN3MPC6andIntEPN3emp7IntegerES2_S2_i"},
      {"and32", "_ZN3MPC6andIntEPN3emp7IntegerES2_S2_i"},
      {"and64", "_ZN3MPC6andIntEPN3emp7IntegerES2_S2_i"},
      {"add8", "_ZN3MPC3addEPN3emp7IntegerES2_S2_i"},
      {"add16", "_ZN3MPC3addEPN3emp7IntegerES2_S2_i"},
      {"add32", "_ZN3MPC3addEPN3emp7IntegerES2_S2_i"},
      {"add64", "_ZN3MPC3addEPN3emp7IntegerES2_S2_i"},
      {"icmpEq8", "_ZN3MPC4icmpEPN3emp7IntegerES2_PNS0_3BitEii"},
      {"icmpEq16", "_ZN3MPC4icmpEPN3emp7IntegerES2_PNS0_3BitEii"},
      {"icmpEq32", "_ZN3MPC4icmpEPN3emp7IntegerES2_PNS0_3BitEii"},
      {"icmpEq64", "_ZN3MPC4icmpEPN3emp7IntegerES2_PNS0_3BitEii"},
      {"mul8", "_ZN3MPC4multEPN3emp7IntegerES2_S2_i"},
      {"mul16", "_ZN3MPC4multEPN3emp7IntegerES2_S2_i"},
      {"mul32", "_ZN3MPC4multEPN3emp7IntegerES2_S2_i"},
      {"mul64", "_ZN3MPC4multEPN3emp7IntegerES2_S2_i"},
      {"sub8", "_ZN3MPC3subEPN3emp7IntegerES2_S2_i"},
      {"sub16", "_ZN3MPC3subEPN3emp7IntegerES2_S2_i"},
      {"sub32", "_ZN3MPC3subEPN3emp7IntegerES2_S2_i"},
      {"sub64", "_ZN3MPC3subEPN3emp7IntegerES2_S2_i"},
      {"select", "_ZN3MPC6selectEPN3emp7IntegerES2_PNS0_3BitES2_i"},
      {"select1", "_ZN3MPC6selectEPN3emp3BitES2_S2_S2_i"},

      {"reduction", "_ZN3MPC9reductionEPviiS0_ib"},
      {"div", "_ZN3MPC6divideEPN3emp7IntegerES2_S2_i"},
      {"smax", "_ZN3MPC3maxEPN3emp7IntegerES2_S2_i"},
      {"smin", "_ZN3MPC3minEPN3emp7IntegerES2_S2_i"},
      // {"type", "_ZN3MPC10updateTypeEPvS0_iii"},
      // {"storeConst", "_ZN3MPC10storeConstEPvPaii"},
      // {"fadd", "_ZN3MPC4addFEPfS0_S0_i"},
      // {"fmul", "_ZN3MPC5multFEPfS0_S0_i"},
      // {"fdiv", "_ZN3MPC4divFEPfS0_S0_i"},
      // {"fsub", "_ZN3MPC4subFEPfS0_S0_i"},
      // {"fcmp", "_ZN3MPC6fcmpEqEPfS0_Pbii"},
      {"not1", "_ZN3MPC6notBitEPN3emp3BitES2_i"},
      {"not", "_ZN3MPC6notIntEPN3emp7IntegerES2_i"},
      {"init", "_ZN3MPC5storeEPvS0_iibb"},
      {"destroyInt", "_ZN3MPC7destroyEPN3emp7IntegerE"},
      {"destroyBit", "_ZN3MPC7destroyEPN3emp3BitE"},
      {"init1", "_ZN3MPC6getBitEb"},
      {"init8", "_ZN3MPC10getIntegerEa"},
      {"init16", "_ZN3MPC10getIntegerEs"},
      {"init32", "_ZN3MPC10getIntegerEi"},
      {"init64", "_ZN3MPC10getIntegerE" MPC_I64},
};

  std::map<std::string, std::string> FuncNames = {
      {"xor1", "_ZN3MPC7xorBoolEPbS0_S0_ib"},
      {"xor8", "_ZN3MPC5xorI8EPaS0_S0_ib"},
      {"xor16", "_ZN3MPC6xorI16EPsS0_S0_ib"},
      {"xor32", "_ZN3MPC6xorI32EPiS0_S0_ib"},
      {"xor64", "_ZN3MPC6xorI64EP" MPC_I64 "S0_S0_ib"},
      {"and1", "_ZN3MPC7andBoolEPbS0_S0_ib"},
      {"add8", "_ZN3MPC5addI8EPaS0_S0_i"},
      {"add16", "_ZN3MPC6addI16EPsS0_S0_i"},
      {"add32", "_ZN3MPC6addI32EPiS0_S0_i"},
      {"add64", "_ZN3MPC6addI64EP" MPC_I64 "S0_S0_i"},
      {"and1", "_ZN3MPC7andBoolEPbS0_S0_ib"},
      {"and8", "_ZN3MPC5andI8EPaS0_S0_ib"},
      {"and16", "_ZN3MPC6andI16EPsS0_S0_ib"},
      {"and32", "_ZN3MPC6andI32EPiS0_S0_ib"},
      {"and64", "_ZN3MPC6andI64EP" MPC_I64 "S0_S0_ib"},
      {"icmpEq8", "_ZN3MPC8icmpEqI8EPaS0_Pbii"},
      {"icmpEq16", "_ZN3MPC9icmpEqI16EPsS0_Pbii"},
      {"icmpEq32", "_ZN3MPC9icmpEqI32EPiS0_Pbii"},
      {"icmpEq64", "_ZN3MPC9icmpEqI64EP" MPC_I64 "S0_Pbii"},
      {"mul8", "_ZN3MPC6multI8EPaS0_S0_i"},
      {"mul16", "_ZN3MPC7multI16EPsS0_S0_i"},
      {"mul32", "_ZN3MPC7multI32EPiS0_S0_i"},
      {"mul64", "_ZN3MPC7multI64EP" MPC_I64 "S0_S0_i"},
      {"sub8", "_ZN3MPC5subI8EPaS0_S0_i"},
      {"sub16", "_ZN3MPC6subI16EPsS0_S0_i"},
      {"sub32", "_ZN3MPC6subI32EPiS0_S0_i"},
      {"sub64", "_ZN3MPC6subI64EP" MPC_I64 "S0_S0_i"},
      {"reduction", "_ZN3MPC9reductionEPviiS0_ib"},
      {"select", "_ZN3MPC6selectEPvS0_PbiiS0_b"},
      {"init", "_ZN3MPC5storeEPvS0_iibb"},
      {"div", "_ZN3MPC6divideEPvS0_iiS0_b"},
      {"smax", "_ZN3MPC3maxEPvS0_iiS0_b"},
      {"smin", "_ZN3MPC3minEPvS0_iiS0_b"},
      {"type", "_ZN3MPC10updateTypeEPvS0_iii"},
      {"storeConst", "_ZN3MPC10storeConstEPvPaii"},
      {"fadd", "_ZN3MPC4addFEPfS0_S0_i"},
      {"fmul", "_ZN3MPC5multFEPfS0_S0_i"},
      {"fdiv", "_ZN3MPC4divFEPfS0_S0_i"},
      {"fsub", "_ZN3MPC4subFEPfS0_S0_i"},
      {"fcmp", "_ZN3MPC6fcmpEqEPfS0_Pbii"}};

  Instruction *createOutput(Instruction *output, Function *F,
                            IRBuilder<> &Builder, Type *type) {
    if (output == nullptr) {
      Builder.SetInsertPoint(F->getEntryBlock().getFirstNonPHI());
      if (type == Type::getInt1Ty(F->getContext()))
        type = Type::getInt8Ty(F->getContext());
      return Builder.CreateAlloca(type, ConstantInt::get(type, 1), "");
    }
    return output;
  }

  Instruction *makeSecretSharedVec(Value *val, Value *opCount,
                                   IRBuilder<> &Builder, BasicBlock *BB,
                                   bool usedAsShared = true);

  Value *getScalarLoop(Loop *VectorLoop, LoopInfo &LI,
                       SmallVector<BasicBlock *> &deleteBBs,
                       PHINode *induction) {

    auto exitBlock = VectorLoop->getExitBlock();
    auto term = exitBlock->getTerminator();
    LLVMContext &context = exitBlock->getContext();
    BasicBlock *Preheader = VectorLoop->getLoopPreheader();
    // this checks if the remainder with vscale is 0 or no
    Value *urem = nullptr;
    BasicBlock *next = nullptr;
    BasicBlock *scalarPh = nullptr;
    if (ICmpInst *icmp = dyn_cast<ICmpInst>(term->getOperand(0))) {
      if (icmp->isEquality()) {
        Value *op0 = icmp->getOperand(0);
        Constant *cop0 = dyn_cast<Constant>(op0);
        if (cop0 && cop0->isZeroValue())
          urem = icmp->getOperand(1);
        else
          urem = icmp->getOperand(0);

        BranchInst *Bterm = dyn_cast<BranchInst>(term);
        next = Bterm->getSuccessor(0);
        scalarPh = Bterm->getSuccessor(1);
      } else {
        // TODO: handle
        errs() << "Handle non equality check in the vectorized loop exit\n";
        return nullptr;
      }
    } else {
      errs() << "scalar ph not found\n";
      return nullptr;
    }
    auto itrCount = getOpCount(VectorLoop, induction).first;
    if (Preheader != nullptr) {
      for (Instruction &I : *Preheader) {
        if (CallInst *ci = dyn_cast<CallInst>(&I)) {
          std::string name = ci->getCalledFunction()->getName().str();
          if (name.find("llvm.vscale") != std::string::npos) {
            Value *replacement = itrCount;
            if (ci->getType() != itrCount->getType()) {
              IRBuilder<> B(ci);
              replacement = B.CreateTruncOrBitCast(itrCount, ci->getType());
            }
            ci->replaceAllUsesWith(replacement);
          }
        }
      }
    }
    for (BasicBlock *pred : predecessors(scalarPh)) {
      for (Instruction &I : *pred) {
        if (CallInst *ci = dyn_cast<CallInst>(&I)) {
          std::string name = ci->getCalledFunction()->getName().str();
          if (name.find("llvm.vscale") != std::string::npos) {
            Value *replacement = itrCount;
            if (ci->getType() != itrCount->getType()) {
              IRBuilder<> B(ci);
              replacement = B.CreateTruncOrBitCast(itrCount, ci->getType());
            }
            ci->replaceAllUsesWith(replacement);
          }
        }
      }
      BranchInst *bterm = dyn_cast<BranchInst>(pred->getTerminator());
      if (bterm && bterm->isConditional()) {
        bool c = bterm->getSuccessor(0) == scalarPh ? false : true;
        bterm->setCondition(ConstantInt::getBool(Type::getInt1Ty(context), c));
      } else {
        errs() << "scalar ph predecessor non-branch or unconditional branch\n";
      }
    }

    if (BranchInst *bterm = dyn_cast<BranchInst>(term)) {
      if (bterm->getSuccessor(0) == next)
        bterm->setCondition(
            ConstantInt::getBool(Type::getInt1Ty(context), true));
      else
        bterm->setCondition(
            ConstantInt::getBool(Type::getInt1Ty(context), false));
    }

    // errs() << "scalarPH" << *scalarPh << "\n";
    // errs() << "next " << *next << "\n";
    // auto ScalarLH =
    //     dyn_cast<BasicBlock>(scalarPh->getTerminator()->getOperand(0));
    // if (Loop *L = LI.getLoopFor(ScalarLH)) {
    //   for (auto *BB : L->blocks()) {
    //     deleteBBs.push_back(BB);
    //   }
    //   if (L->getExitBlock() != next) {
    //     errs() << "not handled exitblock of scalar loop not same as
    //     identified "
    //            << "by the exit block of vectorized loop\n";
    //   }
    // }
    errs() << *itrCount << "\n";
    return itrCount;
  }

  void insertPtrToMPCPtr(Value *ptr1, Value *ptr2) {
    errs() << *ptr1 << " " << *ptr2;
    while (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ptr1))
      ptr1 = gep->getOperand(0);
    Instruction *a = dyn_cast<Instruction>(ptr1);
    if (a && !isa<AllocaInst>(a) && !isa<CallInst>(a))
      return;
    if (!ptr1->hasName())
      ptr1->setName("ptr");
    PtrtoMPCPtr.insert(std::make_pair(ptr1, ptr2));
    ptr2->setName(ptr1->getName() + ".mpc");
  }

  Instruction *createMalloc(IRBuilder<> &Builder, Function *F, Type *type,
                            Value *n, bool isSecShared = false,
                            std::string name = "") {

    llvm::Function *mallocFunc;
    if (gc) {
      std::string name = "createInt";
      Type *scalarType = type->getScalarType();
      if (scalarType == Builder.getInt1Ty() || scalarType == Builder.getInt8Ty())
        name = "createBit";
      mallocFunc = cast<llvm::Function>(
          F->getParent()
              ->getOrInsertFunction(GCFuncNames[name],
                                    FunctionType::get(Builder.getPtrTy(),
                                                      {Builder.getInt64Ty()},
                                                      false))
              .getCallee());
    } else {
      mallocFunc = cast<llvm::Function>(
          F->getParent()
              ->getOrInsertFunction("malloc",
                                    FunctionType::get(Builder.getPtrTy(),
                                                      Builder.getInt64Ty(),
                                                      false))
              .getCallee());
      Type *allocType = type->getScalarType();
      uint64_t elementSize =
          F->getParent()->getDataLayout().getTypeAllocSize(allocType);
      // Compute byte count at n's definition point (not the current builder
      // position) so the mul+zext dominate everywhere n dominates. This
      // avoids dominance violations when the builder is inside a loop body
      // but n is defined in a dominating block.
      auto savedIP = Builder.saveIP();
      if (Instruction *nInst = dyn_cast<Instruction>(n))
        Builder.SetInsertPoint(nInst->getInsertionPointAfterDef().value());
      n = Builder.CreateMul(n, ConstantInt::get(n->getType(), elementSize));
      n = Builder.CreateZExtOrBitCast(n, Builder.getInt64Ty());
      Builder.restoreIP(savedIP);
    }
    auto ptr = Builder.CreateCall(mallocFunc, n, name + ".ptr");
    Instruction *ptrInst = dyn_cast<Instruction>(ptr);
    if (isSecShared) {
      auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
      auto *node = MDNode::get(F->getContext(), MDStr);
      ptrInst->setMetadata("secret_shared", node);
    }
    if (gc) {
      auto name = getTypeMetadata(type);
      if (name != "") {
        auto *MDStr = llvm::MDString::get(F->getContext(), name);
        auto *node = MDNode::get(F->getContext(), MDStr);
        ptrInst->setMetadata(name, node);
      }
      // Initialize Integer buffers with zero values of the correct bit width.
      // createIntEl allocates Integer objects with 0 bits by default;
      // uninitialized Integers cause assertion failures in select/compare.
      Type *scalarType = type->getScalarType();
      if (scalarType != Builder.getInt1Ty() &&
          scalarType != Builder.getInt8Ty()) {
        int elSize = F->getParent()->getDataLayout().getTypeAllocSize(scalarType);
        Value *initVal = Builder.CreateAlloca(scalarType, Builder.getInt32(1));
        Builder.CreateStore(ConstantInt::get(scalarType, 0), initVal);
        Value *n32 = Builder.CreateTruncOrBitCast(n, Builder.getInt32Ty());
        auto *initFunc = cast<Function>(
            F->getParent()
                ->getOrInsertFunction(
                     GCFuncNames["init"],
                     FunctionType::get(Builder.getVoidTy(),
                                       {Builder.getPtrTy(), Builder.getPtrTy(),
                                        Builder.getInt32Ty(), Builder.getInt32Ty(),
                                        Builder.getInt1Ty(), Builder.getInt1Ty()},
                                       false))
                .getCallee());
        Builder.CreateCall(initFunc, {ptrInst, initVal, n32,
                                      Builder.getInt32(elSize),
                                      Builder.getInt1(false),
                                      Builder.getInt1(false)});
      }
    }
    return ptrInst;
  }

  std::string getTypeMetadata(Type *type) {
    if (auto *vecTy = dyn_cast<ScalableVectorType>(type))
      type = vecTy->getElementType();
    if (IntegerType *t = dyn_cast<IntegerType>(type)) {
      switch (t->getBitWidth()) {
      case 8:
        return "int8";
      case 16:
        return "int16";
      case 32:
        return "int32";
      case 64:
        return "int64";
      default:
        break;
      }
    }
    if (type->isFloatTy())
      return "float";
    if (type->isDoubleTy())
      return "double";
    return "";
  }

  // Loop *getScalarLoop(Loop *VectorLoop, LoopInfo &LI,
  //                     llvm::LLVMContext &context) {
  //   auto LoopID = VectorLoop->getLoopID();
  //   if (MDString *S = dyn_cast<MDString>(LoopID->getOperand(0))) {
  //     errs() << "as string" << *S << "\n";
  //   }
  //   for (unsigned i = 0, e = LoopID->getNumOperands(); i < e; ++i) {
  //     if (MDNode *SubNode = dyn_cast<MDNode>(LoopID->getOperand(i))) {
  //       errs() << *LoopID << "\n";
  //       if (MDString *S = dyn_cast<MDString>(SubNode->getOperand(0))) {
  //         if (S->getString().contains("llvm.loop.isvectorized")) {
  //           Value *LoopIdVal = MetadataAsValue::get(context, LoopID);
  //           // if (ConstantInt *ScalarLoopOffsetConst =
  //           //         dyn_cast<ConstantInt>(SubNode->getOperand(1))) {
  //           //   uint64_t ScalarLoopOffset =
  //           //   ScalarLoopOffsetConst->getZExtValue();
  //           // uint64_t VectorLoopID = ; // Assuming LoopID has a unique hash
  //           // method
  //           // uint64_t VectorLoopID = LoopID->getMetadataID();
  //           // for (Loop *L : LI)
  //           //   if (auto LID = L->getLoopID()) {
  //           //     uint64_t ScalarLoopID = VectorLoopID + ScalarLoopOffset;
  //           //     if (LID->getMetadataID() == ScalarLoopID) {
  //           //       return L;
  //           //     }
  //           //   }
  //           // }
  //         }
  //       }
  //     }
  //   }
  //   return nullptr;
  // }
  //   bool isSecretShared(Value *val, SmallVector<Argument *>
  //   &Args); void updatePhi(Instruction *, SmallVector<Argument
  //   *> &Args);
public:
  llvm::PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
