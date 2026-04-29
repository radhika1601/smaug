
#include "llvm/IR/Type.h"
// #include "llvm/IR/Module.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "Metadata.h"

#include <fstream>
#include <map>
using namespace llvm;

void SecSharedMetadataPass::setPtrSecretShared(
    Instruction *v, LLVMContext &context,
    std::set<llvm::Function *> &PubOutputs) {

  setSecretShared(v, context, PubOutputs);
  if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(v)) {
    auto ptr = gep->getPointerOperand();
    if (Instruction *I = dyn_cast<Instruction>(ptr)) {
      if (!I->hasMetadata("secret_shared")) {
        setPtrSecretShared(I, context, PubOutputs);
      }
    }
  }

  if (PHINode *phi = dyn_cast<PHINode>(v)) {
    for (uint i = 0; i < phi->getNumIncomingValues(); ++i) {
      if (Instruction *I = dyn_cast<Instruction>(phi->getIncomingValue(i))) {
        if (!I->hasMetadata("secret_shared")) {
          setPtrSecretShared(I, context, PubOutputs);
        }
      }
    }
  }
}

cl::opt<std::string> MetadataFilePath("metadata-path",
                                          cl::desc("Specify input filepath"),
                                          cl::value_desc("filepath"));

void SecSharedMetadataPass::setSecretShared(
    Instruction *v, LLVMContext &context,
    std::set<llvm::Function *> &PubOutputs) {
  auto *MDStr = llvm::MDString::get(context, "secret_shared");
  auto *node = MDNode::get(context, MDStr);
  v->setMetadata("secret_shared", node);

  for (auto itr = v->user_begin(); itr != v->user_end(); ++itr) {
    if (Instruction *user_inst = dyn_cast<Instruction>(*itr)) {
      if (auto *CI = dyn_cast<CallInst>(user_inst)) {
        if (PubOutputs.find(CI->getCalledFunction()) != PubOutputs.end()) {
          continue;
        }
      }
      if (!user_inst->hasMetadata("secret_shared")) {
        setSecretShared(user_inst, context, PubOutputs);
      }
    }
  }
  if (v->getOpcode() == Instruction::Store) {
    auto ptr = v->getOperand(1);
    if (Instruction *I = dyn_cast<Instruction>(ptr)) {
      if(!I->hasMetadata("secret_shared"))
        setPtrSecretShared(I, context, PubOutputs);
    }
  }
}

void SecSharedMetadataPass::removeMetadata(Function &F) {
  IRBuilder<> Builder(F.getContext());
  std::set<Instruction *> deleteInstrs;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (CallInst *ci = dyn_cast<CallInst>(&I)) {
        std::string name = ci->getCalledFunction()->getName().str();
        if (name.find("malloc") != std::string::npos)
          continue;
        if (name.find("calloc") != std::string::npos)
          continue;
        if(name.find("_ZN3MPC9create") != std::string::npos)
          continue;
        if (name.find("llvm.fmuladd") != std::string::npos) {
          Builder.SetInsertPoint(&I);
          Value *mul =
              Builder.CreateFMul(ci->getArgOperand(0), ci->getArgOperand(1));
          Value *add = Builder.CreateFAdd(ci->getArgOperand(2), mul);
          I.replaceAllUsesWith(add);
          deleteInstrs.insert(&I);
        }
      }
      if (isa<AllocaInst>(I))
        continue;
      if (I.hasMetadata("secret_shared")) {
        I.setMetadata("secret_shared", nullptr);
      }
    }
  }
  for (auto I : deleteInstrs) {
    I->eraseFromParent();
  }
}

void SecSharedMetadataPass::runImpl(Function &F, SmallVector<Argument *> &Args,
                                    std::set<llvm::Function *> &PubOutputs,
                                    std::set<llvm::Function *> &PrivOutputs) {
  llvm::SmallVector<Instruction *> secret_shared;
  for (Argument *arg : Args) {
    for (auto *User : arg->users()) {
      if (Instruction *I = dyn_cast<Instruction>(User)) {
        if (!I->hasMetadata("secret_shared")) {
          if (auto *CI = dyn_cast<CallInst>(I)) {
            if (PubOutputs.find(CI->getCalledFunction()) != PubOutputs.end()) {
              continue;
            }
          }
          setSecretShared(I, F.getContext(), PubOutputs);
        }
      }
    }
  }

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (I.hasMetadata("secret_shared")) {
        if (CallInst *ci = dyn_cast<CallInst>(&I)) {
          auto name = ci->getCalledFunction()->getName();
          if (name.contains("malloc") || name.contains("_ZN3MPC9create"))
            setSecretShared(&I, F.getContext(), PubOutputs);
        }
        if (isa<AllocaInst>(I)) {
          setSecretShared(&I, F.getContext(), PubOutputs);
        }
      }
    }
  }

  for (Function *Func : PrivOutputs) {
    for (auto *U : Func->users()) {
      if (auto *CI = dyn_cast<CallInst>(U)) {
        setSecretShared(CI, F.getContext(), PubOutputs);
      }
    }
  }
}

llvm::PreservedAnalyses SecSharedMetadataPass::run(Module &M,
                                                   ModuleAnalysisManager &MAM) {

  StringRef moduleName = sys::path::filename(M.getName());
  std::string filePath = formatv("{0}", MetadataFilePath, moduleName);

  // Reading the file into a buffer
  ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr =
      MemoryBuffer::getFile(filePath);
  if (std::error_code EC = fileOrErr.getError()) {
    errs() << "Error reading file: " << EC.message() << "\t" << filePath
           << "\n";
    return PreservedAnalyses::all();
  }
  auto &buffer = *fileOrErr.get();

  // Parsing JSON from the buffer
  Expected<json::Value> parsed = json::parse(buffer.getBuffer());
  if (!parsed) {
    errs() << "Failed to parse JSON: " << toString(parsed.takeError()) << "\n";
    return PreservedAnalyses::all();
  }

  // Use the parsed JSON object
  json::Object *obj = parsed->getAsObject();
  std::set<llvm::Function *> PubOutputs, PrivOutputs;
  for (auto itr = obj->begin(); itr != obj->end(); ++itr) {
    auto val = obj->getObject(itr->getFirst());
    Function *func = M.getFunction(itr->getFirst());
    if (func) {
      if (val->getInteger("output") == 0)
        PubOutputs.insert(func);
      else
        PrivOutputs.insert(func);
    }
  }

  // errs() << PrivOutputs.size() << "\n";

  for (auto itr = obj->begin(); itr != obj->end(); ++itr) {
    Function *F = M.getFunction(itr->getFirst());
    if (F == nullptr) {
      // errs() << itr->getFirst() << "\n";
      continue;
    }
    SmallVector<Argument *> Args;
    json::Object *Fobj = obj->getObject(F->getName());
    if (Fobj) {
      json::Array *inputs = Fobj->getArray("input");
      if (inputs->size() != 0) {
        if (inputs->begin()->getAsArray() != nullptr) {
          for (auto itr = inputs->begin(); itr != inputs->end(); ++itr) {
            json::Array *arr = itr->getAsArray();
            if (arr->size() == F->arg_size()) {
              inputs = arr;
              break;
            }
          }
        }
        for (size_t i = 0; i < inputs->size(); ++i) {
          if ((inputs->begin() + i)->getAsInteger() == 1) {
            Args.push_back(F->getArg(i));
          }
        }
      }
    }
    removeMetadata(*F);
    runImpl(*F, Args, PubOutputs, PrivOutputs);
    Args.clear();
  }

  return llvm::PreservedAnalyses::all();
}
