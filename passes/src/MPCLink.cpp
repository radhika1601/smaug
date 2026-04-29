#include "MPCLink.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "VectorMPCLink.h"


extern cl::opt<std::string> MetadataFilePath;
extern cl::opt<bool> UseGCMode;

#define DEBUG_TYPE "mpc-link"
#include "CheckSecretShared.h"

bool MPCLinkPass::isSecretShared(Value *val, SmallVector<Argument *> &Args) {
  if (Instruction *I = dyn_cast<Instruction>(val)) {
    if (I->hasMetadata("secret_shared"))
      return true;
  }
  if (std::find(Args.begin(), Args.end(), val) != Args.end())
    return true;

  // TODO: check args
  return false;
}

void MPCLinkPass::setSecretShared(Value *val) {

  Instruction *I = dyn_cast<Instruction>(val);
  Function *F = I->getParent()->getParent();
  auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
  auto *node = MDNode::get(F->getContext(), MDStr);
  I->setMetadata("secret_shared", node);
}

Value *MPCLinkPass::i1Toi8(Value *v, IRBuilder<> &Builder, Type *boolTy) {
  return Builder.CreateZExt(v, boolTy);
}
Value *MPCLinkPass::i8Toi1(Value *v, IRBuilder<> &Builder, Type *i1Ty) {
  return Builder.CreateTrunc(v, i1Ty);
}

Value *MPCLinkPass::isAlice(IRBuilder<> &Builder, Module *module,
                            Function *F = nullptr) {
  // if(isAliceInst == nullptr){
  //   if(F != nullptr){
  //     BasicBlock &entry = F->getEntryBlock();
  //     Builder.SetInsertPoint(entry.getFirstInsertionPt());
  //   }
  llvm::FunctionType *funcType =
      FunctionType::get(Type::getInt1Ty(Builder.getContext()), {}, false);

  llvm::FunctionCallee isAliceFuncCallee =
      module->getOrInsertFunction("_ZN3MPC7isAliceEv", funcType);
  Function *isAliceFunc = llvm::cast<Function>(isAliceFuncCallee.getCallee());
  isAliceInst = Builder.CreateCall(isAliceFunc, {}, "isAlice");
  // }

  return isAliceInst;
}

bool MPCLinkPass::icmpCall(Instruction &I, SmallVector<Argument *> &Args) {
  //  LLVM_DEBUG(dbgs() << "icmp " << I << "\n");
  Function *F = I.getParent()->getParent();
  LLVMContext &context = F->getContext();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(&I);

  auto module = F->getParent();
  auto intTy = Type::getInt32Ty(context);
  auto boolTy = Type::getInt8Ty(context);
  std::string name;
  if (ICmpInst *cmpInst = dyn_cast<ICmpInst>(&I)) {
    std::string name = "icmpEq";
    Value *in1 = I.getOperand(0), *in2 = I.getOperand(1);

    if(!gc){
    if (!isSecretShared(in1, Args)) {
      in1 = makeSecretShared(in1, Builder, module, F);
    }
    if (!isSecretShared(in2, Args)) {
      in2 = makeSecretShared(in2, Builder, module, F);
    }}
    auto type = I.getOperand(0)->getType();
    int op = 1;
    if (cmpInst->isEquality()) {
      op = 1;
    } else if (cmpInst->isGT(cmpInst->getPredicate())) {
      op = 2;
    } else if (cmpInst->isGE(cmpInst->getPredicate())) {
      op = 3;
    } else {
      LLVM_DEBUG(dbgs() << "cmp run MPCRemoveOpsPass first \n");
      return false;
    }
    if (type == Type::getInt1Ty(context)) {
      LLVM_DEBUG(dbgs() << "icmp type i1 run mpcRemove ops first\n");
    } else if (type == boolTy) {
      name += "8";
    } else if (type == Type::getInt16Ty(context)) {
      name += "16";
    } else if (type == Type::getInt32Ty(context)) {
      name += "32";
    } else if (type == Type::getInt64Ty(context)) {
      name += "64";
    } else {
      LLVM_DEBUG(dbgs() << I << " equal type not found\n");
      return false;
    }

    llvm::FunctionCallee icmpEqFuncCallee;
    if (gc) {
      icmpEqFuncCallee = module->getOrInsertFunction(
          GCFuncNames[name],
          FunctionType::get(
              Builder.getPtrTy(),
              {Builder.getPtrTy(), Builder.getPtrTy(), Builder.getInt32Ty()},
              false));
    } else {
    llvm::FunctionType *funcType =
        FunctionType::get(Builder.getInt1Ty(), {type, type, intTy}, false);
      icmpEqFuncCallee = module->getOrInsertFunction(FuncNames[name], funcType);
    }
    Function *icmpEq = llvm::cast<Function>(icmpEqFuncCallee.getCallee());
    Builder.SetInsertPoint(&I);

    std::vector<Value *> args = {in1, in2, ConstantInt::get(intTy, op)};
    if (gc) {
      if (ItoMPCI.find(in1) != ItoMPCI.end())
        in1 = ItoMPCI[in1];
      else
        in1 = makeSecretShared(in1, Builder, module, F);
      if (ItoMPCI.find(in2) != ItoMPCI.end())
        in2 = ItoMPCI[in2];
      else
        in2 = makeSecretShared(in2, Builder, module, F);
      args = {in1, in2, ConstantInt::get(intTy, op)};
    }
    auto output = Builder.CreateCall(icmpEq, args);
    setSecretShared(output);
    if (gc)
      ItoMPCI.insert(std::make_pair(&I, output));
    else
      I.replaceAllUsesWith(output);
    return true;
  }

  return false;
}

bool MPCLinkPass::fcmpCall(Instruction &I, SmallVector<Argument *> &Args) {
  Function *F = I.getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(&I);

  auto module = F->getParent();
  std::string name = "fcmp";
  if (FCmpInst *cmpInst = dyn_cast<FCmpInst>(&I)) {
    Value *in1 = I.getOperand(0), *in2 = I.getOperand(1);
    if (!isSecretShared(in1, Args))
      in1 = makeSecretShared(in1, Builder, module, F);
    if (!isSecretShared(in2, Args))
      in2 = makeSecretShared(in2, Builder, module, F);

    auto type = I.getOperand(0)->getType();
    int op = 1;
    switch (cmpInst->getPredicate()) {
    case FCmpInst::FCMP_OEQ:
    case FCmpInst::FCMP_UEQ: {
      op = 1;
      break;
    }
    case FCmpInst::FCMP_OGT:
    case FCmpInst::FCMP_UGT: {
      op = 2;
      break;
    }
    case FCmpInst::FCMP_OGE:
    case FCmpInst::FCMP_UGE: {
      op = 3;
      break;
    }
    case FCmpInst::FCMP_ONE:
    case FCmpInst::FCMP_UNE: {
      op = 4;
      break;
    }
    case FCmpInst::FCMP_OLE:
    case FCmpInst::FCMP_ULE: {
      auto tmp = in1;
      in1 = in2;
      in2 = tmp;
      op = 3;
      break;
    }
    case FCmpInst::FCMP_OLT:
    case FCmpInst::FCMP_ULT: {
      auto tmp = in1;
      in1 = in2;
      in2 = tmp;
      op = 2;
      break;
    }
    default:
      break;
    }
    llvm::FunctionType *funcType = FunctionType::get(
        Builder.getInt1Ty(), {type, type, Builder.getInt32Ty()}, false);
    llvm::FunctionCallee icmpEqFuncCallee =
        module->getOrInsertFunction(FuncNames[name], funcType);
    Function *icmpEq = llvm::cast<Function>(icmpEqFuncCallee.getCallee());
    Builder.SetInsertPoint(&I);

    std::vector<Value *> args = {in1, in2,
                                 ConstantInt::get(Builder.getInt32Ty(), op)};
    auto output = Builder.CreateCall(icmpEq, args);
    setSecretShared(output);
    I.replaceAllUsesWith(output);
    return true;
  }

  return false;
}

bool MPCLinkPass::findFunc(Instruction &I, std::string name,
                           SmallVector<Argument *> &Args) {
  Function *F = I.getParent()->getParent();
  LLVMContext &context = F->getContext();
  llvm::IRBuilder<> Builder(F->getContext());

  auto module = F->getParent();
  auto boolTy = Type::getInt8Ty(context);

  Value *op1 = I.getOperand(0);
  Value *op2 = I.getOperand(1);
  auto type = I.getType();
  if (type == Type::getInt1Ty(context) && (name == "and" || name == "xor")) {
    name += "1";
    type = Type::getInt1Ty(context);
  } else if (type == boolTy || type == Type::getInt1Ty(context)) {
    name += "8";
    type = boolTy;
  } else if (type == Type::getInt16Ty(context)) {
    name += "16";
  } else if (type == Type::getInt32Ty(context)) {
    name += "32";
  } else if (type == Type::getInt64Ty(context)) {
    name += "64";
  } else if (type == Builder.getFloatTy()) {
  } else {
    LLVM_DEBUG(dbgs() << I << " type not found\n");
    return false;
  }
  Builder.SetInsertPoint(&I);
  if (!gc) {
    if (!isSecretShared(op1, Args)) {
      op1 = makeSecretShared(op1, Builder, module, F);
    }
    if (!isSecretShared(op2, Args)) {
      op2 = makeSecretShared(op2, Builder, module, F);
    }
    if (I.getType() == Type::getInt1Ty(context) && name != "and1") {
      op1 = i1Toi8(op1, Builder, boolTy);
      op2 = i1Toi8(op2, Builder, boolTy);
    }
  }

  llvm::FunctionCallee FuncCallee;
  if (gc) {
    FuncCallee = module->getOrInsertFunction(
        GCFuncNames[name],
        FunctionType::get(Builder.getPtrTy(),
                          {Builder.getPtrTy(), Builder.getPtrTy()}, false));
    if (ItoMPCI.find(op1) != ItoMPCI.end())
      op1 = ItoMPCI[op1];
    else {
      op1 = makeSecretShared(op1, Builder, module, F);
    }
    if (ItoMPCI.find(op2) != ItoMPCI.end()) {
      op2 = ItoMPCI[op2];
    } else {
      op2 = makeSecretShared(op2, Builder, module, F);
    }
  } else {

    llvm::FunctionType *funcType =
        FunctionType::get(I.getType(), {type, type}, false);
    FuncCallee = module->getOrInsertFunction(FuncNames[name], funcType);
  }
  Function *Func = llvm::cast<Function>(FuncCallee.getCallee());

  std::vector<Value *> args = {op1, op2};
  auto output = Builder.CreateCall(Func, args);
  setSecretShared(output);
  if (gc) {
    ItoMPCI.insert(std::make_pair(&I, output));
  } else {
    if (I.getType() == output->getType())
      I.replaceAllUsesWith(output);
    else {
      auto i1 = i8Toi1(output, Builder, I.getType());
      setSecretShared(i1);
      I.replaceAllUsesWith(i1);
    }
  }
  return true;
}

bool MPCLinkPass::publicXor(Instruction &I, SmallVector<Argument *> &Args) {
  auto op1 = I.getOperand(0);
  auto op2 = I.getOperand(1);

  Function *F = I.getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(&I);
  if (!isSecretShared(op1, Args)) {
    if (Constant *c = dyn_cast<Constant>(op1))
      if (c->isZeroValue())
        return false;
    auto v = makeSecretShared(op1, Builder, F->getParent(), F);
    I.setOperand(0, v);
  } else if (!isSecretShared(op2, Args)) {
    if (Constant *c = dyn_cast<Constant>(op2))
      if (c->isZeroValue())
        return false;
    auto v = makeSecretShared(op2, Builder, F->getParent(), F);
    I.setOperand(1, v);
  }

  return false;
}

bool MPCLinkPass::updatePrivateStore(Instruction &I) {

  // store => (select (i == index) val load[i]), store at i

  return true;
}

bool MPCLinkPass::updatePrivateLoad(Instruction &I,
                                    SmallVector<Argument *> &Args) {

  Function *F = I.getParent()->getParent();
  LLVMContext &context = F->getContext();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(&I);

  auto module = F->getParent();
  auto boolTy = Type::getInt8Ty(context);

  std::string name;
  Value *idxptr = nullptr;
  Value *n = F->getArg(F->arg_size() - 1);

  Type *type = I.getType();
  if (I.getOpcode() == Instruction::Load) {
    name = "load";
    idxptr = I.getOperand(0);
  } else {
    name = "store";
    idxptr = I.getOperand(1);
    type = I.getOperand(0)->getType();
  }
  // load from all indices, and with i == index, xor all of them
  // Value *op0 = I.getOperand(0);
  Value *idx = nullptr;
  Value *ptr = nullptr;
  if (Instruction *getptr = dyn_cast<GetElementPtrInst>(idxptr)) {
    idx = getptr->getOperand(1);
    ptr = getptr->getOperand(0);
    Builder.SetInsertPoint(getptr->getInsertionPointAfterDef().value());
    LLVM_DEBUG(dbgs() << "idx " << *idx << " n" << *n << "\n");
    idx = Builder.CreateZExtOrTrunc(idx, Builder.getInt32Ty());
    LLVM_DEBUG(dbgs() << "new idx " << *idx << "\n");
    n = Builder.CreateZExtOrTrunc(n, Builder.getInt32Ty());
  }
  llvm::FunctionType *funcType = nullptr;
  std::vector<Value *> args;
  Value *output;
  if (type == Builder.getInt8Ty() || type == Builder.getInt1Ty()) {
    I8Output = createOutput(I8Output, F, Builder, Builder.getInt8Ty());
    output = I8Output;
    type = boolTy;
  } else if (type == Type::getInt16Ty(context)) {
    I16Output = createOutput(I16Output, F, Builder, type);
    output = I16Output;
  } else if (type == Type::getInt32Ty(context)) {
    I32Output = createOutput(I32Output, F, Builder, type);
    output = I32Output;
  } else if (type == Type::getInt64Ty(context)) {
    I64Output = createOutput(I64Output, F, Builder, type);
    output = I64Output;
  } else if (type == Builder.getFloatTy()) {
    I32Output = createOutput(I32Output, F, Builder, Builder.getInt32Ty());
    output = I32Output;
  } else {
    LLVM_DEBUG(dbgs() << I << " type not found\n");
    return false;
  }
  if (I.getOpcode() == Instruction::Load) {
    funcType = FunctionType::get(Builder.getVoidTy(),
                                 {Builder.getPtrTy(), Builder.getInt32Ty(),
                                  Builder.getPtrTy(), Builder.getInt32Ty(),
                                  Builder.getInt32Ty(), Builder.getInt1Ty()},
                                 false);
    args = {ptr,
            idx,
            output,
            n,
            Builder.getInt32(module->getDataLayout().getTypeAllocSize(type)),
            Builder.getInt1(!isSecretShared(ptr, Args))};
  } else {
    Value *v = I.getOperand(0);
    funcType = FunctionType::get(Builder.getVoidTy(),
                                 {Builder.getPtrTy(), Builder.getPtrTy(),
                                  Builder.getInt32Ty(), Builder.getInt32Ty(),
                                  Builder.getInt32Ty(), Builder.getInt1Ty()},
                                 false);
    auto store = dyn_cast<Instruction>(Builder.CreateStore(v, output));
    store->moveBefore(&I);
    args = {ptr,
            output,
            idx,
            n,
            Builder.getInt32(module->getDataLayout().getTypeAllocSize(type)),
            Builder.getInt1(!isSecretShared(v, Args))};
  }

  Function *Func = llvm::cast<Function>(
      module->getOrInsertFunction(FuncNames[name], funcType).getCallee());
  auto call = dyn_cast<Instruction>(Builder.CreateCall(Func, args));
  call->moveBefore(&I);
  if (I.getOpcode() == Instruction::Load) {
    Instruction *newLoad =
        dyn_cast<Instruction>(Builder.CreateLoad(type, output));
    newLoad->moveAfter(call);
    setSecretShared(newLoad);
    I.replaceAllUsesWith(newLoad);
  }
  return true;
}

void MPCLinkPass::updatePhi(Instruction *I, SmallVector<Argument *> &Args) {

  Function *F = I->getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());

  if (PHINode *phi = dyn_cast<PHINode>(I)) {
    PHINode *newPhi;
    if (gc) {
      Builder.SetInsertPoint(I);
      newPhi =
          Builder.CreatePHI(Builder.getPtrTy(), phi->getNumIncomingValues());
      ItoMPCI.insert(std::make_pair(phi, newPhi));
      errs() << *phi << " " << *newPhi << "\n";
    }
    for (size_t i = 0; i < phi->getNumIncomingValues(); ++i) {
      BasicBlock *iB = phi->getIncomingBlock(i);
      Value *iV = phi->getIncomingValue(i);

      if (isSecretShared(iV, Args)) {
        if (gc && (ItoMPCI.find(iV) != ItoMPCI.end())) {
          newPhi->addIncoming(ItoMPCI[iV], iB);
        }
        continue;
      }
      if (isa<Constant>(iV)) {
        Builder.SetInsertPoint(iB->getFirstInsertionPt());
      } else {
        Instruction *iI = dyn_cast<Instruction>(iV);
        Builder.SetInsertPoint(iI->getInsertionPointAfterDef().value());
      }
      auto v = makeSecretShared(iV, Builder, F->getParent(), F);
      if (gc)
        newPhi->addIncoming(v, iB);
      else
        phi->setIncomingValueForBlock(iB, v);
    }
  }
}

bool MPCLinkPass::isPrivateLoadStore(Instruction *Ld,
                                     SmallVector<Argument *> &Args) {
  Value *op0 = nullptr;
  if (Ld->getOpcode() == Instruction::Load)
    op0 = Ld->getOperand(0);
  else if (Ld->getOpcode() == Instruction::Store)
    op0 = Ld->getOperand(1);
  else
    return false;
  if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(op0)) {
    auto index = gep->getOperand(1);
    if (isSecretShared(index, Args)) {
      errs() << "index " << *index << " " << *Ld << "\n";
      return true;
    }
  }
  if (PHINode *phi = dyn_cast<PHINode>(op0)) {
    for (uint i = 0; i < phi->getNumIncomingValues(); ++i) {
      if (GetElementPtrInst *gep =
              dyn_cast<GetElementPtrInst>(phi->getIncomingValue(i))) {
        auto index = gep->getOperand(1);
        if (isSecretShared(index, Args)) {
          errs() << "index " << *index << " " << *Ld << "\n";
          return true;
        }
      }
    }
  }
  return false;
}

void MPCLinkPass::revealOutput(Instruction &I) {
  std::string name;
  Function *F = I.getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Value *op = I.getOperand(0);
  if (op->getType() == Builder.getInt1Ty())
    name = "revI1";
  else if (op->getType() == Builder.getInt8Ty())
    name = "revI8";
  else if (op->getType() == Builder.getInt16Ty())
    name = "revI16";
  else if (op->getType() == Builder.getInt32Ty())
    name = "revI32";
  else if (op->getType() == Builder.getInt64Ty())
    name = "revI64";
  else {
    LLVM_DEBUG(dbgs() << "return type: no reveal function " << I << "\n");
  }
  Function *Func = llvm::cast<Function>(
      F->getParent()
          ->getOrInsertFunction(
              FuncNames[name],
              FunctionType::get(op->getType(),
                                {op->getType(), Builder.getInt32Ty()}, false))
          .getCallee());
  Builder.SetInsertPoint(&I);
  auto val = Builder.CreateCall(Func, {op, Builder.getInt32(1)});
  I.setOperand(0, val);
}

void MPCLinkPass::replaceFunc(Function &F, SmallVector<Argument *> &Args,
                              char output) {
  std::vector<Instruction *> removeInsts;
  llvm::IRBuilder<> Builder(F.getContext());
  std::map<std::string, Value *> mallocs;

  // get mpc ptrs for input args;
  if (gc) {
    for (auto &p : (checkSecretShared->readArgAccess[&F]))
      mallocs.insert(std::make_pair(p.first->getName(), p.first));
    std::set<Instruction *> mpcptrs;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getName().ends_with(".mpc")) {
          mpcptrs.insert(&I);
        } else if (isa<AllocaInst>(I) || isa<CallInst>(I))
          if (I.hasName()) {
            mallocs.insert(std::make_pair(I.getName(), &I));
          }
      }
    }
    for (Instruction *I : mpcptrs) {
      std::string name = I->getName().str();
      name = name.substr(0, name.size() - 4);
      if (mallocs.find(name) != mallocs.end()) {
        PtrToMPCPtr.insert(std::make_pair(mallocs[name], I));
        Type *type = getTypefromMetadata(I, Builder);
        updatePtrUses(mallocs[name], I, Builder, removeInsts, type);
      }
    }
    mpcptrs.clear();
    mallocs.clear();
  }

  std::set<Value *> phistoremove;
  isAliceInst = nullptr;
  for (auto &BB : F) {
    I1Output = nullptr;
    I8Output = nullptr;
    I16Output = nullptr;
    I32Output = nullptr;
    I64Output = nullptr;
    for (auto &I : BB) {
      if (I.hasMetadata("secret_shared")) {
        bool remove = false;
        switch (I.getOpcode()) {
        case Instruction::PHI:
          if (I.getType() == Builder.getPtrTy()) {
            if (PtrToMPCPtr.find(&I) != PtrToMPCPtr.end()) {
              phistoremove.insert(&I);
              break;
            }
            break;
          }
          updatePhi(&I, Args);
          if (gc)
            phistoremove.insert(&I);
          break;
        case Instruction::GetElementPtr:
          if (PtrToMPCPtr.find(&I) != PtrToMPCPtr.end())
            break;
          LLVM_FALLTHROUGH;
        case Instruction::ZExt:
          if (gc) {
            if (I.getType() == Builder.getInt8Ty() &&
                I.getOperand(0)->getType() == Builder.getInt1Ty()) {
              if (isa<StoreInst>(I.getUniqueUndroppableUser()) &&
                  ItoMPCI.find(I.getOperand(0)) != ItoMPCI.end()) {
                ItoMPCI.insert(std::make_pair(&I, ItoMPCI[I.getOperand(0)]));
                remove = true;
                break;
              }
            }
          }
          errs() << "unchanged " << I << "\n";
          break;
        case Instruction::SExt:
          if (gc) {
            Function *func = cast<llvm::Function>(
                F.getParent()
                    ->getOrInsertFunction(
                        GCFuncNames["bittoint"],
                        FunctionType::get(
                            Builder.getPtrTy(),
                            {Builder.getPtrTy(), Builder.getInt32Ty()}, false))
                    .getCallee());
            Builder.SetInsertPoint(&I);
            auto elementsize =
                F.getParent()->getDataLayout().getTypeSizeInBits(I.getType());
            auto mpc =
                Builder.CreateCall(func, {ItoMPCI[I.getOperand(0)],
                                          Builder.getInt32(elementsize)});
            ItoMPCI.insert(std::make_pair(&I, mpc));
            remove = true;
          }
          break;
        case Instruction::Trunc:
          if (gc) {
            if (I.getType() == Builder.getInt1Ty() &&
                I.getOperand(0)->getType() == Builder.getInt8Ty()) {
              if ((isa<LoadInst>(I.getOperand(0))) &&
                  (ItoMPCI.find(I.getOperand(0)) != ItoMPCI.end())) {
                ItoMPCI.insert(std::make_pair(&I, ItoMPCI[I.getOperand(0)]));
                remove = true;
                break;
              }
            }
          }
          LLVM_FALLTHROUGH;
        case Instruction::BitCast:
        case Instruction::Alloca:
          if (ItoMPCI.find(&I) != ItoMPCI.end())
            break;
          errs() << "unchanged " << I << "\n";
          break;
        case Instruction::Load:
          if (ItoMPCI.find(&I) != ItoMPCI.end())
            break;
          if (gc) {
            Value *ptr = I.getOperand(0);
            if (CallInst *CI = dyn_cast<CallInst>(ptr)) {
              if (CI->getCalledFunction()->getName().contains("ZN3MPC")) {
                ItoMPCI.insert(std::make_pair(&I, CI));
                remove = true;
                break;
              }
            }
          }
          LLVM_FALLTHROUGH;
        case Instruction::Store: {
          Value *val = I.getOperand(0);
          if (I.getOpcode() == Instruction::Store)
            val = I.getOperand(1);
          // Also check if val is secret-shared argument
          if (Instruction *ptr = dyn_cast<Instruction>(val)) {
            if (ptr->hasMetadata("secret_shared")) {
              // If the ptr comes from some malloc instruction then use mpc ptr
              // type
              if (gc && isa<StoreInst>(&I)) {
                Instruction *basePtr = ptr;
                Value *MPCPtr = nullptr;
                if (PtrToMPCPtr.find(basePtr) != PtrToMPCPtr.end())
                  MPCPtr = PtrToMPCPtr[basePtr];
                if (!MPCPtr)
                  if (CallInst *CI = dyn_cast<CallInst>(basePtr)) {
                    if (CI->getCalledFunction()->getName().contains("ZN3MPC"))
                      MPCPtr = basePtr;
                  }
                if (!MPCPtr) {
                  errs() << "store at " << *basePtr
                         << " base ptr does not have mpc ptr created\n";
                  // Print the GEP's base pointer if applicable
                  if (auto *GEP = dyn_cast<GetElementPtrInst>(basePtr))
                    errs() << "  GEP base: " << *GEP->getPointerOperand() << "\n";
                  // Print what IS in PtrToMPCPtr
                  errs() << "  PtrToMPCPtr entries (" << PtrToMPCPtr.size() << "):\n";
                  for (auto &[k, v] : PtrToMPCPtr)
                    errs() << "    key=" << *k << "  val=" << *v << "\n";
                }
                if (MPCPtr) {
                  StoreInst *store = dyn_cast<StoreInst>(&I);
                  Value *v = store->getValueOperand();
                  auto name = "storeInt";
                  if (v->getType() == Builder.getInt8Ty() ||
                      v->getType() == Builder.getInt1Ty())
                    name = "storeBit";

                  auto ldVal = ItoMPCI[v];
                  Builder.SetInsertPoint(&I);
                  if (!ldVal) {
                    ldVal = makeSecretShared(v, Builder, F.getParent(), &F);
                  }
                  Function *strFunc = cast<llvm::Function>(
                      F.getParent()
                          ->getOrInsertFunction(
                              GCFuncNames[name],
                              FunctionType::get(
                                  Builder.getVoidTy(),
                                  {Builder.getPtrTy(), Builder.getPtrTy()},
                                  false))
                          .getCallee());
                  Builder.CreateCall(strFunc, {MPCPtr, ldVal});
                  remove = true;
                } else {
                  errs() << "714 mpc ptr not found \n";
                }
              }

              if (isPrivateLoadStore(&I, Args)) {
                if (I.getType()->isPointerTy()) {
                  LLVM_DEBUG(dbgs()
                             << "Found secret load or store of ptr type in "
                             << F.getName().str() << "\n");
                  LLVM_DEBUG(dbgs()
                             << "Seems there is a non 1-D array with private "
                                "access, please change it to one-D array\n");
                  return;
                } else {
                  LLVM_DEBUG(dbgs()
                             << formatv("load/store at a private index. Using "
                                        "the arg of function {0} as the total "
                                        "number of elements in the array\n",
                                        F.getName().str()));
                  remove = updatePrivateLoad(I, Args);
                }
              }
            }
          }
          break;
        }
        case Instruction::Select: {
          if (SelectInst *selInst = dyn_cast<SelectInst>(&I)) {
            auto condition = selInst->getCondition();
            if (!isSecretShared(condition, Args)) {
              if (gc) {
                Builder.SetInsertPoint(selInst);
                auto newInst = Builder.CreateSelect(
                    condition, ItoMPCI[selInst->getTrueValue()],
                    ItoMPCI[selInst->getFalseValue()]);
                ItoMPCI.insert(std::make_pair(selInst, newInst));
                remove = true;
              }
              break;
            }
          }
          LLVM_DEBUG(dbgs()
                     << "SELECT run MPCRemoveOpsPass first " << I << "\n");
          break;
        }
        case Instruction::ICmp: {
          remove = icmpCall(I, Args);
          break;
        }
        case Instruction::FCmp: {
          remove = fcmpCall(I, Args);
          break;
        }
        case Instruction::Or:
          LLVM_DEBUG(dbgs() << "OR run MPCRemoveOpsPass first \n");
          break;
        case Instruction::And:
          if (gc)
            remove = findFunc(I, "and", Args);
          else if (isSecretShared(I.getOperand(0), Args) &&
                   isSecretShared(I.getOperand(1), Args))
            remove = findFunc(I, "and", Args);
          break;
        case Instruction::Add:
          remove = findFunc(I, "add", Args);
          break;
        case Instruction::Mul:
          remove = findFunc(I, "mul", Args);
          break;
        case Instruction::Sub:
          remove = findFunc(I, "sub", Args);
          break;
        case Instruction::SDiv:
          remove = findFunc(I, "div", Args);
          break;
        case Instruction::FAdd:
          remove = findFunc(I, "fadd", Args);
          break;
        case Instruction::FMul:
          remove = findFunc(I, "fmul", Args);
          break;
        case Instruction::FSub:
          remove = findFunc(I, "fsub", Args);
          break;
        case Instruction::FDiv:
          remove = findFunc(I, "fdiv", Args);
          break;
        case Instruction::Xor:
          if (gc)
            remove = findFunc(I, "xor", Args);
          else
            remove = publicXor(I, Args);
          break;
        case Instruction::Call:
          if (CallInst *callInst = dyn_cast<CallInst>(&I)) {
            std::string name = callInst->getCalledFunction()->getName().str();
            if ((name.find("llvm.smax") != std::string::npos) ||
                (name.find("llvm.smin") != std::string::npos)) {
              LLVM_DEBUG(dbgs() << "call run MPCRemoveOpsPass first \n");
            } else if (name.find("llvm.memset") != std::string::npos) {
              break;
            } else if (name.find("llvm.memcpy") != std::string::npos) {
              break;
            } else if (name.find("llvm") != std::string::npos) {
              LLVM_DEBUG(dbgs() << I << "\n");
              // } else if (name == "malloc") {
              //   if (PtrToMPCPtr.find(callInst) == PtrToMPCPtr.end()) {
              //     llvm::Function *createInputMPCTypes = cast<llvm::Function>(
              //         F.getParent()
              //             ->getOrInsertFunction(
              //                 GCFuncNames["createInt"],
              //                 FunctionType::get(Builder.getPtrTy(),
              //                                   {Builder.getInt64Ty()},
              //                                   false))
              //             .getCallee());
              //     Builder.SetInsertPoint(callInst);
              //     Value *n = callInst->getArgOperand(0);
              //     // get size from uses
              //     Type *type = nullptr;
              //     Instruction *ci = callInst;
              //     while (!type && ci) {
              //       Instruction *nextInst = nullptr;
              //       for (User *user : ci->users()) {
              //         if (isa<CallInst>(user)) {
              //           CallInst *tmp = dyn_cast<CallInst>(user);
              //           if (tmp->getCalledFunction()->getName().str() ==
              //               "_ZN3MPC5storeEPvS0_iibb") {
              //             auto op1 = tmp->getArgOperand(1);
              //             if (AllocaInst *a = dyn_cast<AllocaInst>(op1))
              //               type = a->getAllocatedType();
              //           }
              //         } else if (isa<LoadInst>(user)) {
              //           type = user->getType();
              //           break;
              //         } else if (isa<StoreInst>(user)) {
              //           auto strInst = dyn_cast<StoreInst>(user);
              //           type = strInst->getValueOperand()->getType();
              //           break;
              //         }
              //         if (GetElementPtrInst *gep =
              //                 dyn_cast<GetElementPtrInst>(user)) {
              //           nextInst = gep;
              //         }
              //       }
              //       ci = nextInst;
              //     }
              //     if (!type) {
              //       errs() << *callInst << "\n";
              //     }
              //     Builder.SetInsertPoint(callInst);
              //     uint64_t elementSize =
              //         F.getParent()->getDataLayout().getTypeAllocSize(type);
              //     n = Builder.CreateSDiv(
              //         n, ConstantInt::get(n->getType(), elementSize));
              //     auto MPCPtr = Builder.CreateCall(createInputMPCTypes, {n});
              //     PtrToMPCPtr.insert(std::make_pair(callInst, MPCPtr));
              //     updatePtrUses(callInst, MPCPtr, Builder, removeInsts);
              //   }
            }
          }
          break;
        case Instruction::Ret:
          if (gc) {
            if (ItoMPCI.find(I.getOperand(0)) != ItoMPCI.end()) {
              std::string name = "revealInt";
              auto type = Builder.getInt64Ty();
              if (I.getOperand(0)->getType() == Builder.getInt1Ty()) {
                name = "revealBit";
                type = Builder.getInt1Ty();
              }
              Function *func = cast<llvm::Function>(
                  F.getParent()
                      ->getOrInsertFunction(
                          GCFuncNames[name],
                          FunctionType::get(type, {Builder.getPtrTy()}, false))
                      .getCallee());
              Builder.SetInsertPoint(&I);
              Value *tmp = Builder.CreateCall(func, {ItoMPCI[I.getOperand(0)]});
              if (type != I.getOperand(0)->getType())
                tmp = Builder.CreateTruncOrBitCast(tmp,
                                                   I.getOperand(0)->getType());
              I.setOperand(0, tmp);
            }
          }
          if (output == 0)
            revealOutput(I);
          break;
        default:
          LLVM_DEBUG(dbgs() << I << "\n");
          break;
        }
        if (remove) {
          removeInsts.push_back(&I);
        }
      }
    }
  }
  std::set<BasicBlock *> blocks;
  for (auto *I : phistoremove) {
    PHINode *phi = dyn_cast<PHINode>(I);
    PHINode *newPhi = nullptr;
    if (ItoMPCI.find(phi) != ItoMPCI.end())
      newPhi = dyn_cast<PHINode>(ItoMPCI[phi]);
    for (uint i = 0; i < phi->getNumIncomingValues(); ++i) {
      BasicBlock *iB = phi->getIncomingBlock(i);
      Value *iV = phi->getIncomingValue(i);
      blocks.insert(iB);
      if (newPhi && (newPhi->getBasicBlockIndex(iB) == -1)) {
        if (ItoMPCI.find(iV) == ItoMPCI.end())
          errs() << "iv not in mpc inst map " << *iV << "\n";
        newPhi->addIncoming(ItoMPCI[iV], iB);
      }
    }
    for (BasicBlock *iB : blocks) {
      phi->removeIncomingValue(iB, false);
    }
    if (std::find(removeInsts.begin(), removeInsts.end(), phi) ==
        removeInsts.end())
      removeInsts.push_back(phi);
    blocks.clear();
  }

  auto itr = removeInsts.begin();
  while (removeInsts.size() > 0) {
    if (itr == removeInsts.end())
      itr = removeInsts.begin();
    else if (*itr == nullptr)
      itr = removeInsts.erase(itr);
    else {
      Instruction *I = *itr;
      if (I->getNumUses() == 0) {
        removeInsts.erase(itr);
        I->eraseFromParent();
      } else {
        bool rem = false;
        for (User *user : I->users()) {
          if (std::find(removeInsts.begin(), removeInsts.end(), user) ==
              removeInsts.end()) {
            errs() << "remove: " << *I << " user " << *user << "\n";
            rem = true;
            break;
          }
        }
        if (rem) {
          removeInsts.erase(itr);
          continue;
        }
        itr++;
      }
    }
  }

  PtrToMPCPtr.clear();
}

// void MPCLinkPass::vectorizedLoop(Loop &L) {}

llvm::PreservedAnalyses MPCLinkPass::run(Module &M,
                                         ModuleAnalysisManager &MAM) {
  gc = UseGCMode;
  errs() << "mpclink pass\n";
  std::string filePath = formatv("{0}", MetadataFilePath);
  checkSecretShared = new CheckSecretShared(filePath, M);

  bool containsSetup = false, containsShutdown = false;
  for (Function &F : M) {
    if (F.getName() == "main") {
      SmallDenseSet<Instruction *> retInsts;
      if (!containsShutdown || !containsSetup) {
        for (auto &BB : F) {
          for (auto &I : BB) {
            if (CallInst *ci = dyn_cast<CallInst>(&I)) {
              std::string name = ci->getCalledFunction()->getName().str();
              if ((name.find("setup") != std::string::npos) &&
                  (name.find("MPC") != std::string::npos)) {
                containsSetup = true;
              } else if ((name.find("finish") != std::string::npos) &&
                         (name.find("MPC") != std::string::npos)) {
                containsShutdown = true;
              }
            } else if (ReturnInst *retInst = dyn_cast<ReturnInst>(&I)) {
              retInsts.insert(retInst);
            }
          }
        }
      }
      Module *module = F.getParent();
      LLVMContext &context = F.getContext();
      auto funcType = FunctionType::get(Type::getVoidTy(context), {}, false);

      if (!containsSetup) {
        BasicBlock &BB = F.getEntryBlock();
        auto FuncCallee =
            module->getOrInsertFunction(FuncNames["setup"], funcType);
        Function *Func = llvm::cast<Function>(FuncCallee.getCallee());
        std::vector<Value *> args;
        llvm::IRBuilder<> Builder(F.getContext());
        Builder.SetInsertPoint(BB.getFirstNonPHI());
        Builder.CreateCall(Func, args);
        containsSetup = true;
      }
      if (!containsShutdown) {
        auto FuncCallee =
            module->getOrInsertFunction(FuncNames["finish"], funcType);
        Function *Func = llvm::cast<Function>(FuncCallee.getCallee());
        std::vector<Value *> args;
        llvm::IRBuilder<> Builder(F.getContext());
        for (auto retInst : retInsts) {
          Builder.SetInsertPoint(retInst);
          Builder.CreateCall(Func, args);
        }
        containsSetup = true;
      }
    }
    if (!F.isDeclaration()) {
      SmallVector<Argument *> *Args = checkSecretShared->args.count(&F)
                                          ? checkSecretShared->args[&F]
                                          : nullptr;
      char output = -1;
      if (checkSecretShared->outputFuncs.find(&F) !=
          checkSecretShared->outputFuncs.end())
        output = checkSecretShared->outputFuncs[&F];
      if (!Args){
        errs() << "Function not found \n";
        F.printAsOperand(errs());
        continue;
      }
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
      replaceFunc(F, *Args, output);
    }
  }
  return PreservedAnalyses::all();
}
