#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/FormatVariadic.h"

#include <map>
#include <set>

using namespace llvm;
class CheckSecretShared;

class MPCLinkPass : public llvm::PassInfoMixin<MPCLinkPass> {
private:
  bool gc = true;
  CheckSecretShared *checkSecretShared;

  std::map<Value *, Value *> PtrToMPCPtr;
  std::map<Value *, Value *> ItoMPCI;
  Value *i1Toi8(Value *v, IRBuilder<> &Builder, Type *boolTy);
  Value *i8Toi1(Value *v, IRBuilder<> &Builder, Type *i1Ty);
  Value *isAlice(IRBuilder<> &Builder, Module *module, Function *F);
  // std::map<std::pair<unsigned, Type>, Function *> ItoF;
  bool findFunc(Instruction &I, std::string name,
                SmallVector<Argument *> &Args);
  bool selectCall(Instruction &I);
  bool icmpCall(Instruction &I, SmallVector<Argument *> &Args);
  bool fcmpCall(Instruction &I, SmallVector<Argument *> &Args);
  void replaceFunc(Function &F, SmallVector<Argument *> &Args, char output);
  bool publicXor(Instruction &I, SmallVector<Argument *> &Args);
  bool updatePrivateStore(Instruction &I);
  bool updatePrivateLoad(Instruction &I, SmallVector<Argument *> &Args);
  bool isPrivateLoadStore(Instruction *Ld, SmallVector<Argument *> &Args);
  // void vectorizedLoop(Loop &L);
  void setSecretShared(Value *val);
  Value *makeSecretShared(Value *val, IRBuilder<> &Builder, Module *module,
                          Function *F) {
    if (gc) {
      std::string name = "init";
      if (val->getType() == Builder.getInt1Ty())
        name += "1";
      else if (val->getType() == Builder.getInt8Ty())
        name += "8";
      else if (val->getType() == Builder.getInt16Ty())
        name += "16";
      else if (val->getType() == Builder.getInt32Ty())
        name += "32";
      else if (val->getType() == Builder.getInt64Ty())
        name += "64";
      Function *func = cast<llvm::Function>(
          module
              ->getOrInsertFunction(GCFuncNames[name],
                                    FunctionType::get(Builder.getPtrTy(),
                                                      {val->getType()}, false))
              .getCallee());
      val = Builder.CreateCall(func, {val});
      return val;
    }
    Constant *c = dyn_cast<Constant>(val);
    if (c && c->isZeroValue())
      return val;
    auto v = isAlice(Builder, module, F);
    if (val->getType()->isIntegerTy()) {
      v = Builder.CreateSExt(v, val->getType());
      val = Builder.CreateAnd(val, v);
    } else if (val->getType()->isFloatTy()) {
      v = Builder.CreateSExt(v, Builder.getInt32Ty());
      val = Builder.CreateBitCast(val, Builder.getInt32Ty());
      val = Builder.CreateAnd(val, v);
      val = Builder.CreateBitCast(val, Builder.getFloatTy());
    } else if (val->getType()->isDoubleTy()) {
      v = Builder.CreateSExt(v, Builder.getInt64Ty());
      val = Builder.CreateBitCast(val, Builder.getInt64Ty());
      val = Builder.CreateAnd(val, v);
      val = Builder.CreateBitCast(val, Builder.getDoubleTy());
    }
    return val;
  }
  Instruction *I1Output = nullptr;
  Instruction *I8Output = nullptr;
  Instruction *I16Output = nullptr;
  Instruction *I32Output = nullptr;
  Instruction *I64Output = nullptr;
  Value *isAliceInst = nullptr;

  std::map<std::string, std::string> GCFuncNames = {
      {"init1", "_ZN3MPC6getBitEb"},
      {"init8", "_ZN3MPC10getIntegerEa"},
      {"init16", "_ZN3MPC10getIntegerEs"},
      {"init32", "_ZN3MPC10getIntegerEi"},
      {"init64", "_ZN3MPC10getIntegerEl"},
      {"createInt", "_ZN3MPC9createIntEl"},
      {"createBit", "_ZN3MPC9createBitEl"},
      {"loadInt", "_ZN3MPC4loadEPN3emp7IntegerE"},
      {"storeInt", "_ZN3MPC5storeEPN3emp7IntegerERS1_"},
      {"storeBit", "_ZN3MPC5storeEPN3emp3BitERS1_"},
      {"gepInt", "_ZN3MPC3gepEPN3emp7IntegerEl"},
      {"gepBit", "_ZN3MPC3gepEPN3emp3BitEl"},
      {"add8", "_ZN3MPC3addERN3emp7IntegerES2_"},
      {"add16", "_ZN3MPC3addERN3emp7IntegerES2_"},
      {"add32", "_ZN3MPC3addERN3emp7IntegerES2_"},
      {"add64", "_ZN3MPC3addERN3emp7IntegerES2_"},
      {"xor1", "_ZN3MPC6xorBitERN3emp3BitES2_"},
      {"xor8", "_ZN3MPC6xorIntERN3emp7IntegerES2_"},
      {"xor16", "_ZN3MPC6xorIntERN3emp7IntegerES2_"},
      {"xor32", "_ZN3MPC6xorIntERN3emp7IntegerES2_"},
      {"xor64", "_ZN3MPC6xorIntERN3emp7IntegerES2_"},
      {"and1", "_ZN3MPC6andBitERN3emp3BitES2_"},
      {"and8", "_ZN3MPC6andIntERN3emp7IntegerES2_"},
      {"and16", "_ZN3MPC6andIntERN3emp7IntegerES2_"},
      {"and32", "_ZN3MPC6andIntERN3emp7IntegerES2_"},
      {"and64", "_ZN3MPC6andIntERN3emp7IntegerES2_"},
      {"icmpEq8", "_ZN3MPC4icmpERN3emp7IntegerES2_i"},
      {"icmpEq16", "_ZN3MPC4icmpERN3emp7IntegerES2_i"},
      {"icmpEq32", "_ZN3MPC4icmpERN3emp7IntegerES2_i"},
      {"icmpEq64", "_ZN3MPC4icmpERN3emp7IntegerES2_i"},
      {"mul8", "_ZN3MPC4multERN3emp7IntegerES2_"},
      {"mul16", "_ZN3MPC4multERN3emp7IntegerES2_"},
      {"mul32", "_ZN3MPC4multERN3emp7IntegerES2_"},
      {"mul64", "_ZN3MPC4multERN3emp7IntegerES2_"},
      {"div8", "_ZN3MPC3divERN3emp7IntegerES2_"},
      {"div16", "_ZN3MPC3divERN3emp7IntegerES2_"},
      {"div32", "_ZN3MPC3divERN3emp7IntegerES2_"},
      {"div64", "_ZN3MPC3divERN3emp7IntegerES2_"},
      {"sub8", "_ZN3MPC3subERN3emp7IntegerES2_"},
      {"sub16", "_ZN3MPC3subERN3emp7IntegerES2_"},
      {"sub32", "_ZN3MPC3subERN3emp7IntegerES2_"},
      {"sub64", "_ZN3MPC3subERN3emp7IntegerES2_"},
      {"bittoint", "_ZN3MPC8bitToIntEPN3emp3BitEi"},
      {"revealBit", "_ZN3MPC6revealEPN3emp3BitE"},
      {"revealInt", "_ZN3MPC6revealEPN3emp7IntegerE"}
      // {},
      // {},
      // {},
  };

  std::map<std::string, std::string> FuncNames = {
      {"setup", "_ZN3MPC5setupEv"},
      {"finish", "_ZN3MPC6finishEv"},
      {"add8", "_ZN3MPC5addI8Eaa"},
      {"add16", "_ZN3MPC6addI16Ess"},
      {"add32", "_ZN3MPC6addI32Eii"},
      {"add64", "_ZN3MPC6addI64Ell"},
      {"and1", "_ZN3MPC7andBoolEbb"},
      {"and8", "_ZN3MPC5andI8Eaa"},
      {"and16", "_ZN3MPC6andI16Ess"},
      {"and32", "_ZN3MPC6andI32Eii"},
      {"and64", "_ZN3MPC6andI64Ell"},
      {"icmpEq8", "_ZN3MPC8icmpEqI8Eaai"},
      {"icmpEq16", "_ZN3MPC9icmpEqI16Essi"},
      {"icmpEq32", "_ZN3MPC9icmpEqI32Eiii"},
      {"icmpEq64", "_ZN3MPC9icmpEqI64Elli"},
      {"mul8", "_ZN3MPC6multI8Eaa"},
      {"mul16", "_ZN3MPC7multI16Ess"},
      {"mul32", "_ZN3MPC7multI32Eii"},
      {"mul64", "_ZN3MPC7multI64Ell"},
      {"div8", "_ZN3MPC5divI8Eaa"},
      {"div16", "_ZN3MPC6divI16Ess"},
      {"div32", "_ZN3MPC6divI32Eii"},
      {"div64", "_ZN3MPC6divI64Ell"},
      {"sub8", "_ZN3MPC5subI8Eaa"},
      {"sub16", "_ZN3MPC6subI16Ess"},
      {"sub32", "_ZN3MPC6subI32Eii"},
      {"sub64", "_ZN3MPC6subI64Ell"},
      {"load", "_ZN3MPC4loadEPviS0_iib"},
      {"store", "_ZN3MPC5storeEPvS0_iiib"},
      {"fadd", "_ZN3MPC4addFEff"},
      {"fsub", "_ZN3MPC4subFEff"},
      {"fmul", "_ZN3MPC5multFEff"},
      {"fdiv", "_ZN3MPC4divFEff"},
      {"fcmp", "_ZN3MPC6fcmpEqEffi"},
      {"revI1", "_ZN3MPC6revealIbEET_S1_i"},
      {"revI8", "_ZN3MPC6revealIaEET_S1_i"},
      {"revI16", "_ZN3MPC6revealIsEET_S1_i"},
      {"revI32", "_ZN3MPC6revealIiEET_S1_i"},
      {"revI64", "_ZN3MPC6revealIxEET_S1_i"}};

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

  bool isSecretShared(Value *val, SmallVector<Argument *> &Args);
  void updatePhi(Instruction *, SmallVector<Argument *> &Args);
  void revealOutput(Instruction &I);

  uint64_t getElemSizeFromMPCPtr(Value *MPCPtr) {
    CallInst *ci = dyn_cast<CallInst>(MPCPtr);
    while (ci) {
      StringRef fname = ci->getCalledFunction()->getName();
      if (fname.contains("getIntPtrIi") || fname.contains("getIntPtrIj"))
        return 4;
      if (fname.contains("getIntPtrIl") || fname.contains("getIntPtrIm"))
        return 8;
      if (fname.contains("getIntPtrIs") || fname.contains("getIntPtrIt"))
        return 2;
      if (fname.contains("getIntPtrIa") || fname.contains("getIntPtrIh"))
        return 1;
      if (fname.contains("gepInt") || fname.contains("gepBit"))
        ci = dyn_cast<CallInst>(ci->getArgOperand(0));
      else
        break;
    }
    return 0;
  }

  void updatePtrUses(Value *oldPtr, Value *MPCPtr, IRBuilder<> &Builder,
                     std::vector<Instruction *> &removeInsts,
                     Type *vType = nullptr) {
    // errs() << formatv("oldptr {0} newptr {1}\n", *oldPtr, *MPCPtr);
    Instruction *MPCPtrInst = dyn_cast<Instruction>(MPCPtr);
    Function *F = MPCPtrInst->getParent()->getParent();
    Instruction *oldInst = dyn_cast<Instruction>(oldPtr);
    if (oldInst)
      removeInsts.push_back(oldInst);

    std::set<User *> users;
    for (User *U : oldPtr->users())
      users.insert(U);

    for (User *U : users) {
      // errs() << formatv("\t{0}\n", *U);
      if (CallInst *CI = dyn_cast<CallInst>(U)) {
        auto name = CI->getCalledFunction()->getName();
        if (name.contains("free"))
          removeInsts.push_back(CI);
        else if (name.contains("getIntPtr") || name.contains("getBitPtr") ||
                 name.contains("writeArg"))
          continue;
        else if (name.contains("ZN3MPC")) {
          for (size_t i = 0; i < CI->getNumOperands(); ++i)
            if (CI->getOperand(i) == oldPtr)
              CI->setOperand(i, MPCPtr);
        } else
          errs() << *oldPtr << " " << *CI << "\n";
      } else if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(U)) {
        auto idx = gep->getOperand(1);
        auto gepType = gep->getSourceElementType();
        Instruction *tmp = gep;
        while (!vType && (tmp->getNumUses() != 0)) {
          User *user1 = nullptr;
          for (User *gepUser : tmp->users()) {
            user1 = gepUser;
            if (isa<LoadInst>(gepUser))
              vType = gepUser->getType();
            else if (isa<StoreInst>(gepUser))
              vType = gepUser->getOperand(0)->getType();
            if (vType)
              break;
          }
          tmp = dyn_cast<Instruction>(user1);
        }
        if (vType) {
          if (vType != gepType) {
            // if types are not same then we divide by (value type
            // size / source type size)
            uint64_t vSize =
                F->getParent()->getDataLayout().getTypeAllocSize(vType);
            uint64_t GEPSize =
                F->getParent()->getDataLayout().getTypeAllocSize(gepType);
            if (GEPSize < vSize) {
              uint64_t div = vSize / GEPSize;
              Builder.SetInsertPoint(gep);
              idx = Builder.CreateSDiv(idx,
                                       ConstantInt::get(idx->getType(), div));
            } else {
              errs() << "something is incorrect gep size > vsize\n";
            }
          }
        } else {
          if (gep->getNumUses() == 0)
            continue;
          uint64_t elemBytes = getElemSizeFromMPCPtr(MPCPtr);
          uint64_t GEPSize =
              F->getParent()->getDataLayout().getTypeAllocSize(gepType);
          if (elemBytes > GEPSize) {
            Builder.SetInsertPoint(gep);
            idx = Builder.CreateSDiv(
                idx, ConstantInt::get(idx->getType(), elemBytes / GEPSize));
          }
        }
        std::string name = "gepInt";
        if (vType == Builder.getInt8Ty() || vType == Builder.getInt1Ty())
          name = "gepBit";
        Builder.SetInsertPoint(gep);
        Function *gepFunc = cast<llvm::Function>(
            F->getParent()
                ->getOrInsertFunction(GCFuncNames[name],
                                      FunctionType::get(Builder.getPtrTy(),
                                                        {Builder.getPtrTy(),
                                                         Builder.getInt64Ty()},
                                                        false))
                .getCallee());
        auto newGEP = Builder.CreateCall(gepFunc, {MPCPtr, idx});
        PtrToMPCPtr.insert(std::make_pair(gep, newGEP));
        newGEP->copyMetadata(*gep);
        updatePtrUses(gep, newGEP, Builder, removeInsts, vType);
      } else if (LoadInst *ld = dyn_cast<LoadInst>(U)) {
        ItoMPCI.insert(std::make_pair(ld, MPCPtr));
        removeInsts.push_back(ld);
      } else if (isa<StoreInst>(U)) {
        continue;
      } else if (PHINode *phi = dyn_cast<PHINode>(U)) {
        // Also update the old phi to ensure smooth deletion later
        if (PtrToMPCPtr.find(phi) != PtrToMPCPtr.end()) {
          PHINode *newPHI = dyn_cast<PHINode>(PtrToMPCPtr[phi]);
          for (size_t i = 0; i < phi->getNumIncomingValues(); i++) {
            Value *iv = phi->getIncomingValue(i);
            if (iv == oldPtr) {
              newPHI->addIncoming(MPCPtr, phi->getIncomingBlock(i));
            }
          }
        } else {
          Builder.SetInsertPoint(phi);
          PHINode *newPhi = Builder.CreatePHI(Builder.getPtrTy(),
                                              phi->getNumIncomingValues());
          newPhi->copyMetadata(*phi);
          for (size_t i = 0; i < phi->getNumIncomingValues(); i++) {
            Value *iv = phi->getIncomingValue(i);
            if (iv == oldPtr) {
              newPhi->addIncoming(MPCPtr, phi->getIncomingBlock(i));
            }
          }
          errs() << *phi << " " << *newPhi << "\n";
          PtrToMPCPtr.insert(std::make_pair(phi, newPhi));
          updatePtrUses(phi, newPhi, Builder, removeInsts, vType);
        }
      } else {
        // errs() << oldPtr->getName() << ": " << *U << "\n";
      }
    }
  }

  Type *getTypefromMetadata(Instruction *I, IRBuilder<> &Builder) {
    if (I->hasMetadata("int8"))
      return Builder.getInt8Ty();
    if (I->hasMetadata("int16"))
      return Builder.getInt16Ty();
    if (I->hasMetadata("int32"))
      return Builder.getInt32Ty();
    if (I->hasMetadata("int64"))
      return Builder.getInt64Ty();
    if (I->hasMetadata("float"))
      return Builder.getFloatTy();
    if (I->hasMetadata("double"))
      return Builder.getDoubleTy();
    return nullptr;
  }

public:
  llvm::PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
