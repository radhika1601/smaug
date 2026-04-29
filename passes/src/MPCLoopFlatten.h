#pragma once
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "Vectorize/LoopVectorizationLegality.h"
#include <set>
#include <tuple>

using namespace llvm;
#define DEBUG_TYPE "loop-flatten"
#include "MPCVecUtils.h"

class MPCLoopFlatten {
public:
  MPCLoopFlatten(Function &F, ScalarEvolution &SE_, LoopInfo &LI_,
                 DominatorTree &DT_, AssumptionCache &AC_,
                 LoopAccessInfoManager &LAIs_)
      : F(&F), SE(&SE_), LI(&LI_), DT(&DT_), AC(&AC_), LAIs(&LAIs_) {}

  void getUsedOutside(std::vector<BasicBlock *> &blocks, Loop *outerLoop,
                      SmallVector<Instruction *> &instructions,
                      Instruction *induction,
                      SmallVector<Instruction *> &canDuplicate,
                      BasicBlock *newHeader, BasicBlock *newLatch) {
    SmallVector<Instruction *> deleteLater;
    for (auto BB : blocks) {
      for (auto &I : *BB) {
        if (&I == induction)
          continue;
        bool found = false;
        for (auto *User : I.users()) {
          if (Instruction *userInst = dyn_cast<Instruction>(User)) {
            auto parent = userInst->getParent();
            if (parent == newHeader || parent == newLatch)
              continue;
            if (outerLoop->contains(parent)) {
              if (std::find(blocks.begin(), blocks.end(), parent) ==
                  blocks.end()) {
                if (isa<PHINode>(userInst) && userInst->getNumOperands() == 1) {
                  if (userInst->getNumUses() > 0) {
                    found = true;
                    break;
                  }
                  userInst->replaceAllUsesWith(&I);
                  deleteLater.push_back(userInst);
                } else {
                  found = true;
                  break;
                }
              }
            }
          }
        }
        if (found) {
          instructions.push_back(&I);
        }
      }
    }

    for (auto I : deleteLater) {
      I->eraseFromParent();
    }
    deleteLater.clear();

    uint itrOffset = canDuplicate.size();
    for (Instruction *I : instructions) {
      if (isa<PHINode>(I))
        continue;
      if (!I->hasMetadata("secret_shared")) {
        addToDuplicate(I, canDuplicate, blocks, induction, itrOffset);
      } else {
        if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<GetElementPtrInst>(I)) {
          if(!MPCVecUtils::isPrivateLoadStore(I))
            addToDuplicate(I, canDuplicate, blocks, induction, itrOffset);
        }
      }
    }

    for (uint i = itrOffset; i < canDuplicate.size(); ++i) {
      auto I = canDuplicate[i];
      auto itr = std::find(instructions.begin(), instructions.end(), I);
      if (itr != instructions.end())
        instructions.erase(itr);
    }
  }

  void addToDuplicate(Instruction *I, SmallVector<Instruction *> &canDuplicate,
                      std::vector<BasicBlock *> &blocks, Instruction *induction,
                      uint iteratorOffset) {
    if (isa<PHINode>(I))
      return;
    if (I == induction)
      return;
    if(isa<CallInst>(I))
      return;
    if (std::find(canDuplicate.begin(), canDuplicate.end(), I) !=
        canDuplicate.end())
      return;
    if (std::find(blocks.begin(), blocks.end(), I->getParent()) !=
        blocks.end()) {
      for (uint8_t i = 0; i < I->getNumOperands(); ++i) {
        auto op = I->getOperand(i);
        if (op == induction)
          continue;
        if (op == I) {
          errs() << "contains I with self reference\n"
                 << *(I->getParent()) << "\n";
          continue;
        }
        if (Instruction *opInst = dyn_cast<Instruction>(op)) {
          if (std::find(blocks.begin(), blocks.end(), opInst->getParent()) !=
              blocks.end()) {
            addToDuplicate(opInst, canDuplicate, blocks, induction,
                           iteratorOffset);
          }
        }
      }
      canDuplicate.push_back(I);
    }
  }

  void getFromOutside(SmallPtrSetImpl<const BasicBlock *> &blocks,
                      Loop *outerLoop,
                      SmallVector<const Instruction *> &instructions,
                      Instruction *induction, BasicBlock *newHeader,
                      BasicBlock *newLatch,
                      BasicBlock *outerLoopHeader = nullptr) {
    for (auto BB : blocks) {
      for (auto &I : *BB) {
        if (BB == outerLoopHeader && isa<PHINode>(&I)) {
          continue;
        }
        for (uint8_t i = 0; i < I.getNumOperands(); ++i) {
          Value *op = I.getOperand(i);
          if (op == induction)
            continue;
          if (Instruction *opInst = dyn_cast<Instruction>(op)) {
            auto parent = opInst->getParent();
            if (parent == newHeader)
              continue;
            if (parent == newLatch)
              continue;
            if (!blocks.contains(parent) && outerLoop->contains(parent)) {
              instructions.push_back(opInst);
            }
          }
        }
      }
    }
  }

  void getPhisToMove(std::vector<std::vector<BasicBlock *> *> &parts,
                     BasicBlock *header,
                     MapVector<int, SmallVector<PHINode *> *> &PhisToMove,
                     Instruction *induction) {
    for (auto &I : *header) {
      if (&I == induction)
        continue;
      if (PHINode *phi = dyn_cast<PHINode>(&I)) {
        uint partNum = parts.size();
        // errs() << formatv("PHInode: {0}\n", *phi);
        for (auto user : phi->users()) {
          if (auto UserInst = dyn_cast<Instruction>(user)) {
            auto parent = UserInst->getParent();
            // errs() << formatv("\t{0}, {1}\n", parent->getName(),
            // *UserInst);
            for (uint i = 0; i < parts.size(); ++i) {
              if (std::find(parts[i]->begin(), parts[i]->end(), parent) !=
                  parts[i]->end()) {
                partNum = i;
                break;
              }
            }
          }
        }
        if (partNum < parts.size()) {
          auto itr = PhisToMove.find(partNum);
          if (itr != PhisToMove.end()) {
            itr->second->push_back(phi);
          } else {
            PhisToMove.insert(
                std::make_pair(partNum, new SmallVector<PHINode *>(1, phi)));
          }
        }
      } else {
        break;
      }
    }
  }

  bool processLoop(Loop *L);

  bool runImpl() {

    bool Changed = false;

    for (const auto &L : *LI) {
      Changed |=
          simplifyLoop(L, DT, LI, SE, AC, nullptr, false /* PreserveLCSSA */);
    }

    // Build up a worklist of inner-loops to vectorize. This is necessary as the
    // act of distributing a loop creates new loops and can invalidate iterators
    // across the loops.
    SmallVector<Loop *, 8> Worklist;

    for (Loop *TopLevelLoop : *LI) {
      Changed |= formLCSSARecursively(*TopLevelLoop, *DT, LI, SE);
      for (Loop *L : depth_first(TopLevelLoop)) {
        // start from the outermost loop, do not need to process innermost loop
        if (!L->isInnermost()) {
          if (utils.hasSecretSharedInsts(L->getBlocksVector()))
            Worklist.push_back(L);
        }
      }
    }

#ifndef NDEBUG
    LLVM_DEBUG(dbgs() << "Before transformation \n\n"
                      << F->getName() << "\n");
    if (F->getName() == "check_splitting") {
      for (auto &BB : *F) {
        LLVM_DEBUG(dbgs() << BB << "\n");
      }
    }
#endif

    // Now walk the identified inner loops.
    for (Loop *L : Worklist) {
      Changed |= processLoop(L);
    }

#ifndef NDEBUG
    LLVM_DEBUG(dbgs() << "Transformed \n\n" << F->getName() << "\n");
    if (F->getName() == "check_splitting") {
      for (auto &BB : *F) {
        LLVM_DEBUG(dbgs() << BB << "\n");
      }
    }
#endif

    // Process each loop nest in the function.
    return Changed;
  }

  std::tuple<BasicBlock *, BasicBlock *, BasicBlock *>
  createLoop(Value *loopCount, IRBuilder<> &Builder, BasicBlock *insertBefore,
             BasicBlock *insertAfter, BasicBlock *BB,
             SmallVector<PHINode *> *phisToMove = nullptr,
             BasicBlock *oldLatch = nullptr, BasicBlock *oldPh = nullptr,
             StringRef name = "") {
    LLVM_DEBUG(dbgs() << "create loop " << insertAfter->getName()
                      << " " << insertBefore->getName() << "\n");

    BasicBlock *preExit =
        BasicBlock::Create(F->getContext(), name + ".latch", F, insertBefore);
    BasicBlock *preHeader =
        BasicBlock::Create(F->getContext(), name + ".preheader", F, BB);
    BasicBlock *header =
        BasicBlock::Create(F->getContext(), name + ".header", F, BB);
    PHINode *induction = PHINode::Create(loopCount->getType(), 2, "", header);
    induction->addIncoming(ConstantInt::get(loopCount->getType(), 0),
                           preHeader);
    if (phisToMove) {
      for (auto phi : *phisToMove) {
        phi->moveAfter(induction);
        phi->replaceIncomingBlockWith(oldPh, preHeader);
        phi->replaceIncomingBlockWith(oldLatch, preExit);
      }
    }
    DenseSet<PHINode *> phisToErase;
    for (PHINode &phi : BB->phis()) {
      if (phi.getNumIncomingValues() > 1) {
        LLVM_DEBUG(
            dbgs() << phi
                   << " should be phis to move or is original induction\n");
      } else {
        phi.replaceAllUsesWith(phi.getIncomingValue(0));
        phisToErase.insert(&phi);
      }
    }

    for (auto phi : phisToErase)
      phi->eraseFromParent();

    BranchInst::Create(preExit, header);
    BranchInst::Create(header, preHeader);
    Builder.SetInsertPoint(preExit);

    Value *indNext = Builder.CreateAdd(
        induction, ConstantInt::get(loopCount->getType(), 1), "ind.next");
    induction->addIncoming(indNext, preExit);
    Value *latchCmp = Builder.CreateICmpEQ(indNext, loopCount);
    BranchInst::Create(insertBefore, header, latchCmp, preExit);

    MDNode *mdnode = MDNode::get(
        header->getContext(), MDString::get(header->getContext(), "llvm.loop"));
    preExit->getTerminator()->setMetadata("llvm.loop", mdnode);
    // errs() << *header << "\n";
    return std::make_tuple(preHeader, header, preExit);
  }

  Instruction *updateLoop(Loop *innerLoop, Value *tripCount,
                          BasicBlock *insertBefore, BasicBlock *insertAfter,
                          IRBuilder<> &Builder, Instruction *outerInduction,
                          BasicBlock *prevPH, BasicBlock *LPH,
                          BasicBlock *LLatch,
                          SmallVector<PHINode *> *phisToMove = nullptr,
                          StringRef name = "") {
    LLVM_DEBUG(dbgs() << "update loop " << *innerLoop << "\n");
    auto PH = innerLoop->getLoopPreheader();

    BasicBlock *header = innerLoop->getHeader();
    // errs() << *header << "\n";
    BasicBlock *preExit = innerLoop->getLoopLatch();
    if (!header || !preExit)
      return nullptr;
    bool newPh = false;
    if (!PH) {
      newPh = true;
      PH = BasicBlock::Create(F->getContext(), name + ".preheader", F, header);
      BranchInst::Create(header, PH);
      header->replacePhiUsesWith(prevPH, PH);
      auto iaTerm = insertAfter->getTerminator();
      for (uint8_t i = 0; i < iaTerm->getNumOperands(); ++i) {
        if (iaTerm->getOperand(i) == header)
          iaTerm->setOperand(i, PH);
      }
    }
    auto innerCount = utils.getTripCount(innerLoop, SE);
    if (!innerCount) {
      LLVM_DEBUG(dbgs() << "inner count not found\n");
      if (newPh) {
        auto iaTerm = insertAfter->getTerminator();
        for (uint8_t i = 0; i < iaTerm->getNumOperands(); ++i) {
          if (iaTerm->getOperand(i) == PH)
            iaTerm->setOperand(i, header);
        }
        PH->eraseFromParent();
      }
      return nullptr;
    }
    Instruction *induction = innerLoop->getInductionVariable(*SE);
    if (!induction) {
      if (!innerLoop->getLoopPreheader())
        LLVM_DEBUG(dbgs() << "preheader not found\n");
      if (!innerLoop->getLoopLatch())
        LLVM_DEBUG(dbgs() << "loop latch not found\n");

      if (!innerLoop->hasDedicatedExits())
        LLVM_DEBUG(dbgs() << "does not have dedicated exits\n");
      if (!innerLoop->isLoopSimplifyForm())
        LLVM_DEBUG(dbgs() << "inner loop not in simplify form\n");

      if (!innerLoop->getLatchCmpInst())
        LLVM_DEBUG(dbgs() << "latch icmp inst not found\n");

      LLVM_DEBUG(dbgs() << "induction variable not found \n");
    }
    Builder.SetInsertPoint(PH->getFirstNonPHI());
    auto i1 = Builder.CreateZExtOrBitCast(tripCount, innerCount->getType());
    auto finalCount = Builder.CreateMul(i1, innerCount, name + "loopCount");
    ICmpInst *latchCmp = innerLoop->getLatchCmpInst();
    bool countUpdated = false;
    Value *indNext = nullptr;
    for (uint i = 0; i < latchCmp->getNumOperands(); ++i) {
      if (latchCmp->getOperand(i) == innerCount) {
        latchCmp->setOperand(i, finalCount);
        countUpdated = true;
      } else {
        indNext = latchCmp->getOperand(i);
      }
    }
    if (!countUpdated) {
      LLVM_DEBUG(dbgs() << "count not updated\n");
      if (newPh) {
        auto iaTerm = insertAfter->getTerminator();
        for (uint8_t i = 0; i < iaTerm->getNumOperands(); ++i) {
          if (iaTerm->getOperand(i) == PH)
            iaTerm->setOperand(i, header);
        }
        PH->eraseFromParent();
      }
      return nullptr;
    }

    Builder.SetInsertPoint(header->getFirstNonPHI());

    auto newOuterInduction = Builder.CreateUDiv(induction, innerCount);

    auto newInnerInduction = Builder.CreateURem(induction, innerCount);

    updateUsers(innerLoop->getBlocksSet(), outerInduction, Builder,
                newOuterInduction);

    SmallVector<Instruction *> users;
    for (auto user : induction->users()) {
      if (auto userInst = dyn_cast<Instruction>(user)) {
        if (userInst == newOuterInduction)
          continue;
        if (userInst == newInnerInduction)
          continue;
        if (userInst == indNext)
          continue;

        users.push_back(userInst);
      }
    }

    for (Instruction *user : users) {
      for (uint8_t i = 0; i < user->getNumOperands(); ++i) {
        if (user->getOperand(i) == induction) {
          user->setOperand(i, newInnerInduction);
        }
      }
    }

    Instruction *newInduction = dyn_cast<Instruction>(newOuterInduction);

    // update existing phi nodes other than induction to take default value when
    // prevOuterInd != outerInduction
    auto NIIInst = dyn_cast<Instruction>(newInnerInduction);
    Builder.SetInsertPoint(NIIInst->getInsertionPointAfterDef().value());
    auto InnerEqualsZero = Builder.CreateICmpEQ(
        NIIInst, ConstantInt::get(NIIInst->getType(), 0), "inner.eq.0");
    Instruction *IEZInst = dyn_cast<Instruction>(InnerEqualsZero);
    for (auto &I : *header) {
      if (&I == induction)
        continue;

      if (PHINode *phi = dyn_cast<PHINode>(&I)) {
        auto newVal = Builder.CreateSelect(
            IEZInst, phi->getIncomingValueForBlock(PH), phi);
        phi->replaceAllUsesWith(newVal);
        SelectInst *sel = dyn_cast<SelectInst>(newVal);
        sel->setFalseValue(phi);
      } else
        break;
    }

    if (phisToMove) {
      for (auto phi : *phisToMove) {
        phi->moveAfter(induction);
        phi->replaceIncomingBlockWith(LPH, PH);
        phi->replaceIncomingBlockWith(LLatch, preExit);

        // change if incoming value is from lcssa to the non-lcssa version
        auto valLatch = phi->getIncomingValueForBlock(preExit);
        if (PHINode *phiLatch = dyn_cast<PHINode>(valLatch)) {
          if (phiLatch->getNumIncomingValues() == 1) {
            phi->setIncomingValueForBlock(preExit, phiLatch->getOperand(0));
          }
        }
        Builder.SetInsertPoint(induction->getInsertionPointAfterDef().value());
        auto prevPhi = Builder.CreatePHI(phi->getType(), 2);
        prevPhi->addIncoming(phi->getIncomingValueForBlock(PH), PH);
        Builder.SetInsertPoint(IEZInst->getInsertionPointAfterDef().value());
        auto toUsePhi = Builder.CreateSelect(IEZInst, phi, prevPhi);
        phi->replaceAllUsesWith(toUsePhi);
        prevPhi->addIncoming(phi, preExit);
        SelectInst *toUseSel = dyn_cast<SelectInst>(toUsePhi);
        toUseSel->setTrueValue(phi);

        SmallVector<PHINode *> phiUsers;
        for (auto user : toUsePhi->users()) {
          if (PHINode *userPHI = dyn_cast<PHINode>(user)) {
            if (userPHI->getParent() == header) {
              if (userPHI->getIncomingValueForBlock(PH) == toUsePhi)
                phiUsers.push_back(userPHI);
            }
          }
        }

        for (PHINode *UserPHI : phiUsers) {
          UserPHI->setIncomingValueForBlock(PH,
                                            phi->getIncomingValueForBlock(PH));
          Builder.SetInsertPoint(toUseSel->getInsertionPointAfterDef().value());
          auto userPHISelect =
              Builder.CreateSelect(InnerEqualsZero, toUseSel, UserPHI);
          UserPHI->replaceAllUsesWith(userPHISelect);
          SelectInst *userSelectInst = dyn_cast<SelectInst>(userPHISelect);
          userSelectInst->setFalseValue(UserPHI);
        }
      }
    }
    MDNode *mdnode = MDNode::get(
        header->getContext(),
        MDString::get(header->getContext(), "llvm.mpc.loop.flattened"));
    header->getTerminator()->setMetadata("llvm.mpc.loop.flattened", mdnode);

    return newInduction;
  }

  bool getParts(Loop *L, std::vector<std::vector<BasicBlock *> *> &parts) {
    BasicBlock *BB = L->getHeader();
    uint depth = L->getLoopDepth();

    while (L->contains(BB)) {
      auto iL = LI->getLoopFor(BB);
      if (!iL || (iL == L)) {
        std::vector<BasicBlock *> *thisPart = new std::vector<BasicBlock *>();
        thisPart->push_back(BB);
        parts.push_back(thisPart);
        BasicBlock *nextBB = BB->getUniqueSuccessor();
        if (!nextBB) {
          auto terminator = BB->getTerminator();
          BranchInst *bi = dyn_cast<BranchInst>(terminator);
          for (uint i = 0; i < bi->getNumSuccessors(); ++i) {
            auto successor = bi->getSuccessor(i);
            if (DT->dominates(BB, successor) && BB != successor) {
              if (!nextBB) {
                nextBB = bi->getSuccessor(i);
              } else {
                LLVM_DEBUG(dbgs() << formatv(
                               "basic block has more than one successor\n"));
                return false;
              }
            }
          }
        }
        BB = nextBB;
      } else {
        if (iL->getLoopDepth() == (depth + 1)) {
          parts.push_back(&(iL->getBlocksVector()));
        }
        BB = iL->getExitBlock();
        if (!BB)
          return false;
      }
    }

    LLVM_DEBUG(dbgs() << "parts\n");
    for (auto Part : parts) {
      for (auto BB : *Part) {
        LLVM_DEBUG(dbgs() << BB->getName() << "\n");
      }
      LLVM_DEBUG(dbgs() << "\n");
    }
    return true;
  }

  void updateUsers(SmallPtrSetImpl<const BasicBlock *> &blocks, Value *oldVal,
                   IRBuilder<> &Builder, Value *newVal = nullptr,
                   Instruction *newInduction = nullptr,
                   Value *loadFrom = nullptr) {

    SmallVector<Instruction *> toUpdate;
    for (auto User : oldVal->users()) {
      if (Instruction *userInst = dyn_cast<Instruction>(User)) {
        auto parent = userInst->getParent();
        if (blocks.contains(parent)) {
          toUpdate.push_back(userInst);
        }
      }
    }

    if (toUpdate.size() > 0 && !newVal && newInduction && loadFrom) {
      auto type = oldVal->getType();
      if (type == Builder.getInt1Ty())
        type = Builder.getInt8Ty();
      if (isa<GetElementPtrInst>(oldVal)) {
        auto tmp = dyn_cast<GetElementPtrInst>(oldVal);
        type = tmp->getResultElementType();
      }
      auto gep = Builder.CreateGEP(type, loadFrom, newInduction, "", true);
      if (gep->getType() == oldVal->getType()) {
        newVal = gep;
      } else {
        newVal = Builder.CreateLoad(type, gep);
        if (oldVal->getType() == Builder.getInt1Ty()) {
          newVal = Builder.CreateTrunc(newVal, Builder.getInt1Ty());
        }
      }
    }
    for (Instruction *userInst : toUpdate) {
      bool removed = false;
      if (PHINode *phi = dyn_cast<PHINode>(userInst)) {
        if (phi->getNumIncomingValues() == 1) {
          phi->replaceAllUsesWith(newVal);
          phi->eraseFromParent();
          removed = true;
        }
      }
      if (!removed) {
        for (uint i = 0; i < userInst->getNumOperands(); ++i) {
          if (userInst->getOperand(i) == oldVal) {
            userInst->setOperand(i, newVal);
            break;
          }
        }
      }
    }
  }

  void duplicateInsts(BasicBlock *newPH, BasicBlock *newHeader,
                      BasicBlock *newLatch,
                      SmallPtrSetImpl<const BasicBlock *> &blocks,
                      SmallVector<Instruction *> &toDuplicate,
                      Instruction *newInduction,
                      SmallVector<Instruction *> &inductions,
                      Instruction *insertAfter, IRBuilder<> &Builder,
                      MapVector<Instruction *, Instruction *> &copies,
                      bool insertBefore = false) {

    // errs() << "to duplicate\n";
    // for (auto I : toDuplicate) {
    //   errs() << *I << "\n";
    // }
    // errs() << "\n";

    for (uint j = 0; j < toDuplicate.size(); ++j) {
      auto I = toDuplicate[j];
      auto cloned = I->clone();
      // cloned->setName(I->getName() + ".copy");
      cloned->takeName(I);
      if (insertBefore)
        cloned->insertBefore(insertAfter);
      else
        cloned->insertAfter(insertAfter);
      insertAfter = cloned;
      insertBefore = false;
      copies.insert(std::make_pair(I, cloned));
      for (uint8_t i = 0; i < cloned->getNumOperands(); ++i) {
        auto op = cloned->getOperand(i);
        if (auto opInst = dyn_cast<Instruction>(op)) {
          auto itr = std::find(toDuplicate.begin(), toDuplicate.end(), opInst);
          if (itr != toDuplicate.end()) {
            cloned->setOperand(i, copies[opInst]);
          } else {
            itr = std::find(inductions.begin(), inductions.end(), op);
            if (itr != inductions.end()) {
              Builder.SetInsertPoint(
                  newInduction->getInsertionPointAfterDef().value());
              auto tmp =
                  Builder.CreateZExtOrTrunc(newInduction, (*itr)->getType());
              cloned->setOperand(i, tmp);
            }
          }
        }
      }
    }

    SmallVector<PHINode *> userInPHI;
    std::set<Instruction *> updateOperand;
    for (auto I : toDuplicate) {
      for (auto user : I->users()) {
        if (auto UserInst = dyn_cast<Instruction>(user)) {
          auto parent = UserInst->getParent();
          if (blocks.contains(parent)) {
            PHINode *userPHI = dyn_cast<PHINode>(UserInst);
            if ((parent == newHeader) && userPHI &&
                userPHI->getIncomingValueForBlock(newPH) == I) {
              userInPHI.push_back(userPHI);
            } else if (updateOperand.find(UserInst) == updateOperand.end()) {
              updateOperand.insert(UserInst);
            }
          }
        }
      }
    }

    for (auto I : updateOperand) {
      for (uint8_t i = 0; i < I->getNumOperands(); ++i) {
        auto op = I->getOperand(i);
        if (auto opInst = dyn_cast<Instruction>(op)) {
          if (copies.find(opInst) != copies.end()) {
            I->setOperand(i, copies[opInst]);
          }
        }
      }
    }

    if (userInPHI.size() > 0) {
      MapVector<Instruction *, Instruction *> copiesInPH;
      insertAfter = newPH->getTerminator();
      insertBefore = true;
      for (uint j = 0; j < toDuplicate.size(); ++j) {
        auto I = toDuplicate[j];
        auto cloned = I->clone();
        // cloned->setName(I->getName() + ".copy");
        cloned->takeName(I);
        if (insertBefore)
          cloned->insertBefore(insertAfter);
        else
          cloned->insertAfter(insertAfter);
        insertAfter = cloned;
        insertBefore = false;
        copiesInPH.insert(std::make_pair(I, cloned));
        for (uint8_t i = 0; i < cloned->getNumOperands(); ++i) {
          auto op = cloned->getOperand(i);
          if (auto opInst = dyn_cast<Instruction>(op)) {
            auto itr =
                std::find(toDuplicate.begin(), toDuplicate.end(), opInst);
            if (itr != toDuplicate.end()) {
              cloned->setOperand(i, copiesInPH[opInst]);
            } else {
              itr = std::find(inductions.begin(), inductions.end(), op);
              if (itr != inductions.end()) {
                cloned->setOperand(i, ConstantInt::get(op->getType(), 0));
              }
            }
          }
        }
      }

      for (PHINode *I : userInPHI) {
        auto incomingVal = I->getIncomingValueForBlock(newPH);
        if (Instruction *incomingInst = dyn_cast<Instruction>(incomingVal)) {
          if (copiesInPH.find(incomingInst) != copiesInPH.end()) {
            I->setIncomingValueForBlock(newPH, copiesInPH[incomingInst]);
          }
        }
      }
      copiesInPH.clear();
    }
    userInPHI.clear();
    updateOperand.clear();
    toDuplicate.clear();
  }

  void findToDuplicate(Instruction *I, SmallVector<Instruction *> &toDuplicate,
                       SmallVector<Instruction *> &canDuplicate,
                       SmallVector<Instruction *> *inductions = nullptr) {
    if (inductions && std::find(inductions->begin(), inductions->end(), I) !=
                          inductions->end())
      return;
    if (std::find(toDuplicate.begin(), toDuplicate.end(), I) !=
        toDuplicate.end())
      return;
    if (std::find(canDuplicate.begin(), canDuplicate.end(), I) ==
        canDuplicate.end())
      return;
    for (uint8_t i = 0; i < I->getNumOperands(); ++i) {
      auto op = I->getOperand(i);
      if (auto opInst = dyn_cast<Instruction>(op)) {
        findToDuplicate(opInst, toDuplicate, canDuplicate, inductions);
      }
    }
    // toDuplicate.insert(toDuplicate.begin(), I);
    toDuplicate.push_back(I);
  }

private:
  Function *F;
  ScalarEvolution *SE;
  LoopInfo *LI;
  DominatorTree *DT;
  AssumptionCache *AC;
  LoopAccessInfoManager *LAIs;
  MPCVecUtils utils;
};

bool MPCLoopFlatten::processLoop(Loop *L) {

  if (!L->isLoopSimplifyForm()) {
    LLVM_DEBUG(dbgs() << "run loop simplify pass first\n");
    return false;
  }
  if (!L->getExitBlock()) {
    LLVM_DEBUG(dbgs() << "more than one exit blocks\n");
    return false;
  }
  const LoopAccessInfo *LAI = &(LAIs->getInfo(*L));
  const MemoryDepChecker *MDC = &(LAI->getDepChecker());
  if (!MDC->isSafeForAnyVectorWidth()) {
    LLVM_DEBUG(dbgs() << "!MDC->isSafeForAnyVectorWidth() \n");
    return false;
  }

  MPCVecUtils utils = MPCVecUtils();
  Value *tripCount = utils.getTripCount(L, SE);
  if (tripCount == nullptr) {
    LLVM_DEBUG(dbgs() << "trip count not found\n");
    return false;
  }

  if (!L->isLCSSAForm(*DT)) {
    LLVM_DEBUG(dbgs() << "not in lcsaa form\n");
  }

  std::vector<std::vector<BasicBlock *> *> parts;

  if (!getParts(L, parts))
    return false;

  // errs() << "parts size " << parts.size() << "\n";
  /*
    find sub loops => make separate parts

    L => BB1 -> L1 -> BB2 -> L2 -> BB3 -> BB1

    part, instructions to lift, instructions to duplicate in the header of new
    loop

    parts:  BB1 (insts used outside BB1 (except inductionL))
            L1 (insts used outside L1 in L)

    since the loops are in the lcssa form

  */
  Instruction *induction = L->getInductionVariable(*SE);
  PHINode *phiInd = dyn_cast<PHINode>(induction);
  Value *indNext = phiInd->getIncomingValueForBlock(L->getLoopLatch());
  MapVector<Instruction *, Value *> ItoPtrMap;
  SmallVector<Instruction *> freeLater;
  IRBuilder<> Builder(F->getContext());
  BasicBlock *PH = L->getLoopPreheader();
  BasicBlock *header = L->getHeader();
  BasicBlock *latch = L->getLoopLatch();
  BasicBlock *Exit = L->getExitBlock();
  BasicBlock *insertAfter = PH;
  uint partNum = 0;
  SmallVector<Instruction *> usedOutside, canDuplicate;
  if (parts.size() <= 1)
    return false;

  MapVector<int, SmallVector<PHINode *> *> phisToMove;
  // phi nodes used outside the header except induction
  getPhisToMove(parts, header, phisToMove, induction);

  // errs() << "phis to move\n";
  // for (auto itr : phisToMove) {
  //   errs() << itr.first << "\n";
  //   for (auto I : *(itr.second)) {
  //     errs() << "\t" << *I << "\n";
  //   }
  // }

  // errs() << "\n\n\n";

  SmallVector<Instruction *> inductions;
  BasicBlock *insertBefore = *(parts[1]->begin());
  SmallVector<Instruction *> toDuplicateInThisPart;

  for (std::vector<BasicBlock *> *Part : parts) {
    usedOutside.clear();

    SmallPtrSetImpl<const BasicBlock *> *blocks = nullptr;
    BasicBlock *BB = (*Part)[0];
    BasicBlock *newPH = nullptr, *newHeader = nullptr, *newLatch = nullptr;
    auto iL = LI->getLoopFor(BB);
    Instruction *newInduction = nullptr, *storeInsertPt = nullptr;

    if (iL == L)
      iL = nullptr;

    auto itr = phisToMove.find(partNum);
    SmallVector<PHINode *> *phisToduplicate = nullptr;
    if (itr != phisToMove.end()) {
      phisToduplicate = itr->second;
    }
    if (iL) {
      blocks = &(iL->getBlocksSet());
      newInduction = updateLoop(
          iL, tripCount, insertBefore, insertAfter, Builder, induction,
          *(parts[partNum - 1]->rbegin()), PH, latch, phisToduplicate, "");
      if (newInduction) {
        storeInsertPt = iL->getLatchCmpInst();
        newHeader = iL->getHeader();
        newLatch = iL->getLoopLatch();
        newPH = iL->getLoopPreheader();
      }
    }

    if (!newInduction) {
      if (!iL) {
        blocks = new SmallPtrSet<const BasicBlock *, 1>();
        blocks->insert(BB);
      } else
        blocks = &(iL->getBlocksSet());
      auto newLoop = createLoop(tripCount, Builder, insertBefore, insertAfter,
                                BB, phisToduplicate, latch, PH);

      newPH = get<0>(newLoop);
      newHeader = get<1>(newLoop);
      newLatch = get<2>(newLoop);
      Instruction &tmp = *(newHeader->begin());

      newInduction = &tmp;
      PHINode *newIndPHI = dyn_cast<PHINode>(newInduction);
      storeInsertPt =
          dyn_cast<Instruction>(newIndPHI->getIncomingValueForBlock(newLatch));
      updateUsers(*blocks, induction, Builder, newInduction);

      auto terminator = insertAfter->getTerminator();
      terminator->replaceSuccessorWith(BB, newPH);

      auto newBr = BranchInst::Create(BB);
      auto oldBr = newHeader->getTerminator();
      newBr->copyMetadata(*oldBr);
      ReplaceInstWithInst(oldBr, newBr);

      if (!iL) {
        BasicBlock *lastBB = *(Part->rbegin());
        terminator = lastBB->getTerminator();
        newBr = BranchInst::Create(newLatch);
        newBr->copyMetadata(*terminator);
        ReplaceInstWithInst(terminator, newBr);
      } else {
        BasicBlock *exit = iL->getExitBlock();
        for (BasicBlock *pred : predecessors(exit)) {
          if (iL->contains(pred)) {
            pred->getTerminator()->replaceSuccessorWith(exit, newLatch);
          }
        }
      }
    }
    getUsedOutside(*Part, L, usedOutside, induction, canDuplicate, newHeader,
                   newLatch);

    inductions.push_back(newInduction);
    SmallVector<const Instruction *> fromOutside;
    getFromOutside(*blocks, L, fromOutside, induction, newHeader, newLatch,
                   header);

    errs() << "to duplicate overall \n";
    for (auto I : canDuplicate) {
      errs() << "\t" << *I << "\n";
    }
    // errs() << "from outside\n";
    // for (auto I : fromOutside) {
    //   errs() << *I << "\n";
    // }

    toDuplicateInThisPart.clear();

    for (auto I : fromOutside) {
      auto itr = std::find(canDuplicate.begin(), canDuplicate.end(), I);
      if (itr != canDuplicate.end())
        findToDuplicate(*itr, toDuplicateInThisPart, canDuplicate, &inductions);
    }

    Instruction *insertDuplicatedInstrsAfter;
    if (isa<PHINode>(newInduction)) {
      insertDuplicatedInstrsAfter = newHeader->getFirstNonPHI();
      Builder.SetInsertPoint(newHeader->getFirstNonPHI());
    } else {
      Instruction &tmp = *(newInduction->getInsertionPointAfterDef().value());
      insertDuplicatedInstrsAfter = &tmp;
      Builder.SetInsertPoint(newInduction->getInsertionPointAfterDef().value());
    }
    MapVector<Instruction *, Instruction *> copies;
    copies.clear();

    duplicateInsts(newPH, newHeader, newLatch, *blocks, toDuplicateInThisPart,
                   newInduction, inductions, insertDuplicatedInstrsAfter,
                   Builder, copies, isa<PHINode>(newInduction));

    for (auto ItoPtr : ItoPtrMap) {
      auto val = ItoPtr.first;
      updateUsers(*blocks, val, Builder, nullptr, newInduction, ItoPtr.second);
    }

    for (auto val : usedOutside) {
      if (BB == header) {
        if (isa<PHINode>(val)) {
          auto itr = phisToMove.find(0);
          if (itr != phisToMove.end()) {
            if (std::find(itr->second->begin(), itr->second->end(), val) ==
                itr->second->end())
              continue;
          } else {
            continue;
          }
        }
      } else if (PHINode *phi = dyn_cast<PHINode>(val)) {
        if (phi->getNumIncomingValues() == 1) {
          auto v1 = phi->getIncomingValue(0);
          auto i1 = dyn_cast<Instruction>(v1);
          if (i1) {
            auto itr = ItoPtrMap.find(i1);
            if (itr != ItoPtrMap.end()) {
              phi->replaceAllUsesWith(i1);
              phi->eraseFromParent();
              continue;
            }
          }
        }
      }
      bool newPtr = true;

      if (LoadInst *loadInst = dyn_cast<LoadInst>(val)) {
        Value *ptr = loadInst->getPointerOperand();
        if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ptr)) {
          Value *index = gep->getOperand(1);
          if (index == induction || index == newInduction) {
            ItoPtrMap.insert(std::make_pair(val, gep->getOperand(0)));
            newPtr = false;
          }
        }
      } else if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(val)) {
        Value *index = gep->getOperand(1);
        if (index == induction || index == newInduction) {
          ItoPtrMap.insert(std::make_pair(val, gep->getOperand(0)));
          newPtr = false;
        }
      }
      if (val == indNext) {
        newPtr = false;
        continue;
      }

      Builder.SetInsertPoint(newPH->getFirstNonPHI());

      if (newPtr) {
        // TODO: update to malloc and free

        // Get or insert the malloc and free functions
        // llvm::Function *mallocFunc = cast<llvm::Function>(
        //     F->getParent()
        //         ->getOrInsertFunction(
        //             "malloc",
        //             FunctionType::get(PointerType::get(val->getType(), 0),
        //                               tripCount->getType(), false))
        //         .getCallee());
        // uint64_t elementSize =
        //     F->getParent()->getDataLayout().getTypeAllocSize(val->getType());

        // auto size = Builder.CreateMul(tripCount,
        // Builder.getInt64(elementSize)); auto ptr =
        // Builder.CreateCall(mallocFunc, size,
        //                               val->getName() + ".ptr");
        auto type = val->getType();
        Value *toStore = val;
        if (val->getType() == Builder.getInt1Ty()) {
          type = Builder.getInt8Ty();
        }
        auto ptr = utils.createMalloc(Builder, F, toStore->getType(), tripCount,
                                      val->hasMetadata("secret_shared"), "");
        ItoPtrMap.insert(std::make_pair(val, ptr));
        freeLater.push_back(ptr);
        Builder.SetInsertPoint(storeInsertPt);
        if (val->getType() != type)
          toStore = Builder.CreateZExt(val, Builder.getInt8Ty());

        auto gep = Builder.CreateGEP(type, ItoPtrMap[val], newInduction);
        Builder.CreateStore(toStore, gep, false);
      }
    }

    insertAfter = newLatch;
    partNum += 1;
    if (partNum + 1 < parts.size()) {
      insertBefore = *(parts[partNum + 1]->begin());
    } else {
      insertBefore = Exit;
    }
    if (!iL)
      blocks->clear();
  }

  SmallVector<Instruction *> deleteLater;
  for (auto &I : *Exit) {
    if (PHINode *phi = dyn_cast<PHINode>(&I)) {
      phi->replaceAllUsesWith(phi->getIncomingValue(0));
      deleteLater.push_back(phi);
    }
  }
  for (auto phi : deleteLater) {
    phi->eraseFromParent();
  }
  //   insertBefore->replacePhiUsesWith(PH, insertAfter);

  induction->eraseFromParent();

  // Declare the free function if it's not already declared
  Type *VoidTy = Type::getVoidTy(F->getContext());
  Type *Int8PtrTy = Builder.getPtrTy();
  FunctionType *FreeTy = FunctionType::get(VoidTy, Int8PtrTy, false);
  FunctionCallee FreeFunc = F->getParent()->getOrInsertFunction("free", FreeTy);
  Builder.SetInsertPoint(Exit->getFirstNonPHI());
  for (auto ptr : freeLater) {
    Builder.CreateCall(FreeFunc, ptr);
  }

  return false;
}
