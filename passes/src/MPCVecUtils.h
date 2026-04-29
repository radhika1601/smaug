#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

namespace llvm {

class MPCVecUtils {
public:
  MPCVecUtils() {}

  static bool isPrivateLoadStore(Instruction *I) {
    Value *ptr = nullptr;
    if (isa<LoadInst>(I)) {
      LoadInst *ld = dyn_cast<LoadInst>(I);
      ptr = ld->getPointerOperand();
    } else if (isa<StoreInst>(I)) {
      StoreInst *store = dyn_cast<StoreInst>(I);
      ptr = store->getPointerOperand();
    } else if (isa<GetElementPtrInst>(I))
      ptr = I;
    if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ptr)) {
      if (Instruction *index = dyn_cast<Instruction>(gep->getOperand(1)))
        if (index->hasMetadata("secret_shared"))
          return true;
    }
    return false;
  }

  static void CreateFree(IRBuilder<> &Builder, Function *F, Value *ptr) {
    Type *VoidTy = Type::getVoidTy(F->getContext());
    Type *Int8PtrTy = Builder.getPtrTy();
    FunctionType *FreeTy = FunctionType::get(VoidTy, Int8PtrTy, false);
    FunctionCallee FreeFunc =
        F->getParent()->getOrInsertFunction("free", FreeTy);
    Builder.CreateCall(FreeFunc, ptr);
  }
  inline void replaceOperand(Instruction *I, Instruction *oldOp,
                             Instruction *newOp) {

    llvm::Use *ops = I->getOperandList();
    int num_ops = I->getNumOperands();
    for (int i = 0; i < num_ops; ++i) {
      if (Instruction *op = dyn_cast<Instruction>(ops[i])) {
        if (op == oldOp) {
          I->setOperand(i, newOp);
          break;
        }
      }
    }
  }
  static Instruction *createMalloc(IRBuilder<> &Builder, Function *F,
                                   Type *type, Value *n,
                                   bool isSecShared = false,
                                   std::string name = "") {
    llvm::Function *mallocFunc = cast<llvm::Function>(
        F->getParent()
            ->getOrInsertFunction("malloc",
                                  FunctionType::get(PointerType::get(type, 0),
                                                    n->getType(), false))
            .getCallee());
    uint64_t elementSize =
        F->getParent()->getDataLayout().getTypeAllocSize(type);
    auto size =
        Builder.CreateMul(n, ConstantInt::get(n->getType(), elementSize));
    size = Builder.CreateZExtOrBitCast(size, Builder.getInt64Ty());
    auto ptr = Builder.CreateCall(mallocFunc, size, name + ".ptr");
    Instruction *ptrInst = dyn_cast<Instruction>(ptr);
    if (isSecShared) {
      auto *MDStr = llvm::MDString::get(F->getContext(), "secret_shared");
      auto *node = MDNode::get(F->getContext(), MDStr);
      ptrInst->setMetadata("secret_shared", node);
    }
    // Attach element type metadata so downstream passes (VecMPCLink) can
    // determine the buffer type without user traversal.
    Type *scalarType = type->getScalarType();
    if (auto *intTy = dyn_cast<IntegerType>(scalarType)) {
      std::string typeName;
      switch (intTy->getBitWidth()) {
      case 1:  typeName = "int1";  break;
      case 8:  typeName = "int8";  break;
      case 16: typeName = "int16"; break;
      case 32: typeName = "int32"; break;
      case 64: typeName = "int64"; break;
      default: break;
      }
      if (!typeName.empty()) {
        auto *MDStr = llvm::MDString::get(F->getContext(), typeName);
        auto *mdNode = MDNode::get(F->getContext(), MDStr);
        ptrInst->setMetadata(typeName, mdNode);
      }
    }
    return ptrInst;
  }

  bool hasSecretSharedInsts(std::vector<BasicBlock *> blocks) {
    for (auto BB : blocks)
      for (auto &I : *BB)
        // Instruction should be vectorizable
        if (I.hasMetadata("secret_shared")) {
          switch (I.getOpcode()) {
          case Instruction::Call:
            if (CallInst *CI = dyn_cast<CallInst>(&I)) {
              auto name = CI->getCalledFunction()->getName().str();
              // errs() << name << "\n";
              if (name.find("llvm.") != std::string::npos) {
                return true;
              }
            }
            break;
          case Instruction::GetElementPtr:
          case Instruction::Load:
          case Instruction::Store:
            // load store and gep instrs do not create extra burden
            break;
          default:
            return true;
            break;
          }
        }

    return false;
  }

  bool isLoopVectorized(Loop *L) {
    // Check for the presence of vectorization metadata
    BasicBlock *Header = L->getHeader();
    for (auto &I : *Header) {
      if (auto *MD = I.getMetadata("llvm.loop")) {
        // Loop through all metadata attached to the instruction
        for (unsigned i = 0, e = MD->getNumOperands(); i != e; ++i) {
          MDNode *node = dyn_cast<MDNode>(MD->getOperand(i));

          for (unsigned j = 0, f = node->getNumOperands(); j != f; ++j) {
            if (MDString *MDStr = dyn_cast<MDString>(node->getOperand(j))) {
              if (MDStr->getString() == "llvm.loop.isvectorized") {
                return true; // Found vectorization metadata
              }
            }
          }
        }
      }
    }
    return false;
  }

  bool canVectorizeInst(Instruction *I) {
    if (!isa<CallInst>(I))
      return true;
    if (CallInst *callInst = dyn_cast<CallInst>(I)) {
      std::string name = callInst->getCalledFunction()->getName().str();
      if (name.find("llvm.") != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  bool hasSecretSharedInstrs(BasicBlock *BB) {
    for (auto &I : *BB) {
      if (I.hasMetadata("secret_shared")) {
        switch (I.getOpcode()) {
          // Update load, store in case of private index
        case Instruction::Load:
        case Instruction::Store:
        case Instruction::Add:
        case Instruction::And:
        case Instruction::AShr:
        case Instruction::FAdd:
        case Instruction::FCmp:
        case Instruction::FDiv:
        case Instruction::FMul:
        case Instruction::FNeg:
        case Instruction::FRem:
        case Instruction::FSub:
        case Instruction::ICmp:
        case Instruction::LShr:
        case Instruction::Mul:
        case Instruction::Or:
        case Instruction::Select:
        case Instruction::Shl:
        case Instruction::Sub:
        case Instruction::Xor:
          return true;
          //   case Instruction::FPToUI:
          //   case Instruction::FPToSI:
          //   case Instruction::FPExt:
          // case Instruction::PtrToInt:
          // case Instruction::IntToPtr:
          // case Instruction::SIToFP:
          // case Instruction::UIToFP:
          // case Instruction::Trunc:
          // case Instruction::FPTrunc:
          break;
        default:
          break;
        }
      }
    }

    return false;
  }

  void getlatchInsts(Loop *L, SmallDenseSet<const Instruction *> &finalInsts,
                     ScalarEvolution *SE, LoopInfo *LI) {
    auto *induction = L->getInductionVariable(*SE);
    auto *BackEdge = getLoopBackEdge(L, LI);
    auto *BackEdgeCond = L->getLatchCmpInst();
    auto *BackEdgeInst = dyn_cast<Instruction>(BackEdge);
    auto *indNext = induction->getIncomingValueForBlock(L->getLoopLatch());
    auto *indNextInst = dyn_cast<Instruction>(indNext);
    finalInsts.insert(indNextInst);
    finalInsts.insert(BackEdgeCond);
    finalInsts.insert(BackEdgeInst);
  }

  const llvm::BranchInst *getLoopBackEdge(llvm::Loop *L, llvm::LoopInfo *LI) {
    auto *Latch = L->getLoopLatch();
    if (!Latch)
      return nullptr;

    auto *Term = Latch->getTerminator();
    if (auto *BI = llvm::dyn_cast<llvm::BranchInst>(Term)) {
      if (BI->isConditional()) {
        return BI;
      }
    }
    return nullptr;
  }

  Value *getTripCount(Loop *L, ScalarEvolution *SE, BasicBlock *PH = nullptr) {

    PredicatedScalarEvolution PSE(*SE, *L);

    if (isa<SCEVCouldNotCompute>(PSE.getBackedgeTakenCount())) {
      LLVM_DEBUG(dbgs() << "cannot compute the trip count\n");
      LLVM_DEBUG(L->print(dbgs()));
      return nullptr;
    }

    // Get the backedge taken count, which represents the number of iterations
    const SCEV *BackedgeTakenCount = SE->getBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(BackedgeTakenCount))
      return nullptr;
    // To add 1, you create a constant SCEV of 1 and use SCEVAddExpr
    const SCEV *One = SE->getConstant(BackedgeTakenCount->getType(), 1);
    const SCEV *scevPlusOne = SE->getAddExpr(BackedgeTakenCount, One);
    // scevPlusOne->dump();
    Value *tripCount = nullptr;
    if (auto *Unknown = dyn_cast<SCEVUnknown>(scevPlusOne)) {
      tripCount = Unknown->getValue();
      // Now V holds the Value* that was wrapped by SCEVUnknown
    } else if (auto *ConstSCEV = dyn_cast<SCEVConstant>(scevPlusOne)) {
      tripCount = ConstSCEV->getValue();
    } else {
      if (!PH)
        PH = L->getLoopPreheader();
      if (!PH) {
        LLVM_DEBUG(dbgs() << "not preheader\n");
        return nullptr;
      }
      const DataLayout &DL = PH->getModule()->getDataLayout();
      SCEVExpander Exp(*SE, DL, "induction");
      Value *Res = Exp.expandCodeFor(scevPlusOne, scevPlusOne->getType(),
                                     PH->getFirstInsertionPt());
      // errs() << "tripcount expanded\n";
      // Res->dump();
      return Res;
    }
    return tripCount;
  }
};

} // namespace llvm
