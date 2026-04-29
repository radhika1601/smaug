#include "MPCRemoveOps.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

extern cl::opt<std::string> MetadataFilePath;

#define DEBUG_TYPE "mpc-remove-ops"

void MPCRemoveOpsPass::setSecretShared(Value *val) {

  Instruction *I = dyn_cast<Instruction>(val);
  if (!I)
    return;
  Function *F = I->getParent()->getParent();
  auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
  auto *node = MDNode::get(F->getContext(), MDStr);
  I->setMetadata("secret_shared", node);
}

bool MPCRemoveOpsPass::isSecretShared(Value *val,
                                      SmallVector<Argument *> &Args) {
  if (Instruction *I = dyn_cast<Instruction>(val)) {
    if (I->hasMetadata("secret_shared"))
      return true;
  }
  if (std::find(Args.begin(), Args.end(), val) != Args.end())
    return true;

  // TODO: check args
  return false;
}

bool MPCRemoveOpsPass::updateIcmp(Instruction *I) {
  if (ICmpInst *cmpInst = dyn_cast<ICmpInst>(I)) {

    Function *F = I->getParent()->getParent();
    llvm::IRBuilder<> Builder(F->getContext());
    Builder.SetInsertPoint(I);

    Value *in1 = I->getOperand(0), *in2 = I->getOperand(1);

    // check if pattern leads to truncation
    Value *trunc = nullptr;

    if (Instruction *in1Inst = dyn_cast<Instruction>(in1))
      if (Constant *c = dyn_cast<Constant>(in2))
        if (c->isZeroValue() && in1Inst->getOpcode() == Instruction::And) {
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(0))) {
            if (c2->isOneValue())
              trunc = in1Inst->getOperand(1);
          }
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(1))) {
            if (c2->isOneValue())
              trunc = in1Inst->getOperand(0);
          }
        }

    if (Instruction *in1Inst = dyn_cast<Instruction>(in2))
      if (Constant *c = dyn_cast<Constant>(in1))
        if (c->isZeroValue() && in1Inst->getOpcode() == Instruction::And) {
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(0))) {
            if (c2->isOneValue())
              trunc = in1Inst->getOperand(1);
          }
          if (Constant *c2 = dyn_cast<Constant>(in1Inst->getOperand(1))) {
            if (c2->isOneValue())
              trunc = in1Inst->getOperand(0);
          }
        }

    if (trunc && cmpInst->isEquality()) {
      Value *val = Builder.CreateTrunc(trunc, Builder.getInt1Ty());
      setSecretShared(val);
      if (cmpInst->getPredicate() == ICmpInst::ICMP_EQ) {
        // equals to zero is NOT of the 1 bit value
        val = Builder.CreateNot(val);
        setSecretShared(val);
      }
      I->replaceAllUsesWith(val);
      return true;
    }

    Value *newVal = nullptr;
    bool isI1 = (I->getOperand(0)->getType() == Builder.getInt1Ty());
    if (isI1) {
      if (cmpInst->isEquality()) {
        newVal = Builder.CreateXor(I->getOperand(0), I->getOperand(1));
        newVal = Builder.CreateNot(newVal);
      }
    }
    if (cmpInst->getPredicate() == ICmpInst::ICMP_SLE) {
      newVal =
          Builder.CreateICmpSGE(cmpInst->getOperand(1), cmpInst->getOperand(0));
    } else if (cmpInst->getPredicate() == ICmpInst::ICMP_ULE) {
      newVal =
          Builder.CreateICmpUGE(cmpInst->getOperand(1), cmpInst->getOperand(0));
    } else if (cmpInst->getPredicate() == ICmpInst::ICMP_SLT) {
      newVal =
          Builder.CreateICmpSGT(cmpInst->getOperand(1), cmpInst->getOperand(0));
    } else if (cmpInst->getPredicate() == ICmpInst::ICMP_ULT) {
      newVal =
          Builder.CreateICmpUGT(cmpInst->getOperand(1), cmpInst->getOperand(0));
    } else if (cmpInst->getPredicate() == ICmpInst::ICMP_NE) {
      if (!isI1) {
        newVal = Builder.CreateICmpEQ(cmpInst->getOperand(0),
                                      cmpInst->getOperand(1));
        setSecretShared(newVal);
        newVal = Builder.CreateNot(newVal);
      } else
        newVal = Builder.CreateXor(I->getOperand(0), I->getOperand(1));
    }

    if (!newVal)
      return false;

    setSecretShared(newVal);
    I->replaceAllUsesWith(newVal);
    return true;
  }

  return false;
}

bool MPCRemoveOpsPass::updateOr(Instruction *I) {
  if (I->getOpcode() != Instruction::Or)
    return false;

  Function *F = I->getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(I);

  auto val1 = Builder.CreateNot(I->getOperand(0));
  auto val2 = Builder.CreateNot(I->getOperand(1));

  setSecretShared(val1);
  setSecretShared(val2);

  auto val3 = Builder.CreateAnd(val1, val2);
  setSecretShared(val3);

  val3 = Builder.CreateNot(val3);
  setSecretShared(val3);
  I->replaceAllUsesWith(val3);
  return true;
}

bool MPCRemoveOpsPass::updateSMax(Instruction *I) {

  Function *F = I->getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(I);
  auto *cmp = Builder.CreateICmpSGT(I->getOperand(0), I->getOperand(1));
  setSecretShared(cmp);

  auto *val =
      this->createSelect(cmp, I->getOperand(0), I->getOperand(1), Builder);
  I->replaceAllUsesWith(val);

  return true;
}

bool MPCRemoveOpsPass::updateSMin(Instruction *I) {

  Function *F = I->getParent()->getParent();
  llvm::IRBuilder<> Builder(F->getContext());
  Builder.SetInsertPoint(I);

  auto *cmp = Builder.CreateICmpSGT(I->getOperand(0), I->getOperand(1));
  setSecretShared(cmp);

  auto *val =
      this->createSelect(cmp, I->getOperand(1), I->getOperand(0), Builder);
  I->replaceAllUsesWith(val);

  return true;
}

Value *MPCRemoveOpsPass::createSelect(Value *condition, Value *trueValue,
                                      Value *falseValue, IRBuilder<> &Builder) {
  if (trueValue->getType()->isIntOrIntVectorTy() ||
      trueValue->getType()->isFloatTy()) {
    Value *v1 = nullptr;
    bool isFalse = false;
    if (Constant *c = dyn_cast<Constant>(falseValue)) {
      if (c->isZeroValue())
        isFalse = true;
    }
    if (isFalse) {
      v1 = trueValue;
      isFalse = false;
    } else if (Constant *c = dyn_cast<Constant>(trueValue)) {
      if (c->isZeroValue())
        isFalse = true;
    }
    if (isFalse) {
      v1 = falseValue;
      isFalse = false;
    } else {
      if (trueValue->getType()->isFloatTy()) {
        auto t = Builder.CreateBitCast(trueValue, Builder.getInt32Ty());
        auto f = Builder.CreateBitCast(falseValue, Builder.getInt32Ty());
        setSecretShared(t);
        setSecretShared(f);
        v1 = Builder.CreateXor(t, f);
        setSecretShared(v1);
        v1 = Builder.CreateBitCast(v1, Builder.getFloatTy());
      } else
        v1 = Builder.CreateXor(trueValue, falseValue);
      setSecretShared(v1);
    }
    if (v1->getType() != condition->getType()) {
      condition = Builder.CreateSExtOrBitCast(
          condition,
          v1->getType()->isFloatTy() ? Builder.getInt32Ty() : v1->getType());
      setSecretShared(condition);
    }
    if (trueValue->getType()->isFloatTy()) {
      auto t = Builder.CreateBitCast(v1, Builder.getInt32Ty());
      auto f = Builder.CreateBitCast(condition, Builder.getInt32Ty());
      setSecretShared(t);
      setSecretShared(f);
      v1 = Builder.CreateAnd(t, f);
      setSecretShared(v1);
      v1 = Builder.CreateBitCast(v1, Builder.getFloatTy());
    } else
      v1 = Builder.CreateAnd(v1, condition);
    setSecretShared(v1);

    if (Constant *c = dyn_cast<Constant>(falseValue)) {
      if (c->isZeroValue())
        isFalse = true;
    }

    if (!isFalse) {
      if (trueValue->getType()->isFloatTy()) {
        auto t = Builder.CreateBitCast(v1, Builder.getInt32Ty());
        auto f = Builder.CreateBitCast(falseValue, Builder.getInt32Ty());
        setSecretShared(t);
        setSecretShared(f);
        v1 = Builder.CreateXor(t, f);
        setSecretShared(v1);
        v1 = Builder.CreateBitCast(v1, Builder.getFloatTy());
        setSecretShared(v1);
      } else
        v1 = Builder.CreateXor(v1, falseValue);
      setSecretShared(v1);
    }
    return v1;
  }
  if (trueValue->getType()->isPointerTy()) {
    LLVM_DEBUG(dbgs() << "select on pointer type\n");
  } else if (trueValue->getType()) {
    LLVM_DEBUG(dbgs() << "select on type " << *(trueValue->getType()) << "\n");
  }
  return nullptr;
}

bool MPCRemoveOpsPass::updateSelect(Instruction *I,
                                    SmallVector<Argument *> &Args) {

  if (SelectInst *selInst = dyn_cast<SelectInst>(I)) {
    auto condition = selInst->getCondition();
    if (!isSecretShared(condition, Args)) {
      return false;
    }
    Function *F = I->getParent()->getParent();
    llvm::IRBuilder<> Builder(F->getContext());
    Builder.SetInsertPoint(I);

    auto v1 = this->createSelect(condition, selInst->getTrueValue(),
                                 selInst->getFalseValue(), Builder);
    if (v1) {
      I->replaceAllUsesWith(v1);
      return true;
    }
  }

  return false;
}

llvm::PreservedAnalyses MPCRemoveOpsPass::run(Module &M,
                                              ModuleAnalysisManager &MAM) {
                                                errs() << "mpc remove ops\n";
  std::string filePath = formatv("{0}", MetadataFilePath);

  // Reading the file into a buffer
  ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr =
      MemoryBuffer::getFile(filePath);
  if (std::error_code EC = fileOrErr.getError()) {
    LLVM_DEBUG(dbgs() << "Error reading file: " << EC.message() << "\t"
                      << filePath << "\n");
    return PreservedAnalyses::all();
  }
  auto &buffer = *fileOrErr.get();

  // Parsing JSON from the buffer
  Expected<json::Value> parsed = json::parse(buffer.getBuffer());
  if (!parsed) {
    LLVM_DEBUG(dbgs() << "Failed to parse JSON: "
                      << toString(parsed.takeError()) << "\n");
    return PreservedAnalyses::all();
  }

  // Use the parsed JSON object
  json::Object *obj = parsed->getAsObject();

  for (Function &F : M) {
    if (!F.isDeclaration()) {
      SmallVector<Argument *> Args;
      json::Object *Fobj = obj->getObject(F.getName());
      if (Fobj) {
        json::Array *inputs = Fobj->getArray("input");
        for (size_t i = 0; i < inputs->size(); ++i) {
          if ((inputs->begin() + i)->getAsInteger() == 1) {
            Args.push_back(F.getArg(i));
          }
        }
      }
      for (BasicBlock &BB : F) {
        this->revertAndIcmpToTrunc(&BB);
        std::vector<Instruction *> removeInsts;
        for (Instruction &I : BB) {
          bool remove = false;
          if (I.hasMetadata("secret_shared")) {
            switch (I.getOpcode()) {
            case Instruction::Ret:
            case Instruction::PHI:
            case Instruction::Alloca:
            case Instruction::ZExt:
            case Instruction::SExt:
            case Instruction::GetElementPtr:
            case Instruction::Store:
            case Instruction::Load:
            case Instruction::Add:
            case Instruction::Mul:
            case Instruction::And:
            case Instruction::Xor:
            case Instruction::Sub:
            case Instruction::Trunc:
            case Instruction::SDiv:
            case Instruction::FAdd:
            case Instruction::FSub:
            case Instruction::FMul:
            case Instruction::FDiv:
            case Instruction::Shl:
              // Update Sub to neg, add
              break;
            case Instruction::Select:
              remove = updateSelect(&I, Args);
              break;
            case Instruction::FCmp:
              break;
            case Instruction::ICmp:
              remove = updateIcmp(&I);
              break;
            case Instruction::Or:
              remove = updateOr(&I);
              break;
            case Instruction::Call:
              if (CallInst *callInst = dyn_cast<CallInst>(&I)) {
                std::string name =
                    callInst->getCalledFunction()->getName().str();
                if (name.find("llvm.smax") != std::string::npos) {
                  remove = updateSMax(&I);
                  break;
                }
                if (name.find("llvm.smin") != std::string::npos) {
                  remove = updateSMin(&I);
                  break;
                } else if (name.find("llvm.memset") != std::string::npos) {
                  break;
                } else if (name.find("llvm.memcpy") != std::string::npos) {
                  break;
                } else if (name.find("llvm") != std::string::npos) {
                  LLVM_DEBUG(dbgs() << I << "\n");
                }
              }
              break;
            default:
              LLVM_DEBUG(dbgs() << I << "\n");
              break;
            }
          }

          if (remove) {
            removeInsts.push_back(&I);
          }
        }

        for (auto I : removeInsts) {
          I->eraseFromParent();
        }
      }
      Args.clear();
    }
  }
  return PreservedAnalyses::all();
}
