#pragma once
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <map>

using namespace llvm;
class MPCRemoveOpsPass : public llvm::PassInfoMixin<MPCRemoveOpsPass> {
private:
  bool updateIcmp(Instruction *I);
  bool updateOr(Instruction *I);
  bool updateSMax(Instruction *I);
  bool updateSMin(Instruction *I);
  bool updateSelect(Instruction *I, SmallVector<Argument *> &Args);

  Value *createSelect(Value *condition, Value *trueValue, Value *falseValue,
                      IRBuilder<> &Builder);
  void setSecretShared(Value *val);
  bool isSecretShared(Value *val, SmallVector<Argument *> &Args);

  inline void revertAndIcmpToTrunc(BasicBlock *BB) {
    SmallVector<Instruction *> erase;
    for (Instruction &I : *BB) {
      if (!I.hasMetadata("secret_shared"))
        continue;
      if (I.getOpcode() == Instruction::And) {
        Value *val = nullptr;
        if (I.getOperand(1) == ConstantInt::get(I.getType(), 1)) {
          val = I.getOperand(0);
        }
        if (I.getOperand(0) == ConstantInt::get(I.getType(), 1)) {
          val = I.getOperand(1);
        }

        if (val) {
          for (auto user : I.users()) {
            if (Instruction *userInst = dyn_cast<Instruction>(user)) {
              if (ICmpInst *icmp = dyn_cast<ICmpInst>(userInst)) {
                // b == 0 => !b
                // b != 0 => b
                if (icmp->isEquality()) {
                  Constant *c = dyn_cast<Constant>(icmp->getOperand(0));
                  if (!c)
                    c = dyn_cast<Constant>(icmp->getOperand(1));
                  if (!c)
                    continue;
                  
                  IRBuilder<> Builder(icmp);
                  Value *trunc = Builder.CreateTrunc(val, Builder.getInt1Ty());
                  setSecretShared(trunc);
                  if ((c->isOneValue() &&
                       icmp->getPredicate() == ICmpInst::ICMP_NE) ||
                      (c->isZeroValue() &&
                       icmp->getPredicate() == ICmpInst::ICMP_EQ)) {
                    trunc = Builder.CreateNot(trunc);
                    setSecretShared(trunc);
                  }
                  icmp->replaceAllUsesWith(trunc);
                  erase.push_back(icmp);
                  if (I.getNumUses() == 1) {
                    erase.push_back(&I);
                  }
                }
              }
            }
          }
        }
      }
    }

    for (auto I : erase) {
      I->eraseFromParent();
    }
  }

public:
  llvm::PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
};
