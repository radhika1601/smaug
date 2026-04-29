#pragma once
#include "map"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "Vectorize/LoopVectorizationLegality.h"

using namespace llvm;

class VectorizeHelperPass : public llvm::PassInfoMixin<VectorizeHelperPass> {
private:
  ScalarEvolution *SE;
  LoopInfo *LI;
  TargetTransformInfo *TTI;
  DominatorTree *DT;
  BlockFrequencyInfo *BFI;
  TargetLibraryInfo *TLI;
  DemandedBits *DB;
  AssumptionCache *AC;
  LoopAccessInfoManager *LAIs;
  OptimizationRemarkEmitter *ORE;
  ProfileSummaryInfo *PSI;
  // bool isLoopVectorized(Loop *L);
  bool runImpl(Function &F, ScalarEvolution &SE_, LoopInfo &LI_,
               TargetTransformInfo &TTI_, DominatorTree &DT_,
               BlockFrequencyInfo *BFI_, TargetLibraryInfo *TLI_,
               DemandedBits &DB_, AssumptionCache &AC_,
               LoopAccessInfoManager &LAIs_, OptimizationRemarkEmitter &ORE_,
               ProfileSummaryInfo *PSI_);
  bool processLoop(Loop *L);

  // static bool canVectorizeInst(Instruction *I);

  void toLift(SmallVector<Instruction *> &isolate,
              SmallDenseSet<Instruction *> &deps,
              SmallVector<Instruction *> &lift,
              SmallVector<Instruction *> &toDuplicate, Function *F);

  inline void revertAndIcmpToTrunc(BasicBlock *BB) {
    SmallVector<Instruction *> IcmpInsts;
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
                if (icmp->getPredicate() == ICmpInst::ICMP_NE &&
                    (icmp->getOperand(1) == ConstantInt::get(I.getType(), 0) ||
                     icmp->getOperand(0) == ConstantInt::get(I.getType(), 0))) {
                  IRBuilder<> Builder(icmp);
                  Value *trunc = Builder.CreateTrunc(val, Builder.getInt1Ty());
                  icmp->replaceAllUsesWith(trunc);
                  IcmpInsts.push_back(icmp);
                }
              }
            }
          }
        }
      }
    }

    for (auto I : IcmpInsts) {
      I->eraseFromParent();
    }
  }

  void updateNewInstOps(Instruction *newInst,
                        std::map<Instruction *, Instruction *> instrsMap,
                        SmallDenseMap<Instruction *, Instruction *> &liftedMap,
                        IRBuilder<> &Builder, PHINode *loadStoreIndex,
                        Instruction *counter, Instruction *incrementedCounter,
                        BasicBlock *header, BasicBlock *insertAfter) {
    for (size_t k = 0; k < newInst->getNumOperands(); ++k) {
      if (Instruction *opInst = dyn_cast<Instruction>(newInst->getOperand(k))) {
        if (instrsMap.find(opInst) != instrsMap.end()) {
          newInst->setOperand(k, instrsMap[opInst]);
          continue;
        }
        if (liftedMap.find(opInst) != liftedMap.end()) {
          if (loadStoreIndex == nullptr) {
            loadStoreIndex = PHINode::Create(Builder.getInt64Ty(), 2, "");
            loadStoreIndex->insertAfter(counter);
            loadStoreIndex->addIncoming(Builder.getInt64(0), insertAfter);
            Builder.SetInsertPoint(header->getFirstNonPHI());
            auto tmp = Builder.CreateAdd(loadStoreIndex, Builder.getInt64(1));
            loadStoreIndex->addIncoming(tmp, header);
          }
          bool isI1 = opInst->getType() == Builder.getInt1Ty();
          auto type = isI1 ? Builder.getInt8Ty() : opInst->getType();
          Value *ptr;
          if(header->getFirstNonPHI())
            Builder.SetInsertPoint(header->getFirstNonPHI());
          else
            Builder.SetInsertPoint(header);
          ptr =
              Builder.CreateGEP(type, liftedMap.at(opInst), loadStoreIndex, "");

          Instruction *load = Builder.CreateLoad(type, ptr, "");
          Instruction *toUse = load;
          if (isI1) {
            auto tmp = Builder.CreateTruncOrBitCast(load, Builder.getInt1Ty());
            toUse = dyn_cast<Instruction>(tmp);
          }
          instrsMap.insert({opInst, toUse});
          newInst->setOperand(k, toUse);
        }
      }
    }
  }

  void getVectorizableInstr(Loop *L, SmallVector<Instruction *> &isolate,
                            SmallDenseSet<Instruction *> &deps,
                            SmallDenseSet<const Instruction *> &finalInsts,
                            ScalarEvolution *SE, LoopInfo *LI);
  // void getlatchInsts(Loop *L, SmallDenseSet<const Instruction *> &finalInsts,
  //                    ScalarEvolution *SE, LoopInfo *LI);

  bool willBeReplaced(Instruction *I) {
    if (I->hasMetadata("secret_shared")) {
      switch (I->getOpcode()) {
      case Instruction::GetElementPtr:
      case Instruction::ZExt:
      case Instruction::SExt:
      case Instruction::Ret:
      case Instruction::Trunc:
      case Instruction::Load:
      case Instruction::Store:
      case Instruction::Xor:
      case Instruction::Freeze:
      case Instruction::PHI:
        break;
      case Instruction::Select: {
        Instruction *cond = dyn_cast<Instruction>(I->getOperand(0));
        if (cond && cond->hasMetadata("secret_shared"))
          return true;
        break;
      }
      case Instruction::ICmp: // check if trunc
      case Instruction::Add:
      case Instruction::Sub:
      case Instruction::Mul:
      case Instruction::Or:
      case Instruction::FMul:
      case Instruction::FAdd:
      case Instruction::FSub:
      case Instruction::FCmp:
        return true;
      case Instruction::Call:
        if (CallInst *callInst = dyn_cast<CallInst>(I)) {
          std::string name = callInst->getCalledFunction()->getName().str();
          if ((name.find("llvm.smax") != std::string::npos) ||
              (name.find("llvm.smin") != std::string::npos)) {
            return true;
          } else if (name.find("llvm") != std::string::npos) {
            errs() << *I << "\n";
          }
        }
        break;
      case Instruction::And:
        if (Instruction *i1 = dyn_cast<Instruction>(I->getOperand(0))) {
          if (Instruction *i2 = dyn_cast<Instruction>(I->getOperand(1))) {
            if (i1->hasMetadata("secret_shared") &&
                i2->hasMetadata("secret_shared"))
              return true;
          }
        }
        break;
      default:
        errs() << "couldn't figure " << *I << "\n";
        break;
      }
    }
    return false;
  }

  BasicBlock *createLoop(IRBuilder<> &Builder, BasicBlock *insertBefore,
                         BasicBlock *insertAfter, BasicBlock *exit,
                         SmallVector<Instruction *> &instrs,
                         SmallDenseMap<Instruction *, Instruction *> &liftedMap,
                         PHINode *induction, Instruction *indNextInst,
                         Instruction *BackEdgeCond, bool ifTrueContinue, bool indCanBeIndex);

  inline void dependsOn(Instruction *I, SmallDenseSet<Instruction *> &part,
                        SmallDenseSet<Instruction *> &operandDeps,
                        SmallDenseSet<Instruction *> &liftSet,
                        Instruction *induction) {
    for (auto &op : I->operands()) {
      if (Instruction *opInst = dyn_cast<Instruction>(&op)) {
        if (opInst->getParent() != I->getParent())
          continue;
        if (opInst == induction)
          continue;
        if (part.contains(opInst))
          continue;
        if (willBeReplaced(opInst))
          liftSet.insert(opInst);
        else {
          operandDeps.insert(opInst);
          dependsOn(opInst, part, operandDeps, liftSet, induction);
        }
      }
    }
  }

  inline void dependsOn(Instruction *i1, std::set<PHINode *> *phis,
                        PHINode *induction) {
    for (auto &op : i1->operands()) {
      if (Instruction *opInst = dyn_cast<Instruction>(&op)) {
        if (opInst == induction)
          continue;
        if (opInst->getParent() != i1->getParent())
          continue;
        if (PHINode *p = dyn_cast<PHINode>(opInst))
          phis->insert(p);
        else
          dependsOn(opInst, phis, induction);
      }
    }
  }

  void getOrderedPhis(SmallVector<std::set<PHINode *>> &ordered_phis,
                      BasicBlock *bb, PHINode *induction) {
    std::map<std::set<PHINode *>, std::set<PHINode *> *> ordered_phi_deps;
    std::set<PHINode *> allPHIs;
    for (PHINode &phi : bb->phis()) {
      if (&phi == induction)
        continue;
      allPHIs.insert(&phi);
    }
    // Order of the phi instructions
    auto itr = allPHIs.begin();
    while (allPHIs.end() != allPHIs.begin()) {
      if (itr == allPHIs.end())
        itr = allPHIs.begin();
      PHINode *phi = *itr;
      Instruction *incomingVal =
          dyn_cast<Instruction>(phi->getIncomingValueForBlock(bb));
      std::set<PHINode *> dependsOnPhis;
      dependsOn(incomingVal, &dependsOnPhis, induction);
      dependsOnPhis.erase(phi);

      // if depends on something that has not been ordered then continue
      bool insert = true;
      for (PHINode *dp : dependsOnPhis) {
        if (allPHIs.find(dp) != allPHIs.end()) {
          insert = false;
          break;
        }
      }
      if (!insert) {
        itr++;
        continue;
      }

      if (ordered_phis.empty()) {
        ordered_phis.push_back({phi});
        std::set<PHINode *> *phis = &(ordered_phis[0]);
        ordered_phi_deps.insert({dependsOnPhis, phis});
      } else {
        auto findItr = ordered_phi_deps.find(dependsOnPhis);
        if (findItr != ordered_phi_deps.end()) {
          findItr->second->insert(phi);
        } else {
          size_t maxIdx = 0;
          for (auto *dp : dependsOnPhis) {
            for (size_t i = maxIdx; i < ordered_phis.size(); ++i) {
              if (ordered_phis[i].find(dp) != ordered_phis[i].end()) {
                maxIdx = i;
                break;
              }
            }
          }
          auto insertItr = ordered_phi_deps.begin();
          std::advance(insertItr, maxIdx + 1);
          ordered_phis.insert(ordered_phis.begin() + maxIdx + 1, {phi});
          ordered_phi_deps.insert(insertItr,
                                  {dependsOnPhis, &(ordered_phis[maxIdx + 1])});
        }
      }
      itr = allPHIs.erase(itr);
    }
    ordered_phi_deps.clear();
  }

  void getPart(SmallDenseSet<Instruction *> &part,
               SmallVector<Instruction *> &allInsts,
               SmallDenseSet<Instruction *> &prevInsts,
               SmallDenseSet<Instruction *> &deps, BasicBlock *BB) {
    for (Instruction *I : allInsts) {
      if (prevInsts.find(I) != prevInsts.end())
        continue;
      if (deps.find(I) != deps.end())
        continue;
      bool dep = false;
      for (auto &op : I->operands())
        if (Instruction *opInst = dyn_cast<Instruction>(&op))
          if (opInst->getParent() == BB && (deps.find(opInst) != deps.end())) {
            dep = true;
            break;
          }
      if (!dep) {
        part.insert(I);
        prevInsts.insert(I);
      } else {
        deps.insert(I);
      }
    }
  }

  void getParts(SmallVector<SmallDenseSet<Instruction *>> &parts,
                SmallDenseSet<Instruction *> &toBeReplaced,
                SmallVector<Instruction *> &allInsts, PHINode *induction,
                BasicBlock *BB, smaug::LoopVectorizationLegality &LVL) {
    std::set<Instruction *> secretSharedInstrs;

    SmallVector<std::set<PHINode *>> ordered_phis;
    getOrderedPhis(ordered_phis, BB, induction);
    SmallDenseSet<Instruction *> prevInsts, deps;
    SmallDenseSet<PHINode *> allPHIs;
    for (PHINode &phi : BB->phis()) {
      if (&phi == induction)
        continue;
      allPHIs.insert(&phi);
    }

    bool lastPartTobeVectorized = false;
    for (size_t i = 0; i < ordered_phis.size(); ++i) {
      SmallDenseSet<Instruction *> deps;
      for (auto *I : allPHIs)
        deps.insert(I);

      // part with insts dep on prevInsts but not on ordered_phis[i ...]
      SmallDenseSet<Instruction *> thisPart;
      getPart(thisPart, allInsts, prevInsts, deps, BB);
      bool containsToBeReplaced = false;
      for (auto *I : thisPart) {
        if (toBeReplaced.find(I) != toBeReplaced.end()) {
          containsToBeReplaced = true;
          break;
        }
      }
      if (containsToBeReplaced) {
        parts.push_back(thisPart);
        lastPartTobeVectorized = true;
      } else {
        if (!thisPart.empty()) {
          if (parts.empty() || lastPartTobeVectorized) {
            parts.push_back(thisPart);
          } else {
            for (Instruction *I : thisPart)
              parts.rbegin()->insert(I);
          }
        }
      }

      // part with insts dep on prevInsts and contains ordered_phis[i]
      // but not on ordered_phis[i+1 ...]
      deps.clear();
      SmallDenseSet<PHINode *> reductionPhis;
      for (auto *phi : ordered_phis[i]) {
        if (LVL.isReductionVariable(phi))
          if (phi->hasMetadata("secret_shared"))
            reductionPhis.insert(phi);
      }
      for (auto *phi : reductionPhis) {
        SmallDenseSet<Instruction *> part2;
        deps.clear();
        allPHIs.erase(phi);
        for (auto *I : allPHIs)
          deps.insert(I);
        getPart(part2, allInsts, prevInsts, deps, BB);
        parts.push_back(part2);
        lastPartTobeVectorized = true;
      }

      SmallDenseSet<Instruction *> part2;
      for (auto *phi : ordered_phis[i]) {
        allPHIs.erase(phi);
      }
      for (auto *I : allPHIs)
        deps.insert(I);
      getPart(part2, allInsts, prevInsts, deps, BB);
      if (part2.empty())
        continue;
      if (!lastPartTobeVectorized) {
        if (parts.empty())
          parts.push_back(part2);
        else {
          auto prevPartContainsMPC = false;
          for (auto *I : *(parts.rbegin())) {
            if (toBeReplaced.find(I) != toBeReplaced.end()) {
              prevPartContainsMPC = true;
              break;
            }
          }
          auto thisPartContainsMPC = false;
          for (auto *I : part2) {
            if (toBeReplaced.find(I) != toBeReplaced.end()) {
              thisPartContainsMPC = true;
              break;
            }
          }
          if (prevPartContainsMPC && thisPartContainsMPC) {
            parts.push_back(part2);
          } else {
            for (auto *I : part2) {
              parts.rbegin()->insert(I);
            }
          }
        }
      } else {
        parts.push_back(part2);
      }
      lastPartTobeVectorized = false;
    }
  }

  void toLift(BasicBlock *BB, SmallVector<SmallVector<Instruction *>> &parts) {
    SmallVector<Instruction *> toLift;
    for (auto itr = parts.begin(); itr != parts.end(); ++itr) {
    }
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

public:
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};
