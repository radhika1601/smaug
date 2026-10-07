#include "VectorMPCLink.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cmath>

#define DEBUG_TYPE "vector-mpc-link"
#include "CheckSecretShared.h"
#include "MPCVecUtils.h"

#include "Options.h"

extern cl::opt<std::string> MetadataFilePath;

std::pair<Value *, Value *> VectorMPCLinkPass::getOpCount(Loop *L,
                                                          PHINode *induction) {

  Instruction *latchIcmp = L->getLatchCmpInst();
  Instruction *indNext = dyn_cast<Instruction>(latchIcmp->getOperand(0));

  Value *vscale = nullptr;
  Value *opCount = nullptr;
  if (indNext->getOpcode() != Instruction::Add)
    return std::make_pair(opCount, vscale);

  if (ICmpInst *I = dyn_cast<ICmpInst>(latchIcmp))
    if (!I->isEquality())
      return std::make_pair(opCount, vscale);

  if (indNext->getOperand(0) == induction) {
    vscale = indNext->getOperand(1);
  } else {
    vscale = indNext->getOperand(0);
  }

  if (latchIcmp->getOperand(0) == indNext)
    opCount = latchIcmp->getOperand(1);
  else
    opCount = latchIcmp->getOperand(0);
  // errs() << formatv("before vscale {0}, opcount {1}, {2}\n", *vscale,
  // *opCount,
  //                   *L);
  while (isa<Instruction>(opCount)) {
    Instruction *opInst = dyn_cast<Instruction>(opCount);
    auto opcode = opInst->getOpcode();
    if (opcode == Instruction::ZExt || opcode == Instruction::Trunc ||
        opcode == Instruction::Freeze || opcode == Instruction::SExt) {
      opCount = opInst->getOperand(0);
    } else
      break;
  }
  Instruction *opInst = dyn_cast<Instruction>(opCount);
  if (opInst && opInst->getOpcode() == Instruction::Sub) {
    // if opcount currently can be n - n %vscale, set opcount = n
    auto n = opInst->getOperand(0);
    auto mod = dyn_cast<Instruction>(opInst->getOperand(1));
    if (mod->getOpcode() == Instruction::URem) {
      if (n == mod->getOperand(0)) {
        auto ci = dyn_cast<CallInst>(mod->getOperand(1));
        if (ci && ci->getCalledFunction()->getName().contains("llvm.vscale")) {
          opCount = n;
        }
      }
    }
  }
  // errs() << formatv("vscale {0}, opcount {1}\n", *vscale, *opCount);

  // errs() << formatv("opcount {0}\n", *opCount);
  return std::make_pair(opCount, vscale);
}

// bool isI1UsedOnlyAsI8(Instruction *I){
//     for(auto user: I->users())
// }

Value *VectorMPCLinkPass::isAlice(IRBuilder<> &Builder, Module *module) {
  llvm::FunctionType *funcType =
      FunctionType::get(Type::getInt1Ty(Builder.getContext()), {}, false);

  llvm::FunctionCallee isAliceFuncCallee =
      module->getOrInsertFunction("_ZN3MPC7isAliceEv", funcType);
  Function *isAliceFunc = llvm::cast<Function>(isAliceFuncCallee.getCallee());
  Instruction *isAliceInst = Builder.CreateCall(isAliceFunc, {}, "isAlice");

  return isAliceInst;
}

Instruction *VectorMPCLinkPass::makeSecretSharedVec(Value *val, Value *opCount,
                                                    IRBuilder<> &Builder,
                                                    BasicBlock *BB,
                                                    bool usedAsShared) {

  if (val == nullptr) {
    LLVM_DEBUG(dbgs() << "nullptr\n");
    return nullptr;
  }
  Type *type = val->getType();
  if (auto *vecTy = dyn_cast<ScalableVectorType>(type)) {
    type = vecTy->getElementType();
  }

  if (Instruction *I = dyn_cast<Instruction>(val)) {
    if (I->getOpcode() == Instruction::Add) {
      if (Constant *c = dyn_cast<Constant>(I->getOperand(0))) {
        if (c->isZeroValue())
          val = I->getOperand(1);
      } else if (Constant *c = dyn_cast<Constant>(I->getOperand(1))) {
        if (c->isZeroValue())
          val = I->getOperand(0);
      }
    }
  }

  Module *module = BB->getParent()->getParent();
  const DataLayout &dataLayout = module->getDataLayout();
  Function *memsetFunc = Intrinsic::getDeclaration(
      module, Intrinsic::memset,
      {PointerType::get(type, 0), opCount->getType()});
  Builder.SetInsertPoint(BB->getFirstNonPHI());
  // Ensure insertion point is after both opCount and val definitions.
  // Without this, non-gc createMalloc (which emits mul+zext+call) and
  // subsequent store/init code can reference values not yet defined.
  for (Value *dep : {opCount, val}) {
    if (Instruction *depInst = dyn_cast<Instruction>(dep)) {
      if (depInst->getParent() == BB &&
          !depInst->comesBefore(&*Builder.GetInsertPoint()))
        Builder.SetInsertPoint(depInst->getInsertionPointAfterDef().value());
    }
  }
  uint64_t elementSize = dataLayout.getTypeAllocSize(type);
  Value *elementSizeValue = Builder.getInt64(elementSize);
  Value *allocSize = Builder.CreateMul(opCount, elementSizeValue, "allocSize");
  Instruction *newPtr;
  llvm::Function *storeFunc;
  newPtr =
      createMalloc(Builder, BB->getParent(), type, opCount,
                   checkSecretShared->isSecretShared(val, BB->getParent()));
  freeLater.insert(newPtr);
  if (!gc) {
    storeFunc = cast<llvm::Function>(
        module
            ->getOrInsertFunction(
                FuncNames["init"],
                FunctionType::get(Type::getVoidTy(module->getContext()),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getInt32Ty(), Builder.getInt32Ty(),
                                   Builder.getInt1Ty(), Builder.getInt1Ty()},
                                  false))
            .getCallee());
  } else {
    storeFunc = cast<llvm::Function>(
        module
            ->getOrInsertFunction(
                GCFuncNames["init"],
                FunctionType::get(Builder.getVoidTy(),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getInt32Ty(), Builder.getInt32Ty(),
                                   Builder.getInt1Ty(), Builder.getInt1Ty()},
                                  false))
            .getCallee());
  }

  if (Constant *C = dyn_cast<Constant>(val)) {
    if (C->isZeroValue()) {
      if (gc) {
        Value *elementPtr = Builder.CreateAlloca(type, Builder.getInt32(1), "");
        Builder.CreateStore(ConstantInt::get(type, 0), elementPtr);
        Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
        Builder.CreateCall(
            storeFunc, {newPtr, elementPtr, n, Builder.getInt32(elementSize),
                        Builder.getInt1(false), Builder.getInt1(false)});
        return newPtr;
      }
      Builder.CreateCall(memsetFunc, {newPtr, Builder.getInt8(0), allocSize,
                                      Builder.getInt1(false)});
      if (usedAsShared)
        setSecretShared(newPtr);
      return newPtr;
    } else {
      Value *val = makeSecretShared(C, Builder, module, BB->getParent());
      setSecretShared(newPtr);
      // need to create multiple stores instead of using memset because it works
      // at byte level
      if (!gc && type == Type::getInt8Ty(module->getContext())) {
        Builder.CreateCall(memsetFunc,
                           {newPtr, val, allocSize, Builder.getInt1(false)});
      } else if (!gc && type == Builder.getInt1Ty()) {
        val = Builder.CreateZExt(val, Builder.getInt8Ty());
        Builder.CreateCall(memsetFunc,
                           {newPtr, val, allocSize, Builder.getInt1(false)});
      } else {
        // Get or insert the malloc and free functions
        // errs() << "create vector for " << *val << "\n";Value *elementPtr;
        Value *elementPtr;
        if (gc) {
          std::string name = "init";
          Value *initVal = val;
          if (val->getType() == Builder.getInt1Ty()) {
            name += "1";
          } else if (val->getType() == Builder.getInt8Ty()) {
            // i8 boolean stored in Bit buffer: convert to i1 and use getBitEb
            name += "1";
            initVal = Builder.CreateICmpNE(val, ConstantInt::get(Builder.getInt8Ty(), 0));
          } else if (val->getType() == Builder.getInt16Ty())
            name += "16";
          else if (val->getType() == Builder.getInt32Ty())
            name += "32";
          else if (val->getType() == Builder.getInt64Ty())
            name += "64";
          Function *func = cast<llvm::Function>(
              module
                  ->getOrInsertFunction(GCFuncNames[name],
                                        FunctionType::get(Builder.getPtrTy(),
                                                          {initVal->getType()},
                                                          false))
                  .getCallee());
          elementPtr = Builder.CreateCall(func, {initVal});
        } else {
          elementPtr =
              Builder.CreateAlloca(val->getType(), Builder.getInt32(1), "");
          Builder.CreateStore(val, elementPtr);
        }
        Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
        // In both modes elementPtr holds a value of the buffer's own kind
        // (an MPC Bit/Integer in gc mode), so it is stored as shared.
        Builder.CreateCall(
            storeFunc, {newPtr, elementPtr, n, Builder.getInt32(elementSize),
                        Builder.getInt1(true), Builder.getInt1(false)});
      }
      return newPtr;
    }
  }

  if (usedAsShared)
    setSecretShared(newPtr);
  if (ShuffleVectorInst *shInst = dyn_cast<ShuffleVectorInst>(val)) {
    llvm::ArrayRef<int> shuffleMask = shInst->getShuffleMask();
    for (int i : shuffleMask) {
      if (i != 0) {
        LLVM_DEBUG(dbgs() << "shuffle vector has non zero shuffle mask "
                          << *shInst << "\n");
        return nullptr;
      }
    }

    if (InsertElementInst *insertInst =
            dyn_cast<InsertElementInst>(shInst->getOperand(0))) {
      val = insertInst;
    } else {
      LLVM_DEBUG(
          dbgs() << "shuffle mask zero but operand one not insert element "
                 << *(shInst->getOperand(0)) << "\n");
      return nullptr;
    }
  }
  if (InsertElementInst *insertInst = dyn_cast<InsertElementInst>(val)) {
    if (Constant *c = dyn_cast<Constant>(insertInst->getOperand(2))) {
      if (!c->isZeroValue()) {
        LLVM_DEBUG(dbgs() << "insert element has non-zero index " << *insertInst
                          << "\n");
        return nullptr;
      }
    } else {
      LLVM_DEBUG(dbgs() << "insert element has non-constant index "
                        << *insertInst
                        << " index: " << *(insertInst->getOperand(2)) << "\n");
      return nullptr;
    }
    // errs() << "insert " << *(insertInst->getOperand(1)) << "\n";
    Value *ieElement = insertInst->getOperand(1);
    if (CallInst *ci = dyn_cast<CallInst>(ieElement)) {
      std::string name = ci->getCalledFunction()->getName().str();
      if (name.find("llvm.vscale") != std::string::npos) {
        ieElement = Builder.CreateZExtOrTrunc(opCount, ci->getType(), "");
      }
    }
    // Match ieElement type to buffer element type (e.g. i64 → i32 when the
    // vector is <vscale x 1 x i32> but the scalar operand is i64)
    if (ieElement->getType() != type)
      ieElement = Builder.CreateTruncOrBitCast(ieElement, type);
    Value *elementPtr;
    if (gc && checkSecretShared->isSecretShared(ieElement, BB->getParent())) {
      elementPtr = createMalloc(Builder, BB->getParent(), ieElement->getType(),
                                Builder.getInt64(1), true);
    } else {
      elementPtr =
          Builder.CreateAlloca(ieElement->getType(), Builder.getInt32(1), "");
    }
    Builder.CreateStore(ieElement, elementPtr);
    Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
    // In gc mode, shared=true means "element is already a garbled Integer*".
    // Using !usedAsShared with shared=true causes MPC::store to interpret a
    // plain stack-allocated scalar as emp::Integer*, reading garbage vector
    // metadata and triggering bad_alloc. Only set shared=true when the element
    // is genuinely secret-shared (garbled).
    bool isElementSecret = checkSecretShared->isSecretShared(ieElement, BB->getParent());
    bool sharedFlag = gc ? isElementSecret : (isElementSecret || !usedAsShared);
    Builder.CreateCall(storeFunc,
                       {newPtr, elementPtr, n, Builder.getInt32(elementSize),
                        Builder.getInt1(sharedFlag),
                        Builder.getInt1(false)});
    return newPtr;
  }
  // Value is  @llvm.experimental.stepvector.nxv1i64() => sequential numbers 0
  // to n-1
  if (CallInst *ci = dyn_cast<CallInst>(val)) {
    std::string name = ci->getCalledFunction()->getName().str();
    if (name.find("llvm.experimental.stepvector") != std::string::npos) {
      Value *elementPtr = Builder.CreateAlloca(type, Builder.getInt32(1), "");
      Builder.CreateStore(ConstantInt::get(type, 0), elementPtr);
      Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
      if (gc) {
        Builder.CreateCall(
            storeFunc, {newPtr, elementPtr, n, Builder.getInt32(elementSize),
                        Builder.getInt1(false), Builder.getInt1(true)});
      } else
        Builder.CreateCall(
            storeFunc, {newPtr, elementPtr, n, Builder.getInt32(elementSize),
                        Builder.getInt1(!usedAsShared), Builder.getInt1(true)});
      return newPtr;
    }
  }

  LLVM_DEBUG(dbgs() << " could not make secret shared vec for " << *val
                    << " returning nullptr\n");
  return nullptr;
}

Instruction *VectorMPCLinkPass::isLoadStoreAtInduction(Instruction *Ld,
                                                       Instruction *induction,
                                                       Loop *L,
                                                       Value *opCount) {
  Value *op0 = nullptr;
  if (Ld->getOpcode() == Instruction::Load)
    op0 = Ld->getOperand(0);
  else if (Ld->getOpcode() == Instruction::Store)
    op0 = Ld->getOperand(1);
  else
    return nullptr;

  if (GetElementPtrInst *ptr = dyn_cast<GetElementPtrInst>(op0)) {
    Value *idx = ptr->getOperand(1);
    if (idx == induction)
      return ptr;
    if (isa<ConstantInt>(idx))
      return ptr;
    if (Instruction *idxInst = dyn_cast<Instruction>(idx)) {
      if (isValidIndex(idxInst, induction, L, opCount))
        return ptr;
    } else {
      LLVM_DEBUG(dbgs() << "299 wide load or store not at induction\t" << *idx
                        << "\n");
    }
  } else {
    // op0->print(errs());
    LLVM_DEBUG(dbgs() << " load not at gep " << *Ld << " " << *op0 << "\n");
  }
  return nullptr;
}

Instruction *VectorMPCLinkPass::getOperandPtr(
    Value *op, Instruction *induction, Value *opCount, IRBuilder<> &Builder,
    BasicBlock *BB, MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  Instruction *opInst = dyn_cast<Instruction>(op);
  bool op0Shared = checkSecretShared->isSecretShared(op, BB->getParent());
  bool usedAsShared = false;
  for (auto *user : op->users()) {
    if (checkSecretShared->isSecretShared(user, BB->getParent())) {
      usedAsShared = true;
    } else if (usedAsShared) {
      LLVM_DEBUG(
          dbgs()
          << "will result in incorrectness due to used as both shared and "
             "not shared\n");
    }
  }
  if (opInst != nullptr) {
    Instruction *ret = nullptr;
    if (ItoPtr.find(opInst) != ItoPtr.end())
      ret = ItoPtr[opInst];
    if (!ret) {
      opInst = isLoadStoreAtInduction(opInst, induction, L, opCount);
      if (opInst)
        ret = opInst;
    }
    if (ret) {
      if (!gc && usedAsShared && !op0Shared) {
        // Skip makeShared in gc mode: gc protocol handles public-vs-secret
        // comparisons natively. The gc-opt makeShared uses n*elementSize bytes
        // instead of n*sizeof(emp::Integer), corrupting emp::Integer vector
        // internals (garbled labels) for party 2.
        LLVM_DEBUG(dbgs() << "used as shared but not shared itself " << *op
                          << " " << *opInst << " \n");
        bool ptrMadeShared =
            checkSecretShared->isSecretShared(ret, BB->getParent());
        if (!ptrMadeShared) {
          setSecretShared(ret);
          llvm::Function *makeSharedFunc = cast<llvm::Function>(
              BB->getParent()
                  ->getParent()
                  ->getOrInsertFunction(
                      "_ZN3MPC10makeSharedEPvii",
                      FunctionType::get(Type::getVoidTy(BB->getContext()),
                                        {Builder.getPtrTy(),
                                         Builder.getInt32Ty(),
                                         Builder.getInt32Ty()},
                                        false))
                  .getCallee());
          int elementSize =
              BB->getParent()->getParent()->getDataLayout().getTypeAllocSize(
                  (dyn_cast<ScalableVectorType>(op->getType()))
                      ->getElementType());
          auto n = Builder.CreateZExtOrTrunc(opCount, Builder.getInt32Ty());
          Builder.CreateCall(makeSharedFunc,
                             {ret, n, Builder.getInt32(elementSize)});
        }
      }
      if (argToMPCtype.find(ret) != argToMPCtype.end())
        return dyn_cast<Instruction>(argToMPCtype[ret]);
      // If ret is a GEP, check if its base pointer was converted.
      // Redirect the GEP to the new buffer so the old malloc becomes dead
      // code and DCE removes it along with misplaced free calls.
      if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ret)) {
        Value *base = gep->getPointerOperand();
        if (argToMPCtype.find(base) != argToMPCtype.end()) {
          gep->setOperand(0, argToMPCtype[base]);
          return ret;
        }
      }
      return ret;
    }
  }
  opInst = makeSecretSharedVec(op, opCount, Builder, BB, usedAsShared);
  if (!opInst) {
    LLVM_DEBUG(dbgs() << "vector not found for " << *op << "\n");
  }
  return opInst;
}

Instruction *
VectorMPCLinkPass::getResPtr(Instruction *I, Instruction *induction,
                             Value *opCount, IRBuilder<> &Builder, Function *F,
                             SmallVector<Instruction *> &deleteInstructions,
                             Loop *L) {

  Instruction *resPtr = nullptr;

  Type *type = I->getType();
  if (auto scalableTy = dyn_cast<ScalableVectorType>(type))
    type = scalableTy->getElementType();
  if (type == Builder.getInt1Ty()) {
    for (auto *U : I->users()) {
      if (isa<ZExtInst>(U)) {
        I = dyn_cast<Instruction>(U);
        deleteInstructions.push_back(I);
      }
    }
  }
  for (auto *U : I->users())
    if (auto *Store = dyn_cast<StoreInst>(U)) {
      if (resPtr) {
        LLVM_DEBUG(dbgs() << "res ptr is already set\n");
      } else {
        resPtr = isLoadStoreAtInduction(Store, induction, L, opCount);
        deleteInstructions.push_back(Store);
      }
    }

  if (resPtr == nullptr) {
    Instruction *v = createMalloc(
        Builder, F, type, opCount,
        checkSecretShared->isSecretShared(I, I->getParent()->getParent()));
    freeLater.insert(v);
    resPtr = v;
  }
  if (argToMPCtype.find(resPtr) != argToMPCtype.end())
    return dyn_cast<Instruction>(argToMPCtype[resPtr]);
  return resPtr;
}

void VectorMPCLinkPass::updateIcmp(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  ICmpInst *icmp = dyn_cast<ICmpInst>(&I);
  if (!icmp)
    return;
  std::string name = getTypeName(icmp->getOperand(0)->getType(), Builder);
  if (name == "" || name == "1") {
    LLVM_DEBUG(dbgs() << I << " icmp type not implemented \n");
    return;
  }

  uint op = 0;
  bool swp = false;
  if (icmp->getPredicate() == ICmpInst::ICMP_EQ)
    op = 1;
  else if (icmp->getPredicate() == ICmpInst::ICMP_NE)
    op = 4;
  else if (icmp->isGT(icmp->getPredicate()))
    op = 2;
  else if (icmp->isGE(icmp->getPredicate()))
    op = 3;
  else if (icmp->isLT(icmp->getPredicate())) {
    op = 2;
    swp = true;
  } else if (icmp->isLE(icmp->getPredicate())) {
    op = 3;
    swp = true;
  } else {
    LLVM_DEBUG(dbgs() << *icmp << " op not determined\n");
    return;
  }
  Builder.SetInsertPoint(&I);

  // check if pattern leads to truncation
  if (op == 4 || op == 1) {
    Value *trunc = nullptr;
    Value *one = nullptr;
    if (Instruction *in1Inst = dyn_cast<Instruction>(I.getOperand(0)))
      if (Constant *c = dyn_cast<Constant>(I.getOperand(1)))
        if (c->isZeroValue() && in1Inst->getOpcode() == Instruction::And) {
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(0))) {
            if (c2->isOneValue()) {
              trunc = in1Inst->getOperand(1);
              one = c2;
            }
          }
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(1))) {
            if (c2->isOneValue()) {
              trunc = in1Inst->getOperand(0);
              one = c2;
            }
          }
          if (in1Inst->getNumUses() == 1 && trunc != nullptr)
            deleteInstructions.push_back(in1Inst);
        }

    if (Instruction *in1Inst = dyn_cast<Instruction>(I.getOperand(1)))
      if (Constant *c = dyn_cast<Constant>(I.getOperand(0)))
        if (c->isZeroValue() && in1Inst->getOpcode() == Instruction::And) {
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(0))) {
            if (c2->isOneValue()) {
              trunc = in1Inst->getOperand(1);
              one = c2;
            }
          }
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(1))) {
            if (c2->isOneValue()) {
              trunc = in1Inst->getOperand(0);
              one = c2;
            }
          }
          if (in1Inst->getNumUses() == 1 && trunc != nullptr)
            deleteInstructions.push_back(in1Inst);
        }

    if (trunc) {
      Type *type =
          (dyn_cast<ScalableVectorType>(trunc->getType()))->getElementType();
      if (type != Builder.getInt8Ty())
        LLVM_DEBUG(dbgs() << "truncate not from i8 " << *trunc << " \n");
      Instruction *ptr =
          getOperandPtr(trunc, induction, opCount, Builder, BB, ItoPtr, L);
      if (op == 1) {
        // For non-gc: resolve ptr2 BEFORE getResPtr so that any mallocs
        // created by makeSecretSharedVec are placed before resPtr, preserving
        // the dominator invariant (resPtr must dominate xorFunc call).
        Instruction *ptr2 = nullptr;
        if (!gc)
          ptr2 = getOperandPtr(one, induction, opCount, Builder, BB, ItoPtr, L);
        // Reset builder to icmp position before allocating resPtr.
        Builder.SetInsertPoint(&I);
        // do not of the ptr
        Instruction *resPtr = getResPtr(&I, induction, opCount, Builder, F,
                                        deleteInstructions, L);
        Value *n =
            Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
        llvm::Function *xorFunc;
        if (gc) {
          xorFunc = cast<llvm::Function>(
              F->getParent()
                  ->getOrInsertFunction(
                      GCFuncNames[((name == "1" || name == "8") ? "not1"
                                                                : "not")],
                      FunctionType::get(Builder.getVoidTy(),
                                        {Builder.getPtrTy(), Builder.getPtrTy(),
                                         Builder.getInt32Ty()},
                                        false))
                  .getCallee());
        } else {
          xorFunc = cast<llvm::Function>(
              F->getParent()
                  ->getOrInsertFunction(
                      FuncNames["xor" + name],
                      FunctionType::get(Type::getVoidTy(F->getContext()),
                                        {Builder.getPtrTy(), Builder.getPtrTy(),
                                         Builder.getPtrTy(),
                                         Builder.getInt32Ty(),
                                         Builder.getInt1Ty()},
                                        false))
                  .getCallee());
        }
        auto insertPoint = resPtr->getInsertionPointAfterDef().value();
        if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
          insertPoint = I.getInsertionPointAfterDef().value();
        Builder.SetInsertPoint(insertPoint);
        if (gc)
          Builder.CreateCall(xorFunc, {ptr, resPtr, n});
        else
          Builder.CreateCall(xorFunc,
                             {ptr, ptr2, resPtr, n, Builder.getInt1(true)});
        ptr = resPtr;
      }
      ItoPtr.insert(std::make_pair(&I, ptr));
      deleteInstructions.push_back(&I);
      return;
    }
  }

  // For gc mode: if operands are Bit buffers (i8 used as boolean), use
  // xorBit/not1 instead of icmpEq8 (which requires Integer* operands)
  if (gc && name == "8") {
    Instruction *op0 = getOperandPtr(I.getOperand(0), induction, opCount,
                                     Builder, BB, ItoPtr, L);
    bool isBitBuffer = false;
    if (op0) {
      // Trace through GEPs and pointer phis to find the underlying allocation,
      // then check if it's a createBit call (directly or via argToMPCtype)
      Instruction *base = op0;
      int depth = 0;
      while (base && depth++ < 8) {
        if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(base)) {
          base = dyn_cast<Instruction>(gep->getPointerOperand());
          continue;
        }
        if (PHINode *phi = dyn_cast<PHINode>(base)) {
          // Use first non-self incoming value
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            Value *inc = phi->getIncomingValue(i);
            if (inc != phi) {
              base = dyn_cast<Instruction>(inc);
              break;
            }
          }
          continue;
        }
        // Check direct createBit call
        if (CallInst *ci = dyn_cast<CallInst>(base)) {
          if (ci->getCalledFunction()->getName() == GCFuncNames["createBit"]) {
            isBitBuffer = true;
            break;
          }
        }
        // Check if base maps to a createBit via argToMPCtype
        auto it = argToMPCtype.find(base);
        if (it != argToMPCtype.end()) {
          if (CallInst *mpcCI = dyn_cast<CallInst>(it->second)) {
            if (mpcCI->getCalledFunction()->getName() == GCFuncNames["createBit"])
              isBitBuffer = true;
          }
        }
        break;
      }
    }
    if (isBitBuffer) {
      Instruction *op1 = getOperandPtr(I.getOperand(1), induction, opCount,
                                       Builder, BB, ItoPtr, L);
      Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
      Instruction *resPtr = getResPtr(&I, induction, opCount, Builder, F,
                                      deleteInstructions, L);
      llvm::Function *xorFunc = cast<llvm::Function>(
          F->getParent()
              ->getOrInsertFunction(
                  GCFuncNames["xor1"],
                  FunctionType::get(Builder.getVoidTy(),
                                    {Builder.getPtrTy(), Builder.getPtrTy(),
                                     Builder.getPtrTy(), Builder.getInt32Ty()},
                                    false))
              .getCallee());
      auto insertPoint = resPtr->getInsertionPointAfterDef().value();
      if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
        insertPoint = I.getInsertionPointAfterDef().value();
      Builder.SetInsertPoint(insertPoint);
      Builder.CreateCall(xorFunc, {op0, op1, resPtr, n});
      if (op == 1) {
        // EQ: NOT(XOR(op0,op1))
        llvm::Function *notFunc = cast<llvm::Function>(
            F->getParent()
                ->getOrInsertFunction(
                    GCFuncNames["not1"],
                    FunctionType::get(Builder.getVoidTy(),
                                      {Builder.getPtrTy(), Builder.getPtrTy(),
                                       Builder.getInt32Ty()},
                                      false))
                .getCallee());
        Builder.CreateCall(notFunc, {resPtr, resPtr, n});
      }
      ItoPtr.insert(std::make_pair(&I, resPtr));
      deleteInstructions.push_back(icmp);
      return;
    }
  }

  name = "icmpEq" + name;

  Instruction *op0Inst = getOperandPtr(I.getOperand(0), induction, opCount,
                                       Builder, BB, ItoPtr, L);
  Instruction *op1Inst = getOperandPtr(I.getOperand(1), induction, opCount,
                                       Builder, BB, ItoPtr, L);
  if (swp) {
    auto tmp = op0Inst;
    op0Inst = op1Inst;
    op1Inst = tmp;
  }
  Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  if (gc)
    name = GCFuncNames[name];
  else
    name = FuncNames[name];
  llvm::Function *icmpEqFunc = cast<llvm::Function>(
      F->getParent()
          ->getOrInsertFunction(
              name, FunctionType::get(Builder.getVoidTy(),
                                      {Builder.getPtrTy(), Builder.getPtrTy(),
                                       Builder.getPtrTy(), Builder.getInt32Ty(),
                                       Builder.getInt32Ty()},
                                      false))
          .getCallee());
  ;
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);
  Builder.CreateCall(icmpEqFunc,
                     {op0Inst, op1Inst, resPtr, n,
                      ConstantInt::get(Type::getInt32Ty(F->getContext()), op)});
  ItoPtr.insert(std::make_pair(&I, resPtr));
  deleteInstructions.push_back(icmp);
}

void VectorMPCLinkPass::updateReduction(
    CallInst *CI, Value *ptr, Value *opCount, IRBuilder<> &Builder,

    SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  // errs() << "update reduction " << *CI << "\n";
  std::string name = CI->getCalledFunction()->getName().str();
  if (name.find("llvm.vector.reduce.") != std::string::npos) {
    Function *F = CI->getParent()->getParent();
    // reduction(arr, n, elSize, ret, op)
    llvm::Function *Func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                FuncNames["reduction"],
                FunctionType::get(Type::getVoidTy(F->getContext()),
                                  {Builder.getPtrTy(), Builder.getInt32Ty(),
                                   Builder.getInt32Ty(), Builder.getPtrTy(),
                                   Builder.getInt32Ty(), Builder.getInt1Ty()},
                                  false))
            .getCallee());
    // errs() << *Func << "\n";
    Type *resTy = CI->getType();
    int elementSize = F->getParent()->getDataLayout().getTypeAllocSize(resTy);
    int op;
    if (name.find("add") != std::string::npos)
      op = 1;
    else if (name.find("mul") != std::string::npos)
      op = 2;
    else if (name.find("smax") != std::string::npos)
      op = 3;
    else if (name.find("smin") != std::string::npos)
      op = 4;
    else if (name.find("reduce.or") != std::string::npos)
      op = 5;
    else {
      LLVM_DEBUG(dbgs() << "reduction not implemented for " << name << "\n");
      return;
    }

    deleteInstructions.push_back(CI);
    Builder.SetInsertPoint(CI);
    bool isShared =
        checkSecretShared->isSecretShared(CI, CI->getParent()->getParent());
    Instruction *newRes;
    if (gc) {
      newRes = createMalloc(Builder, F, resTy, Builder.getInt64(1), true);
    } else {
      newRes = Builder.CreateAlloca(resTy, Builder.getInt32(1), "");
    }
    if (isShared) {
      MDNode *mdnode =
          MDNode::get(Builder.getContext(),
                      MDString::get(Builder.getContext(), "secret_shared"));
      newRes->setMetadata("secret_shared", mdnode);
    }
    Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
    if (resTy == Builder.getInt1Ty())
      elementSize = 0;
    Builder.CreateCall(Func, {ptr, n, Builder.getInt32(elementSize), newRes,
                              Builder.getInt32(op), Builder.getInt1(isShared)});
    // errs() << *v << "\n";
    Value *newResLoaded = Builder.CreateLoad(resTy, newRes, "");

    if (isShared) {
      setSecretShared(newRes);
      setSecretShared(newResLoaded);
    }
    CI->replaceAllUsesWith(newResLoaded);
  }
}
// Lowers `xor v, splat(-1)`, i.e. a vector NOT. The loop vectorizer emits it
// for any-of reductions, e.g. `reduce.or(xor(v, splat(true)))`.
void VectorMPCLinkPass::updateNot(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  Value *op = I.getOperand(0);
  Constant *ones = dyn_cast<Constant>(I.getOperand(1));
  if (!ones || !ones->isAllOnesValue()) {
    op = I.getOperand(1);
    ones = dyn_cast<Constant>(I.getOperand(0));
  }
  if (!ones || !ones->isAllOnesValue()) {
    LLVM_DEBUG(dbgs() << I << " xor without an all-ones operand\n");
    return;
  }
  std::string name = getTypeName(I.getType(), Builder);
  if (name == "" || name == "f") {
    LLVM_DEBUG(dbgs() << I << " not type not implemented\n");
    return;
  }

  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  Builder.SetInsertPoint(&I);
  Instruction *ptr = getOperandPtr(op, induction, opCount, Builder, BB, ItoPtr, L);
  // For non-gc, resolve the all-ones vector before getResPtr so that its
  // malloc dominates the xor call.
  Instruction *onesPtr = nullptr;
  if (!gc)
    onesPtr = getOperandPtr(ones, induction, opCount, Builder, BB, ItoPtr, L);
  if (!ptr || (!gc && !onesPtr)) {
    LLVM_DEBUG(dbgs() << I << " operand vector not found\n");
    return;
  }
  Builder.SetInsertPoint(&I);
  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
  llvm::Function *func;
  if (gc) {
    func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                GCFuncNames[(name == "1" || name == "8") ? "not1" : "not"],
                FunctionType::get(Builder.getVoidTy(),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getInt32Ty()},
                                  false))
            .getCallee());
  } else {
    func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                FuncNames["xor" + name],
                FunctionType::get(Builder.getVoidTy(),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getPtrTy(), Builder.getInt32Ty(),
                                   Builder.getInt1Ty()},
                                  false))
            .getCallee());
  }
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);
  if (gc)
    Builder.CreateCall(func, {ptr, resPtr, n});
  else
    Builder.CreateCall(func, {ptr, onesPtr, resPtr, n, Builder.getInt1(true)});
  ItoPtr.insert(std::make_pair(&I, resPtr));
  deleteInstructions.push_back(&I);
}

/* Incomplete yet */
void VectorMPCLinkPass::updateSelect(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  // errs() << "update select " << I << " " << *(I.getOperand(0)) << "\n";
  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  auto n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
  int elementSize;
  if (ScalableVectorType *scType = dyn_cast<ScalableVectorType>(I.getType())) {
    elementSize = F->getParent()->getDataLayout().getTypeAllocSize(
        scType->getElementType());
    if (scType->getElementType() == Builder.getInt1Ty() ||
        scType->getElementType() == Builder.getInt8Ty())
      elementSize = 0;
  } else
    return;
  Builder.SetInsertPoint(&I);

  Instruction *condition = getOperandPtr(I.getOperand(0), induction, opCount,
                                         Builder, BB, ItoPtr, L);
  Instruction *op0Inst = getOperandPtr(I.getOperand(1), induction, opCount,
                                       Builder, BB, ItoPtr, L);
  Instruction *op1Inst = getOperandPtr(I.getOperand(2), induction, opCount,
                                       Builder, BB, ItoPtr, L);

  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  llvm::Function *Func;
  if (gc) {
    Func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                GCFuncNames[elementSize == 0 ? "select1" : "select"],
                FunctionType::get(Builder.getVoidTy(),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getInt32Ty()},
                                  false))
            .getCallee());
  } else {
    Func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                FuncNames["select"],
                FunctionType::get(Type::getVoidTy(F->getContext()),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getPtrTy(), Builder.getInt32Ty(),
                                   Builder.getInt32Ty(), Builder.getPtrTy(),
                                   Builder.getInt1Ty()},
                                  false))
            .getCallee());
  }

  ItoPtr.insert(std::make_pair(&I, resPtr));
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);
  if (gc)
    Builder.CreateCall(Func, {op0Inst, op1Inst, condition, resPtr, n});
  else
    // The select is secret when the instruction is, even if its result
    // buffer is not marked, e.g. the phi buffer of an any-of reduction.
    Builder.CreateCall(
        Func,
        {op0Inst, op1Inst, condition, n, Builder.getInt32(elementSize), resPtr,
         Builder.getInt1(checkSecretShared->isSecretShared(&I, F) ||
                         checkSecretShared->isSecretShared(resPtr, F))});
  deleteInstructions.push_back(&I);
}

void VectorMPCLinkPass::updateIntFuncs(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  std::string name = getTypeName(I.getType(), Builder);
  if (name == "" || name == "1") {
    LLVM_DEBUG(dbgs() << I << " type not implemented \n");
    return;
  }
  if (I.getOpcode() == Instruction::Mul) {
    name = "mul" + name;
  } else if (I.getOpcode() == Instruction::Sub) {
    name = "sub" + name;
  } else if (I.getOpcode() == Instruction::Add) {
    name = "add" + name;
  } else if (I.getOpcode() == Instruction::FAdd) {
    name = "fadd";
  } else if (I.getOpcode() == Instruction::FSub) {
    name = "fsub";
  } else if (I.getOpcode() == Instruction::FMul) {
    name = "fmul";
  } else if (I.getOpcode() == Instruction::FDiv) {
    name = "fdiv";
  } else {
    LLVM_DEBUG(dbgs() << "update int func called for " << I << "\n");
    return;
  }

  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  Builder.SetInsertPoint(&I);
  Instruction *op0Inst = getOperandPtr(I.getOperand(0), induction, opCount,
                                       Builder, BB, ItoPtr, L);
  Instruction *op1Inst = getOperandPtr(I.getOperand(1), induction, opCount,
                                       Builder, BB, ItoPtr, L);

  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  ItoPtr.insert(std::make_pair(&I, resPtr));
  if (gc)
    name = GCFuncNames[name];
  else
    name = FuncNames[name];
  llvm::Function *func = cast<llvm::Function>(
      F->getParent()
          ->getOrInsertFunction(
              name, FunctionType::get(Type::getVoidTy(F->getContext()),
                                      {Builder.getPtrTy(), Builder.getPtrTy(),
                                       Builder.getPtrTy(),
                                       Type::getInt32Ty(F->getContext())},
                                      false))
          .getCallee());
  Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);
  Builder.CreateCall(func, {op0Inst, op1Inst, resPtr, n});
  deleteInstructions.push_back(&I);
}

void VectorMPCLinkPass::updatePHI(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  // incoming values are vectors (not pointers, not stored as an array) use
  // stack space rn.
  /*
    check all incoming blocks
    self k alava jo bhi alag block h vaha array banao aur array me vo fill karo
    then phi node me baki sab blocks se respective arrays aur self block se phi
    itself
  */

  PHINode *phi = dyn_cast<PHINode>(&I);
  Type *type = phi->getType();
  if (ScalableVectorType *scType = dyn_cast<ScalableVectorType>(type)) {
    type = scType->getElementType();
  } else
    return;
  // we update the loop such that it only runs once, thus remove identity
  // instructions that are created by the first iteration of the loop

  bool isShared =
      checkSecretShared->isSecretShared(phi, phi->getParent()->getParent());
  deleteInstructions.push_back(phi);

  bool allUsersIdentity = true;
  for (size_t i = 0; i < phi->getNumIncomingValues(); ++i) {
    BasicBlock *bb = phi->getIncomingBlock(i);
    Value *val = phi->getIncomingValueForBlock(bb);
    if (bb != I.getParent()) {
      if (Constant *c = dyn_cast<Constant>(val)) {
        for (auto *user : phi->users()) {
          Instruction *userInst = dyn_cast<Instruction>(user);
          if (!userInst)
            continue;
          if (c->isOneValue()) {
            if (userInst->getOpcode() == Instruction::Mul)
              identityInstrs.insert({userInst, userInst->getOperand(0) == phi
                                                   ? userInst->getOperand(1)
                                                   : userInst->getOperand(0)});
            else if (userInst->getOpcode() == Instruction::And &&
                     type == Builder.getInt8Ty())
              identityInstrs.insert({userInst, userInst->getOperand(0) == phi
                                                   ? userInst->getOperand(1)
                                                   : userInst->getOperand(0)});
            else {
              LLVM_DEBUG(dbgs() << "constant phi " << *phi << " user "
                                << *userInst << "\n");
              allUsersIdentity = false;
            }
          } else if (c->isZeroValue()) {
            if (userInst->getOpcode() == Instruction::Add)
              identityInstrs.insert({userInst, userInst->getOperand(0) == phi
                                                   ? userInst->getOperand(1)
                                                   : userInst->getOperand(0)});
            else if (CallInst *ci = dyn_cast<CallInst>(userInst)) {
              std::string name = ci->getCalledFunction()->getName().str();
              if (name.find("llvm.smax") != std::string::npos) {
                identityInstrs.insert(
                    {userInst, userInst->getOperand(0) == phi
                                   ? userInst->getOperand(1)
                                   : userInst->getOperand(0)});
              } else {
                LLVM_DEBUG(dbgs() << "constant phi " << *phi << " user "
                                  << *userInst << "\n");
                allUsersIdentity = false;
              }
            } else {
              LLVM_DEBUG(dbgs() << "constant phi " << *phi << " user "
                                << *userInst << "\n");
              allUsersIdentity = false;
            }
          } else {
            allUsersIdentity = false;
            LLVM_DEBUG(dbgs() << "constant " << *c << "\n");
            break;
          }
        }
      } else {
        allUsersIdentity = false;
      }
      if (allUsersIdentity)
        break;
    }
  }

  if (allUsersIdentity) {
    return;
  }

  PHINode *newPhi =
      PHINode::Create(Builder.getPtrTy(), phi->getNumIncomingValues(), "");
  for (size_t i = 0; i < phi->getNumIncomingValues(); ++i) {
    BasicBlock *bb = phi->getIncomingBlock(i);
    Value *val = phi->getIncomingValueForBlock(bb);
    if (bb != I.getParent()) {
      Instruction *arr = makeSecretSharedVec(
          val, opCount, Builder, bb,
          checkSecretShared->isSecretShared(phi, I.getParent()->getParent()));
      if (arr == nullptr) {
        LLVM_DEBUG(dbgs() << "arr is nullptr \n");
        LLVM_DEBUG(dbgs() << *val << "\n");
      }
      newPhi->addIncoming(arr, bb);
    } else {
      newPhi->addIncoming(newPhi, bb);
    }
  }

  Value *valForBB = phi->getIncomingValueForBlock(I.getParent());
  newPhi->insertAfter(phi);
  Builder.SetInsertPoint(I.getParent()->getFirstNonPHI());
  Value *gep = Builder.CreateGEP(type, newPhi, induction);
  Value *loaded = Builder.CreateLoad(phi->getType(), gep);
  phi->replaceAllUsesWith(loaded);
  if (Instruction *I = dyn_cast<Instruction>(valForBB)) {
    Builder.SetInsertPoint(I->getInsertionPointAfterDef().value());
  }
  LoadInst *ValLoaded = Builder.CreateLoad(valForBB->getType(), gep);
  valForBB->replaceAllUsesWith(ValLoaded);
  if (isShared) {
    setSecretShared(loaded);
    setSecretShared(ValLoaded);
  }
  for (User *user : ValLoaded->users()) {
    if (Instruction *UserInst = dyn_cast<Instruction>(user)) {
      if (UserInst->getParent() == I.getParent())
        continue;
      else {
        if (CallInst *CI = dyn_cast<CallInst>(UserInst)) {
          std::string name = CI->getCalledFunction()->getName().str();
          if (name.find("llvm.vector.reduce.") != std::string::npos) {
            updateReduction(CI, gep, opCount, Builder, deleteInstructions,
                            ItoPtr, L);
          } else {
            updateInst(*UserInst, induction, opCount, Builder,
                       deleteInstructions, ItoPtr, L);
          }
        } else
          updateInst(*UserInst, induction, opCount, Builder, deleteInstructions,
                     ItoPtr, L);
      }
    }
  }
  if (Instruction *I = dyn_cast<Instruction>(valForBB)) {
    Builder.SetInsertPoint(I->getInsertionPointAfterDef().value());
  } else {
    Builder.SetInsertPoint(ValLoaded);
  }
  Builder.CreateStore(valForBB, gep);

  return;
}

void VectorMPCLinkPass::updateDiv(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, std::string name, Loop *L) {
  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  Builder.SetInsertPoint(&I);
  Instruction *op0Inst = getOperandPtr(I.getOperand(0), induction, opCount,
                                       Builder, BB, ItoPtr, L);
  Instruction *op1Inst = getOperandPtr(I.getOperand(1), induction, opCount,
                                       Builder, BB, ItoPtr, L);

  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  ItoPtr.insert(std::make_pair(&I, resPtr));

  llvm::Function *func = cast<llvm::Function>(
      F->getParent()
          ->getOrInsertFunction(
              FuncNames[name],
              FunctionType::get(Type::getVoidTy(F->getContext()),
                                {Builder.getPtrTy(), Builder.getPtrTy(),
                                 Builder.getInt32Ty(), Builder.getInt32Ty(),
                                 Builder.getPtrTy(), Builder.getInt1Ty()},
                                false))
          .getCallee());
  if (gc) {
    func = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction(
                GCFuncNames[name],
                FunctionType::get(Type::getVoidTy(F->getContext()),
                                  {Builder.getPtrTy(), Builder.getPtrTy(),
                                   Builder.getPtrTy(), Builder.getInt32Ty()},
                                  false))
            .getCallee());
  }
  Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);

  Type *type = I.getType();
  if (auto *vecTy = dyn_cast<ScalableVectorType>(type)) {
    type = vecTy->getElementType();
  }
  int elSize = F->getParent()->getDataLayout().getTypeAllocSize(type);
  if (gc)
    Builder.CreateCall(func, {op0Inst, op1Inst, resPtr, n});
  else
    Builder.CreateCall(
        func, {op0Inst, op1Inst, n, Builder.getInt32(elSize), resPtr,
               Builder.getInt1(checkSecretShared->isSecretShared(&I, F))});
  deleteInstructions.push_back(&I);
  // if (!isa<Instruction>(I.getOperand(0)))
  //   MPCVecUtils::CreateFree(Builder, F, op0Inst);
  // if (!isa<Instruction>(I.getOperand(1)))
  //   MPCVecUtils::CreateFree(Builder, F, op1Inst);
}

void VectorMPCLinkPass::updateType(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  Builder.SetInsertPoint(&I);
  Instruction *op0Inst = getOperandPtr(I.getOperand(0), induction, opCount,
                                       Builder, BB, ItoPtr, L);

  int s1, s2;
  if (auto *vecTy = dyn_cast<ScalableVectorType>(I.getType())) {
    auto type = vecTy->getElementType();
    s1 = F->getParent()->getDataLayout().getTypeAllocSize(type);
  } else
    return;
  if (auto *vecTy = dyn_cast<ScalableVectorType>(I.getOperand(0)->getType())) {
    auto type = vecTy->getElementType();
    s2 = F->getParent()->getDataLayout().getTypeAllocSize(type);
  } else
    return;

  if (s1 == s2) {
    ItoPtr.insert(std::make_pair(&I, op0Inst));
    deleteInstructions.push_back(&I);
    return;
  }

  Instruction *resPtr =
      getResPtr(&I, induction, opCount, Builder, F, deleteInstructions, L);
  ItoPtr.insert(std::make_pair(&I, resPtr));

  llvm::Function *func = cast<llvm::Function>(
      F->getParent()
          ->getOrInsertFunction(
              FuncNames["type"],
              FunctionType::get(Type::getVoidTy(F->getContext()),
                                {Builder.getPtrTy(), Builder.getPtrTy(),
                                 Builder.getInt32Ty(), Builder.getInt32Ty(),
                                 Builder.getInt32Ty()},
                                false))
          .getCallee());

  Value *n = Builder.CreateTrunc(opCount, Type::getInt32Ty(F->getContext()));
  auto insertPoint = resPtr->getInsertionPointAfterDef().value();
  if (resPtr->getParent() == BB && resPtr->comesBefore(&I))
    insertPoint = I.getInsertionPointAfterDef().value();
  Builder.SetInsertPoint(insertPoint);
  Builder.CreateCall(
      func, {resPtr, op0Inst, n, Builder.getInt32(s1), Builder.getInt32(s2)});
  deleteInstructions.push_back(&I);
}

// Rebuilds an index expression with every llvm.vscale call replaced by the
// loop's element count, since the lowered loop runs once over all elements.
static Value *replaceVScale(Value *v, Value *opCount, IRBuilder<> &Builder) {
  if (auto *CI = dyn_cast<CallInst>(v))
    if (CI->getCalledFunction() &&
        CI->getCalledFunction()->getIntrinsicID() == Intrinsic::vscale)
      return Builder.CreateZExtOrTrunc(opCount, v->getType());
  if (auto *BO = dyn_cast<BinaryOperator>(v)) {
    Value *a = replaceVScale(BO->getOperand(0), opCount, Builder);
    Value *b = replaceVScale(BO->getOperand(1), opCount, Builder);
    if (a == BO->getOperand(0) && b == BO->getOperand(1))
      return v;
    return Builder.CreateBinOp(BO->getOpcode(), a, b);
  }
  if (auto *Cast = dyn_cast<CastInst>(v)) {
    Value *a = replaceVScale(Cast->getOperand(0), opCount, Builder);
    if (a == Cast->getOperand(0))
      return v;
    return Builder.CreateCast(Cast->getOpcode(), a, Cast->getDestTy());
  }
  return v;
}

// Lowers `extractelement v, idx` on a scalable vector to a scalar load of
// element idx from v's MPC buffer. The loop vectorizer emits it to take the
// last lane of a select, e.g. the index of the last match in a loop.
void VectorMPCLinkPass::updateExtract(
    ExtractElementInst &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  if (!I.getVectorOperandType()->isScalableTy())
    return;
  BasicBlock *BB = I.getParent();
  Function *F = BB->getParent();
  Builder.SetInsertPoint(&I);
  Instruction *vecPtr = getOperandPtr(I.getVectorOperand(), induction, opCount,
                                      Builder, BB, ItoPtr, L);
  if (!vecPtr) {
    LLVM_DEBUG(dbgs() << I << " vector operand not found\n");
    return;
  }
  Builder.SetInsertPoint(&I);
  Value *idx = Builder.CreateZExtOrTrunc(
      replaceVScale(I.getIndexOperand(), opCount, Builder),
      Builder.getInt64Ty());
  Type *elTy = I.getType();
  Value *elPtr;
  if (gc) {
    bool isBit = elTy == Builder.getInt1Ty() || elTy == Builder.getInt8Ty();
    std::string gepName = isBit ? "_ZN3MPC3gepEPN3emp3BitE" MPC_I64
                                : "_ZN3MPC3gepEPN3emp7IntegerE" MPC_I64;
    Function *gepFunc = cast<Function>(
        F->getParent()
            ->getOrInsertFunction(
                gepName, FunctionType::get(Builder.getPtrTy(),
                                           {Builder.getPtrTy(),
                                            Builder.getInt64Ty()},
                                           false))
            .getCallee());
    elPtr = Builder.CreateCall(gepFunc, {vecPtr, idx});
  } else {
    elPtr = Builder.CreateGEP(elTy, vecPtr, idx);
  }
  LoadInst *loaded = Builder.CreateLoad(elTy, elPtr);
  if (checkSecretShared->isSecretShared(&I, F)) {
    setSecretShared(loaded);
    if (Instruction *elPtrInst = dyn_cast<Instruction>(elPtr))
      setSecretShared(elPtrInst);
  }
  I.replaceAllUsesWith(loaded);
  deleteInstructions.push_back(&I);
}

void VectorMPCLinkPass::updateInst(
    Instruction &I, Instruction *induction, Value *opCount,
    IRBuilder<> &Builder, SmallVector<Instruction *> &deleteInstructions,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {
  BasicBlock *BB = I.getParent();
  Module *module = BB->getParent()->getParent();
  if (auto *EE = dyn_cast<ExtractElementInst>(&I)) {
    updateExtract(*EE, induction, opCount, Builder, deleteInstructions, ItoPtr,
                  L);
    return;
  }
  if (!I.getType()->isScalableTy() ||
      !I.getOperand(0)->getType()->isScalableTy())
    return;
  if (std::find(deleteInstructions.begin(), deleteInstructions.end(), &I) !=
      deleteInstructions.end())
    return;
  if (identityInstrs.find(&I) != identityInstrs.end()) {
    Value *op = identityInstrs.at(&I);
    auto ptr = getOperandPtr(op, induction, opCount, Builder, BB, ItoPtr, L);
    ItoPtr.insert({&I, ptr});
    deleteInstructions.push_back(&I);
    return;
  }
  switch (I.getOpcode()) {
  case Instruction::Load: {
    if (!I.getType()->isScalableTy())
      break;
    LLVM_FALLTHROUGH;
  }
  case Instruction::Store: {
    Instruction *loadStoreAtInd =
        isLoadStoreAtInduction(&I, induction, L, opCount);
    if (loadStoreAtInd == nullptr) {
      // TODO: set MPC Load
      LLVM_DEBUG(dbgs() << "scalable load not at induction\t" << I << "\n");
    } else
      deleteInstructions.push_back(&I);
    break;
  }
  case Instruction::ICmp: {
    if (!I.getType()->isScalableTy())
      break;
    updateIcmp(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    break;
  }
  case Instruction::Call:
    if (CallInst *callInst = dyn_cast<CallInst>(&I)) {
      std::string name = callInst->getCalledFunction()->getName().str();
      if ((name.find("llvm.smax") != std::string::npos)) {
        updateDiv(I, induction, opCount, Builder, deleteInstructions, ItoPtr,
                  "smax", L);
      } else if ((name.find("llvm.smin") != std::string::npos)) {
        updateDiv(I, induction, opCount, Builder, deleteInstructions, ItoPtr,
                  "smin", L);
      } else if (name.find("llvm.experimental.vector.reverse") !=
                 std::string::npos) {
        llvm::Function *reverseFunc = cast<llvm::Function>(
            module
                ->getOrInsertFunction(
                    "_ZN3MPC7reverseEPvS0_ii",
                    FunctionType::get(Type::getVoidTy(module->getContext()),
                                      {Builder.getPtrTy(), Builder.getPtrTy(),
                                       Builder.getInt32Ty(),
                                       Builder.getInt32Ty()},
                                      false))
                .getCallee());
        Builder.SetInsertPoint(&I);
        Value *n = Builder.CreateTruncOrBitCast(opCount, Builder.getInt32Ty());
        auto op0 = getOperandPtr(callInst->getArgOperand(0), induction, opCount,
                                 Builder, BB, ItoPtr, L);
        Instruction *resPtr = getResPtr(&I, induction, opCount, Builder,
                                        BB->getParent(), deleteInstructions, L);
        Type *type =
            dyn_cast<ScalableVectorType>(I.getType())->getElementType();
        uint64_t elementSize = module->getDataLayout().getTypeAllocSize(type);
        Builder.CreateCall(reverseFunc,
                           {op0, resPtr, n, Builder.getInt32(elementSize)});
        deleteInstructions.push_back(&I);
        ItoPtr.insert({&I, resPtr});
      }

      else if (name.find("llvm") != std::string::npos) {
        LLVM_DEBUG(dbgs() << I << "\n");
      }
    }
    break;
  case Instruction::UDiv:
  case Instruction::SDiv:
    updateDiv(I, induction, opCount, Builder, deleteInstructions, ItoPtr, "div",
              L);
    break;
  // case Instruction::And:
  case Instruction::Xor:
    updateNot(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    break;
  case Instruction::FMul:
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FDiv:
  case Instruction::Sub:
  case Instruction::Add:
  case Instruction::Mul:
    if (!I.getType()->isScalableTy())
      break;
    updateIntFuncs(I, induction, opCount, Builder, deleteInstructions, ItoPtr,
                   L);
    break;
  case Instruction::Select:
    updateSelect(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    break;
  case Instruction::PHI: {
    if (I.getType()->isScalableTy()) {
      updatePHI(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    }
    break;
  }
  case Instruction::Freeze: {
    I.replaceAllUsesWith(I.getOperand(0));
    deleteInstructions.push_back(&I);
    break;
  }
  case Instruction::ZExt:
  case Instruction::Trunc:
  case Instruction::BitCast: {
    updateType(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    break;
  }
  case Instruction::And: {
    auto type = (dyn_cast<ScalableVectorType>(I.getType()))->getElementType();
    if (type == Builder.getInt8Ty()) {
      Constant *c = dyn_cast<Constant>(I.getOperand(0));
      Value *op = I.getOperand(1);
      if (isa<Constant>(I.getOperand(1))) {
        op = I.getOperand(0);
        c = dyn_cast<Constant>(I.getOperand(1));
      }
      if (c && c->isOneValue()) {
        Instruction *ptr =
            getOperandPtr(op, induction, opCount, Builder, BB, ItoPtr, L);
        ItoPtr.insert({&I, ptr});
        deleteInstructions.push_back(&I);
        break;
      }
    }
    LLVM_FALLTHROUGH;
  }
  default:
    if (ScalableVectorType *scType = dyn_cast<ScalableVectorType>(I.getType()))
      LLVM_DEBUG(dbgs() << I << "\n");
    break;
  }
}

void VectorMPCLinkPass::deleteInsts(
    SmallVector<Instruction *> &deleteInstructions, Instruction *induction,
    Value *opCount, IRBuilder<> &Builder,
    MapVector<Value *, Instruction *> &ItoPtr, Loop *L) {

  auto itr = deleteInstructions.begin();
  size_t lastSize = deleteInstructions.size() + 1; // sentinel to detect no-progress
  while (deleteInstructions.size() > 0) {
    if (itr == deleteInstructions.end()) {
      // Completed a full pass. If nothing was erased, force-break use cycles.
      if (deleteInstructions.size() >= lastSize) {
        for (Instruction *I : deleteInstructions) {
          if (I && I->getParent() && I->getNumUses() > 0)
            I->replaceAllUsesWith(UndefValue::get(I->getType()));
        }
        for (Instruction *I : deleteInstructions) {
          if (I && I->getParent())
            I->eraseFromParent();
        }
        deleteInstructions.clear();
        return;
      }
      lastSize = deleteInstructions.size();
      itr = deleteInstructions.begin();
    } else if (*itr == nullptr)
      itr = deleteInstructions.erase(itr);
    else {
      Instruction *I = *itr;
      if (I->getParent() == nullptr) {
        itr = deleteInstructions.erase(itr);
        continue;
      }
      if (I->getNumUses() > 0) {
        SmallVector<Instruction *> deleteUsers;
        for (auto *user : I->users()) {
          if (Instruction *userInst = dyn_cast<Instruction>(user)) {
            if (std::find(deleteInstructions.begin(), deleteInstructions.end(),
                          userInst) == deleteInstructions.end()) {
              if (CallInst *CI = dyn_cast<CallInst>(userInst)) {
                std::string name = CI->getCalledFunction()->getName().str();
                if (name.find("llvm.vector.reduce.") != std::string::npos) {
                  updateReduction(CI, ItoPtr[I], opCount, Builder, deleteUsers,
                                  ItoPtr, L);
                } else {
                  updateInst(*userInst, induction, opCount, Builder,
                             deleteUsers, ItoPtr, L);
                }
              } else {
                updateInst(*userInst, induction, opCount, Builder, deleteUsers,
                           ItoPtr, L);
              }
            }
          }
        }
        deleteInsts(deleteUsers, induction, opCount, Builder, ItoPtr, L);
        if (I->getNumUses() > 0) {
          if (PHINode *phi = dyn_cast<PHINode>(I)) {
            auto incomingVal = phi->getIncomingValueForBlock(I->getParent());
            auto Iuser = I->getUniqueUndroppableUser();
            auto incomingUser = incomingVal->getUniqueUndroppableUser();
            if (Iuser == incomingVal && incomingUser == I) {
              incomingVal->replaceAllUsesWith(UndefValue::get(phi->getType()));
              if (std::find(deleteInstructions.begin(),
                            deleteInstructions.end(),
                            incomingVal) == deleteInstructions.end()) {
                auto tmp = dyn_cast<Instruction>(incomingVal);
                tmp->eraseFromParent();
              }
            }
          }
          itr++;
          continue;
        }
      }
      I->eraseFromParent();
      itr = deleteInstructions.erase(itr);
    }
  }
}

// void destroyMPC(IRBuilder<> &Builder, Function *F, Value *ptr){
//   if(isa<CallInst>())
// }

void VectorMPCLinkPass::replaceLoop(Loop *L, PHINode *induction,
                                    Value *opCount) {

  Function *F = L->getBlocksVector()[0]->getParent();
  IRBuilder<> Builder(F->getContext());
  SmallVector<Instruction *> deleteInstructions;
  Instruction *latchIcmp = L->getLatchCmpInst();
  Instruction *indNext = dyn_cast<Instruction>(latchIcmp->getOperand(0));

  Value *vscale = nullptr;
  if (indNext->getOperand(0) == induction) {
    vscale = indNext->getOperand(1);
  } else {
    vscale = indNext->getOperand(0);
  }
  freeLater.clear();
  MapVector<Value *, Instruction *> ItoPtr;
  for (BasicBlock *BB : L->blocks()) {
    for (auto &I : *BB) {
      updateInst(I, induction, opCount, Builder, deleteInstructions, ItoPtr, L);
    }
  }
  if (vscale != opCount) {
    // vscale->replaceUsesWithIf(opCount, [L, vscale](Use &U) {
    //   Instruction *I = dyn_cast<Instruction>(U);
    //   errs() << formatv("vscale: {0}, user: {1}\n", *vscale, *I);
    //   L->contains(I->getParent());
    //   return true;
    // });
    vscale->replaceAllUsesWith(opCount);
  }
  // if(vscale != opCount)
  //   vscale->replaceAllUsesWith(opCount);
  deleteInsts(deleteInstructions, induction, opCount, Builder, ItoPtr, L);
  for (auto ptr : freeLater) {
    bool usedOutside = false;
    Instruction *freePtr = nullptr;
    for (auto *user : ptr->users()) {
      if (Instruction *userInst = dyn_cast<Instruction>(user)) {
        if (userInst->getParent() != ptr->getParent()) {
          usedOutside = true;
          break;
        }
        if (freePtr == nullptr)
          freePtr = userInst;
        else if (freePtr->comesBefore(userInst)) {
          freePtr = userInst;
        }
      }
    }
    if (!usedOutside && freePtr) {
      // errs() << *ptr << " " << *freePtr << "\n";
      Builder.SetInsertPoint(freePtr->getNextNonDebugInstruction());
      // destroyMPC(Builder, F, ptr);
    }
  }
  freeLater.clear();
}

llvm::PreservedAnalyses VectorMPCLinkPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  gc = UseGCMode;
  std::string filePath = formatv("{0}", MetadataFilePath, M.getName());

  auto &FAMProxy = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M);
  auto &FAM = FAMProxy.getManager();
  checkSecretShared = new CheckSecretShared(filePath, M);

  for (llvm::Function &F : M) {
    if (!F.isDeclaration()) {
      auto tmp = checkSecretShared->args.find(&F);
      if (tmp == checkSecretShared->args.end())
        continue;
      SmallVector<Argument *> *Args = tmp->second;
      if (Args->size() == 0) {
        bool hasSecretShared = false;
        for (auto &BB : F) {
          if (hasSecretShared)
            break;
          for (auto &I : BB) {
            if (I.hasMetadata("secret_shared")) {
              hasSecretShared = true;
              break;
            }
          }
        }
        if (!hasSecretShared)
          continue;
      }
      auto &LI = FAM.getResult<LoopAnalysis>(F);
      auto &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
      SmallVector<BasicBlock *> deleteBBs;
      std::map<Loop *, std::pair<PHINode *, Value *>> opcountMap;
      for (auto *TopMostLoop : LI) {
        for (auto *L : depth_first(TopMostLoop)) {
          if (!L->isInnermost())
            continue;
          bool isVec = isVectorized(L);
          if (isVec) {
            PHINode *induction = L->getInductionVariable(SE);
            Value *opCount = getScalarLoop(L, LI, deleteBBs, induction);
            if (!opCount) {
              LLVM_DEBUG(dbgs() << "scalar loop not found\n");
              continue;
            }
            opcountMap.insert({L, {induction, opCount}});
          }
        }
      }
      auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
      IRBuilder<> Builder(M.getContext());
      if (gc) {
        if (checkSecretShared->readArgAccess.find(&F) !=
            checkSecretShared->readArgAccess.end()) {
          if (argToMPCtype.size() !=
              checkSecretShared->readArgAccess[&F].size()) {
            for (auto p : checkSecretShared->readArgAccess[&F]) {
              Value *arg = p.first;
              Value *n = p.second;
              // An argument the function never uses may be passed as poison
              // by its callers, so it must not be read.
              if (arg->use_empty())
                continue;
              // Ensure the arg has a unique name so the "<name>.mpc" instruction
              // can be matched by MPCLink's name-based lookup.  Unnamed args
              // (getName() == "") all get the same ".mpc" suffix, causing
              // collisions; use the argument index instead.
              if (arg->getName().empty())
                cast<Argument>(arg)->setName(
                    std::to_string(cast<Argument>(arg)->getArgNo()));
              llvm::Function *createInputMPCTypes = cast<llvm::Function>(
                  F.getParent()
                      ->getOrInsertFunction(
                          MPCTypeFuncNames[checkSecretShared
                                               ->sizesMap[&F][arg]],
                          FunctionType::get(
                              Builder.getPtrTy(),
                              {Builder.getPtrTy(), Builder.getInt32Ty()},
                              false))
                      .getCallee());
              // TODO: relying on assumption that this is not main function
              Builder.SetInsertPoint(F.getEntryBlock().getFirstNonPHI());
              Value *output = Builder.CreateCall(createInputMPCTypes, {arg, n},
                                                 arg->getName() + ".mpc");
              Instruction *tmp = dyn_cast<Instruction>(output);
              argToMPCtype.insert(std::make_pair(arg, output));
            }
          }
        }

        // Populate writeArgtoMPCtype for write args.
        // If the arg is also a read arg, reuse the MPC ptr already created by
        // the read-arg block above (named arg.mpc so MPCLink can find it).
        // For write-only args, create a new MPC buffer.
        if (checkSecretShared->writeArgAccess.find(&F) !=
            checkSecretShared->writeArgAccess.end()) {
          for (auto p : checkSecretShared->writeArgAccess[&F]) {
            Value *arg = p.first;
            if (arg->use_empty() ||
                writeArgtoMPCtype.find(arg) != writeArgtoMPCtype.end())
              continue;
            auto it = argToMPCtype.find(arg);
            if (it != argToMPCtype.end()) {
              writeArgtoMPCtype.insert(std::make_pair(arg, it->second));
            } else {
              int elemBits = checkSecretShared->sizesMap[&F][arg];
              Type *elemType = Builder.getIntNTy(elemBits);
              Builder.SetInsertPoint(F.getEntryBlock().getFirstNonPHI());
              Instruction *mpcPtr = createMalloc(Builder, &F, elemType, p.second, true);
              mpcPtr->setName(arg->getName() + ".mpc");
              writeArgtoMPCtype.insert(std::make_pair(arg, mpcPtr));
            }
          }
        }

      } // end if (gc) — readArgAccess/writeArgAccess

      // Convert secret-shared mallocs to MPC-compatible buffers.
      // Runs in both gc and non-gc modes. In gc mode, creates createIntEl/
      // createBitEl. In non-gc mode, creates a new malloc with correct size.
      // In both cases, the original malloc becomes dead code, and DCE removes
      // it along with any misplaced free calls (e.g. from flatten pass).
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.hasMetadata("secret_shared"))
            continue;
          if (CallInst *ci = dyn_cast<CallInst>(&I)) {
            if (ci->getCalledFunction()->getName().str() == "malloc") {
              Value *n = ci->getOperand(0);
              Type *type = nullptr;
              // Check for type metadata set by the flatten pass
              if (ci->hasMetadata("int1"))
                type = Builder.getInt1Ty();
              else if (ci->hasMetadata("int8"))
                type = Builder.getInt8Ty();
              else if (ci->hasMetadata("int16"))
                type = Builder.getInt16Ty();
              else if (ci->hasMetadata("int32"))
                type = Builder.getInt32Ty();
              else if (ci->hasMetadata("int64"))
                type = Builder.getInt64Ty();
              // Fall back to user traversal if no type metadata
              Instruction *tmp = ci;
              while (!type && tmp) {
                Instruction *nextTmp = nullptr;
                for (User *U : tmp->users()) {
                  Instruction *UI = dyn_cast<Instruction>(U);
                  if (type)
                    break;
                  if (isa<LoadInst>(U)) {
                    type = U->getType();
                  } else if (StoreInst *strInst = dyn_cast<StoreInst>(U)) {
                    Value *storedVal = strInst->getValueOperand();
                    type = storedVal->getType();
                    if (ZExtInst *zext = dyn_cast<ZExtInst>(storedVal)) {
                      if (zext->getSrcTy()->getScalarType() ==
                          Builder.getInt1Ty())
                        type = Builder.getInt1Ty();
                    }
                  } else if (auto *MT = dyn_cast<MemTransferInst>(U)) {
                    // A buffer only copied to or from another buffer (e.g.
                    // after loop-idiom) takes the other buffer's type.
                    Value *other = MT->getRawDest() == tmp ? MT->getRawSource()
                                                           : MT->getRawDest();
                    if (auto *OI = dyn_cast<Instruction>(other)) {
                      for (auto [md, bits] :
                           {std::pair{"int8", 8}, {"int16", 16},
                            {"int32", 32}, {"int64", 64}})
                        if (OI->hasMetadata(md))
                          type = Builder.getIntNTy(bits);
                    } else if (isa<Argument>(other) &&
                               checkSecretShared->sizesMap[&F].count(other)) {
                      type = Builder.getIntNTy(
                          checkSecretShared->sizesMap[&F][other]);
                    }
                  } else if (CallInst *tmpCI = dyn_cast<CallInst>(U)) {
                    if (tmpCI->getCalledFunction() &&
                        tmpCI->getCalledFunction()->getName().str() ==
                        "_ZN3MPC5storeEPvS0_iibb") {
                      AllocaInst *c =
                          dyn_cast<AllocaInst>(tmpCI->getOperand(1));
                      if (c) {
                        type = c->getAllocatedType();
                        errs() << *c << " " << *type << "\n";
                      }
                    }
                  } else if (!nextTmp && DT.dominates(tmp, UI) &&
                             UI->getNumUses() > 0 &&
                             (UI->getUniqueUndroppableUser() != tmp))
                    nextTmp = dyn_cast<Instruction>(UI);
                }
                tmp = nextTmp;
              }
              if (!type) {
                errs() << "VectorMPCLink: could not determine element type for malloc: " << *ci << "\n";
                continue;
              }
              Builder.SetInsertPoint(ci);
              if (!gc) {
                // Non-gc buffers hold plain shares with the original layout,
                // so the vector and scalar loops share the original malloc.
                // A separate buffer would be read by the vector loops while
                // the scalar loops still write the original, because mpc-link
                // only redirects stores in gc mode.
                argToMPCtype.insert(std::make_pair(ci, ci));
                continue;
              }
              if (type != Builder.getInt8Ty()) {
                Type *scalarType = type->getScalarType();
                TypeSize typeSize =
                    F.getParent()->getDataLayout().getTypeAllocSize(scalarType);
                if (!typeSize.isScalable()) {
                  n = Builder.CreateSDiv(
                      n, ConstantInt::get(n->getType(), typeSize.getFixedValue()));
                }
              }
              auto mpcPtr = createMalloc(Builder, &F, type, n,
                                         ci->hasMetadata("secret_shared"));
              argToMPCtype.insert(std::make_pair(ci, mpcPtr));
              insertPtrToMPCPtr(ci, mpcPtr);
            }
          }
        }
      }

      // Remove free calls on old mallocs that were converted to new buffers.
      // The old mallocs should become dead code; their free calls (which may
      // be misplaced inside loops by the flatten pass) must also be removed.
      {
        SmallVector<CallInst *, 8> freesToRemove;
        for (auto &BB : F)
          for (auto &I : BB)
            if (auto *CI = dyn_cast<CallInst>(&I))
              if (CI->getCalledFunction() &&
                  CI->getCalledFunction()->getName() == "free")
                if (argToMPCtype.find(CI->getArgOperand(0)) !=
                    argToMPCtype.end())
                  freesToRemove.push_back(CI);
        for (auto *CI : freesToRemove)
          CI->eraseFromParent();
      }

      // Convert llvm.memcpy between secret-shared MPC buffers.
      {
        // First pass: collect memcpy instructions to convert
        SmallVector<MemCpyInst *, 4> memcpys;
        for (auto &BB : F)
          for (auto &I : BB)
            if (I.hasMetadata("secret_shared"))
              if (auto *MI = dyn_cast<MemCpyInst>(&I))
                memcpys.push_back(MI);

        // Second pass: convert collected memcpys
        for (auto *MI : memcpys) {
          Value *dst = MI->getDest();
          Value *src = MI->getSource();
          auto dstIt = argToMPCtype.find(dst);
          auto srcIt = argToMPCtype.find(src);
          if (dstIt == argToMPCtype.end() || srcIt == argToMPCtype.end())
            continue;
          Value *dstMpc = dstIt->second;
          Value *srcMpc = srcIt->second;
          Value *len = MI->getLength();
          Builder.SetInsertPoint(MI);
          if (gc) {
            // GC mode: element-wise copy via MPC gep+store
            bool isBit = false;
            if (auto *srcCI = dyn_cast<CallInst>(srcMpc))
              if (srcCI->getCalledFunction() &&
                  srcCI->getCalledFunction()->getName() == GCFuncNames["createBit"])
                isBit = true;
            std::string gepName = isBit ? "_ZN3MPC3gepEPN3emp3BitE" MPC_I64
                                        : "_ZN3MPC3gepEPN3emp7IntegerE" MPC_I64;
            std::string storeName = isBit ? "_ZN3MPC5storeEPN3emp3BitERS1_"
                                          : "_ZN3MPC5storeEPN3emp7IntegerERS1_";
            auto *gepFunc = cast<Function>(
                M.getOrInsertFunction(
                     gepName,
                     FunctionType::get(Builder.getPtrTy(),
                                       {Builder.getPtrTy(), Builder.getInt64Ty()},
                                       false))
                    .getCallee());
            auto *storeFunc2 = cast<Function>(
                M.getOrInsertFunction(
                     storeName,
                     FunctionType::get(Builder.getVoidTy(),
                                       {Builder.getPtrTy(), Builder.getPtrTy()},
                                       false))
                    .getCallee());
            int elemSize = isBit ? 1 : 4;
            if (auto *srcCI = dyn_cast<CallInst>(srcMpc)) {
              if (srcCI->hasMetadata("int64")) elemSize = 8;
              else if (srcCI->hasMetadata("int16")) elemSize = 2;
            }
            Value *nElems = (elemSize > 1)
                ? Builder.CreateUDiv(len, ConstantInt::get(len->getType(), elemSize))
                : len;
            BasicBlock *preheader = MI->getParent();
            BasicBlock *afterBB = preheader->splitBasicBlock(MI, "memcpy.after");
            preheader->getTerminator()->eraseFromParent();
            BasicBlock *loopBB = BasicBlock::Create(M.getContext(), "memcpy.loop", &F, afterBB);
            Builder.SetInsertPoint(preheader);
            Builder.CreateBr(loopBB);
            Builder.SetInsertPoint(loopBB);
            PHINode *iv = Builder.CreatePHI(Builder.getInt64Ty(), 2, "mcpy.iv");
            iv->addIncoming(ConstantInt::get(Builder.getInt64Ty(), 0), preheader);
            Value *srcElem = Builder.CreateCall(gepFunc, {srcMpc, iv});
            Value *dstElem = Builder.CreateCall(gepFunc, {dstMpc, iv});
            Builder.CreateCall(storeFunc2, {dstElem, srcElem});
            Value *ivNext = Builder.CreateAdd(iv, ConstantInt::get(Builder.getInt64Ty(), 1));
            iv->addIncoming(ivNext, loopBB);
            Value *nElems64 = Builder.CreateZExtOrBitCast(nElems, Builder.getInt64Ty());
            Value *cond = Builder.CreateICmpULT(ivNext, nElems64);
            Builder.CreateCondBr(cond, loopBB, afterBB);
            MI->eraseFromParent();
          } else {
            // Non-gc mode: replace memcpy operands with new buffer pointers
            MI->setDest(dstMpc);
            MI->setSource(srcMpc);
          }
        }
      }

      for (auto p : opcountMap) {
        replaceLoop(p.first, p.second.first, p.second.second);
      }
      if (gc) {
        for (auto &KV : writeArgtoMPCtype) {
          auto storeFunc = cast<llvm::Function>(
              M.getOrInsertFunction(
                   "_ZN3MPC8writeArgEPN3emp7IntegerEPvii",
                   FunctionType::get(Type::getVoidTy(M.getContext()),
                                     {Builder.getPtrTy(), Builder.getPtrTy(),
                                      Builder.getInt32Ty(),
                                      Builder.getInt32Ty()},
                                     false))
                  .getCallee());
          // Insert writeArg before the function return so it runs after the
          // loop has written all results into the MPC buffer.
          Instruction *retInst = nullptr;
          for (auto &BB : F)
            if (ReturnInst *ret = dyn_cast<ReturnInst>(BB.getTerminator()))
              retInst = ret;
          if (retInst)
            Builder.SetInsertPoint(retInst);
          else {
            Instruction *I = dyn_cast<Instruction>(KV.second);
            Builder.SetInsertPoint(I->getInsertionPointAfterDef().value());
          }
          // writeArg signature: (emp::Integer* mpc_ptr, void* raw_ptr, int n, int elemSize)
          Builder.CreateCall(
              storeFunc,
              {KV.second, KV.first,
               checkSecretShared->writeArgAccess[&F][KV.first],
               ConstantInt::get(Builder.getInt32Ty(),
                                checkSecretShared->sizesMap[&F][KV.first])});
        }
      }
      opcountMap.clear();
      writeArgtoMPCtype.clear();
      Args->clear();
    }
  }
  return PreservedAnalyses::all();
}
