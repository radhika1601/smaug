#include "MPCHierarchical.h"
#include "CustomPostOrder.h"
#include "llvm/ADT/SCCIterator.h"

#define DEBUG_TYPE "mpc-hierarchical"
#include "./PathCondition/PathCondition.h"

using namespace llvm;

// ============================================================
// Utility: removePanics (shared with MPCTransform)
// ============================================================

static bool removePanics(llvm::Function& F) {
    std::set<BasicBlock*> preds;
    std::set<BasicBlock*> deleteBBs;
    for (BasicBlock& BB : F) {
        Instruction* I = BB.getFirstNonPHI();
        if (CallInst* ci = dyn_cast<CallInst>(I)) {
            if (!ci->getCalledFunction())
                continue;
            std::string name = ci->getCalledFunction()->getName().str();
            if (name.find("panicking") != std::string::npos && name.find("core") != std::string::npos) {
                preds.clear();
                for (BasicBlock* pred : predecessors(&BB))
                    preds.insert(pred);
                for (BasicBlock* pred : preds) {
                    Instruction* term = pred->getTerminator();
                    if (BranchInst* b = dyn_cast<BranchInst>(term)) {
                        if (!b->isConditional())
                            return false;
                        auto tmp = term->getSuccessor(0);
                        if (term->getSuccessor(0) == &BB)
                            tmp = term->getSuccessor(1);
                        BranchInst* newTerm = BranchInst::Create(tmp);
                        llvm::ReplaceInstWithInst(term, newTerm);
                    }
                    else {
                        return false;
                    }
                }
                deleteBBs.insert(&BB);
            }
        }
    }
    for (BasicBlock* BB : deleteBBs)
        BB->eraseFromParent();
    return true;
}

// ============================================================
// Utility methods
// ============================================================

inline PHINode* MPCHierarchicalPass::createPhi(Instruction& I, BasicBlock* BB, BasicBlock* lastBB, BasicBlock* newBB) {
    auto type       = I.getType();
    auto terminator = newBB->getTerminator();
    PHINode* phi    = nullptr;
    if (terminator)
        phi = PHINode::Create(type, 2, "", terminator);
    else
        phi = PHINode::Create(type, 2, "", newBB);
    phi->addIncoming(&I, BB);
    if (type->isIntegerTy())
        phi->addIncoming(ConstantInt::get(I.getType(), 0, false), lastBB);
    else if (type->isFloatingPointTy())
        phi->addIncoming(ConstantFP::get(type, 0), lastBB);
    else if (type->isPointerTy())
        phi->addIncoming(Constant::getNullValue(type), lastBB);
    return phi;
}

inline bool MPCHierarchicalPass::checkNot(Value* v1, Value* v2) {
    Instruction* i1 = dyn_cast<Instruction>(v1);
    Instruction* i2 = dyn_cast<Instruction>(v2);
    if (!i1 || !i2)
        return false;
    auto check = [](Instruction* a, Instruction* b) -> bool {
        if (!a->isBinaryOp(BinaryOperator::Xor))
            return false;
        auto ops = a->getOperandList();
        Value* x = nullptr;
        if (ops[0] == b)
            x = ops[1];
        else if (ops[1] == b)
            x = ops[0];
        if (!x)
            return false;
        if (ConstantInt* CI = dyn_cast<ConstantInt>(x))
            return CI->isOne();
        return false;
    };
    return check(i1, i2) || check(i2, i1);
}

llvm::Instruction* MPCHierarchicalPass::getFirstNonPhiAfter(llvm::Instruction* inst) {
    if (!inst || !inst->getParent())
        return nullptr;
    llvm::BasicBlock* block = inst->getParent();
    bool startChecking      = false;
    for (auto& I : *block) {
        if (&I == inst) {
            startChecking = true;
            continue;
        }
        if (startChecking && !llvm::isa<llvm::PHINode>(I))
            return &I;
    }
    return nullptr;
}

inline Value* MPCHierarchicalPass::createAnd(Value* v1, Value* v2, Instruction* insertBefore, DominatorTree* domTree,
                                             bool isAnd) {
    if (v1 == v2)
        return v1;
    if (v1 == nullptr)
        return v2;
    if (v2 == nullptr)
        return v1;
    if (v1 == trueValue)
        return v2;
    if (v2 == trueValue)
        return v1;

    Instruction* new_inst;
    if (isAnd)
        new_inst = BinaryOperator::CreateAnd(v1, v2, "", insertBefore);
    else
        new_inst = BinaryOperator::CreateOr(v1, v2, "", insertBefore);

    if (insertBefore == nullptr) {
        Instruction* i1          = dyn_cast<Instruction>(v1);
        Instruction* i2          = dyn_cast<Instruction>(v2);
        Instruction* insertAfter = i1;
        if (domTree->dominates(i1, i2))
            insertAfter = i2;
        if (isa<PHINode>(insertAfter))
            new_inst->insertBefore(getFirstNonPhiAfter(insertAfter));
        else
            new_inst->insertAfter(insertAfter);
    }

    MDNode* node = nullptr;
    if (isa<Instruction>(v1)) {
        Instruction* i1 = dyn_cast<Instruction>(v1);
        if (i1->hasMetadata("secret_shared"))
            node = i1->getMetadata("secret_shared");
    }
    if (isa<Instruction>(v2)) {
        Instruction* i2 = dyn_cast<Instruction>(v2);
        if (i2->hasMetadata("secret_shared"))
            node = i2->getMetadata("secret_shared");
    }
    if (node)
        new_inst->setMetadata("secret_shared", node);
    return new_inst;
}

std::pair<Value*, Value*>* MPCHierarchicalPass::getValForTree(
    ConditionTree* tree, std::map<ConditionTree*, std::pair<Value*, Value*>>& conditionValMap, DominatorTree& domTree,
    BasicBlock* bb) {
    if (conditionValMap.find(tree) == conditionValMap.end()) {
        if (tree->op == ConditionOP::NOT) {
            auto p = getValForTree(tree->children[0], conditionValMap, domTree, bb);
            if (p->first != trueValue || p->second != trueValue) {
                Instruction* i1 = dyn_cast<Instruction>(p->first);
                Instruction* i2 = dyn_cast<Instruction>(p->second);
                if (p->first == trueValue || (i1 && i2 && domTree.dominates(i1, i2))) {
                    Instruction* i = dyn_cast<Instruction>(p->second);
                    auto newInst   = BinaryOperator::CreateNot(i, "");
                    if (isa<PHINode>(i))
                        newInst->insertBefore(getFirstNonPhiAfter(i));
                    else
                        newInst->insertAfter(i);
                    conditionValMap.insert(std::make_pair(tree, std::make_pair(p->first, newInst)));
                }
                else {
                    Instruction* i = dyn_cast<Instruction>(p->first);
                    auto newInst   = BinaryOperator::CreateNot(i, "");
                    if (isa<PHINode>(i))
                        newInst->insertBefore(getFirstNonPhiAfter(i));
                    else
                        newInst->insertAfter(i);
                    newInst->setMetadata("secret_shared", i->getMetadata("secret_shared"));
                    conditionValMap.insert(std::make_pair(tree, std::make_pair(newInst, p->second)));
                }
            }
        }
        else if (tree->op == ConditionOP::AND) {
            auto p1 = getValForTree(tree->children[0], conditionValMap, domTree, bb);
            auto p2 = getValForTree(tree->children[1], conditionValMap, domTree, bb);
            auto v1 = createAnd(p1->first, p2->first, nullptr, &domTree);
            auto v2 = createAnd(p1->second, p2->second, nullptr, &domTree);
            conditionValMap.insert(std::make_pair(tree, std::make_pair(v1, v2)));
        }
        else {  // OR
            std::map<BasicBlock*, bool> preds;
            for (BasicBlock* Pred : predecessors(bb))
                preds.insert(std::make_pair(Pred, false));

            PHINode* publicPhi  = PHINode::Create(trueValue->getType(), preds.size(), "publicCond");
            PHINode* privatePhi = PHINode::Create(trueValue->getType(), preds.size(), "privateCond");
            publicPhi->insertBefore(bb->getFirstNonPHI());
            privatePhi->insertBefore(bb->getFirstNonPHI());

            MDNode* node = nullptr;
            Value *priv = nullptr, *pub = nullptr;
            bool samePriv = true, samePub = true;

            for (auto child : tree->children) {
                auto conditions    = getValForTree(child, conditionValMap, domTree, bb);
                BasicBlock* parent = nullptr;
                if (conditions->first != trueValue) {
                    auto inst = dyn_cast<Instruction>(conditions->first);
                    parent    = inst->getParent();
                    node      = inst->getMetadata("secret_shared");
                }
                if (conditions->second != trueValue) {
                    auto inst = dyn_cast<Instruction>(conditions->second);
                    auto tmp  = inst->getParent();
                    if (parent) {
                        if (domTree.dominates(parent, tmp))
                            parent = tmp;
                    }
                }

                if (!parent) {
                    conditionValMap.insert(std::make_pair(tree, std::make_pair(trueValue, trueValue)));
                    break;
                }
                else {
                    if (preds.find(parent) != preds.end()) {
                        privatePhi->addIncoming(conditions->first, parent);
                        publicPhi->addIncoming(conditions->second, parent);
                        preds[parent] = true;
                    }
                    else {
                        for (auto pred : preds)
                            if (!pred.second)
                                if (domTree.dominates(parent, pred.first))
                                    parent = pred.first;
                        privatePhi->addIncoming(conditions->first, parent);
                        publicPhi->addIncoming(conditions->second, parent);
                        preds[parent] = true;
                    }
                }

                if (priv == nullptr)
                    priv = conditions->first;
                else if (priv != conditions->first)
                    samePriv = false;
                if (pub == nullptr)
                    pub = conditions->second;
                else if (pub != conditions->second)
                    samePub = false;
            }

            if (!samePriv)
                priv = privatePhi;
            else
                privatePhi->eraseFromParent();
            if (!samePub)
                pub = publicPhi;
            else
                publicPhi->eraseFromParent();

            if (node) {
                if (isa<Instruction>(priv))
                    dyn_cast<Instruction>(priv)->setMetadata("secret_shared", node);
                conditionValMap.insert(std::make_pair(tree, std::make_pair(priv, pub)));
            }
            else {
                conditionValMap.insert(std::make_pair(tree, std::make_pair(trueValue, pub)));
            }
        }
    }
    return &(conditionValMap[tree]);
}

// ============================================================
// getConditionVals — builds private_pc / public_pc for all BBs
// ============================================================

void MPCHierarchicalPass::getConditionVals(Function& F) {
    DominatorTree domTree = DominatorTree(F);
    conditionValMap.insert(std::make_pair(pc->trueTree, std::make_pair(trueValue, trueValue)));

    for (auto item : pc->singleNodeConditions) {
        if (std::find(pc->private_cond.begin(), pc->private_cond.end(), item.first) != pc->private_cond.end()) {
            conditionValMap.insert(std::make_pair(item.second, std::make_pair(item.first, trueValue)));
        }
        else {
            conditionValMap.insert(std::make_pair(item.second, std::make_pair(trueValue, item.first)));
        }
    }

    for (auto item : pc->negSingleNodeConditions) {
        Instruction* i       = dyn_cast<Instruction>(item.first);
        Instruction* newInst = BinaryOperator::CreateNot(item.first, "");
        if (isa<PHINode>(i))
            newInst->insertBefore(getFirstNonPhiAfter(i));
        else
            newInst->insertAfter(i);
        if (i->hasMetadata("secret_shared")) {
            newInst->setMetadata("secret_shared", i->getMetadata("secret_shared"));
            conditionValMap.insert(std::make_pair(item.second, std::make_pair(newInst, trueValue)));
        }
        else {
            conditionValMap.insert(std::make_pair(item.second, std::make_pair(trueValue, newInst)));
        }
    }

    SmallVector<BasicBlock*> bb_queue;
    domTree.getDescendants(domTree.getRoot(), bb_queue);
    auto itr = bb_queue.begin();
    while (!bb_queue.empty()) {
        if (itr == bb_queue.end())
            itr = bb_queue.begin();
        BasicBlock* BB = *itr;

        bool cont = true;
        for (auto pred_itr = pred_begin(BB); pred_itr != pred_end(BB); ++pred_itr) {
            if (domTree.dominates(BB, *pred_itr))
                continue;
            if (private_pc.find(*pred_itr) == private_pc.end()) {
                cont = false;
                break;
            }
        }

        if (cont) {
            std::pair<Value*, Value*>* vals = getValForTree(pc->conditions[BB], conditionValMap, domTree, BB);
            private_pc.insert(std::make_pair(BB, vals->first));
            public_pc.insert(std::make_pair(BB, vals->second));
            bb_queue.erase(itr);

            llvm::SmallVector<llvm::BasicBlock*> descendants;
            domTree.getDescendants(BB, descendants);
            for (auto desc_itr = descendants.begin(); desc_itr != descendants.end(); ++desc_itr) {
                if (domTree.dominates(*desc_itr, BB))
                    continue;
                if (private_pc.find(*desc_itr) == private_pc.end()) {
                    if (std::find(bb_queue.begin(), bb_queue.end(), *desc_itr) == bb_queue.end())
                        bb_queue.push_back(*desc_itr);
                }
            }
        }
        else {
            itr++;
        }
    }
}

// ============================================================
// updateSecretStoreInst
// ============================================================

void MPCHierarchicalPass::updateSecretStoreInst(BasicBlock& bb, Value* cond) {
    if (cond != trueValue && cond != nullptr) {
        MapVector<Instruction*, Instruction*> replace;
        for (auto& I : bb) {
            if (StoreInst* storeInst = dyn_cast<StoreInst>(&I)) {
                auto ptr       = storeInst->getPointerOperand();
                auto val       = storeInst->getValueOperand();
                Type* type     = val->getType();
                LoadInst* load = new LoadInst(type, ptr, "", storeInst);
                Instruction* sel;
                if (Instruction* condI = dyn_cast<Instruction>(cond))
                    sel = SelectInst::Create(cond, val, load, "", storeInst, condI);
                else
                    sel = SelectInst::Create(cond, val, load, "", storeInst);
                replace.insert(std::make_pair(storeInst, sel));
            }
        }
        for (auto& item : replace) {
            StoreInst* storeInst = dyn_cast<StoreInst>(item.first);
            auto ptr             = storeInst->getPointerOperand();
            StoreInst* newStore  = new StoreInst(item.second, ptr, item.first);
            item.first->replaceAllUsesWith(newStore);
            item.first->eraseFromParent();
        }
    }
}

// ============================================================
// Hierarchical helpers
// ============================================================

SmallVector<BasicBlock*> MPCHierarchicalPass::getDirectBlocks(Loop* L) {
    SmallVector<BasicBlock*> result;
    for (BasicBlock* BB : L->getBlocks())
        if (LI->getLoopFor(BB) == L)
            result.push_back(BB);
        else if (LI->isLoopHeader(BB) && LI->getLoopFor(BB)->getParentLoop() == L)
            result.push_back(BB);
    return result;
}

void MPCHierarchicalPass::topologicalOrderForLoop(Loop* L, SmallVector<BasicBlock*>& order) {
    order.clear();
    auto directBlocks = getDirectBlocks(L);
    SmallPtrSet<BasicBlock*, 16> directSet(directBlocks.begin(), directBlocks.end());

    // loop_po_iterator collapses inner loops: when it hits an inner loop header
    // it jumps directly to that loop's exits, so inner loop bodies are skipped.
    // We additionally filter to directSet for safety.
    BasicBlock* exitBlock = L->getExitBlock();
    for (loop_po_iterator It = loop_po_iterator::begin(L->getHeader(), *LI, L),
                          IE = loop_po_iterator::end(L->getLoopLatch());
         It != IE; ++It) {
        BasicBlock* BB = *It;
        if (directSet.count(BB))
            order.push_back(BB);
    }
    (void)exitBlock;
}

void MPCHierarchicalPass::topologicalOrderFunctionLevel(Function& F, SmallVector<BasicBlock*>& order, LoopInfo* LI) {
    order.clear();

    for (scc_iterator<Function*> I = scc_begin(&F), IE = scc_end(&F); I != IE; ++I) {
        // Obtain the vector of BBs in this SCC and print it out.
        const std::vector<BasicBlock*>& SCCBBs = *I;

        if (SCCBBs.size() > 1) {
            // It is a loop
            auto itr = SCCBBs.end();
            itr--;  // Loop header
            Loop* L = LI->getLoopFor(*itr);
            order.push_back(L->getHeader());
        }
        else {
            BasicBlock* BB = *(SCCBBs.begin());
            order.push_back(BB);
        }
    }
}

// ============================================================
// updatePhi
// ============================================================

void MPCHierarchicalPass::updatePhi(BasicBlock* BB, PostDominatorTree* PDT, DominatorTree* DT,
                                    SmallVector<BasicBlock*>& SortedBBs, SmallVector<Value*>& PHICondSelIntrs,
                                    LoopInfo* LI) {
    if (LI->isLoopHeader(BB))
        return;
    if (BB->getUniquePredecessor())
        return;

    SmallVector<BasicBlock*> oldPreds;
    MapVector<Value*, SmallVector<BasicBlock*>> samePrivateCondOrdered;
    SmallVector<Value*> orderedPrivateConditions;
    SmallVector<PHINode*> removeInsts;
                                        bool loopHeader = false;
    for (auto& I : *BB) {
        if (PHINode* phi = dyn_cast<PHINode>(&I)) {
            int n = phi->getNumIncomingValues();
            if (!loopHeader && oldPreds.size() == 0) {
                MapVector<Value*, SmallVector<BasicBlock*>> samePrivateCond;
                for (int i = 0; i < n; ++i) {
                    auto pred = phi->getIncomingBlock(i);
                    if (DT->dominates(BB, pred)) {
                        loopHeader = true;
                        break;
                    }
                    oldPreds.push_back(pred);
                    auto cond = private_pc[pred];
                    if (samePrivateCond.find(cond) == samePrivateCond.end()) {
                        samePrivateCond.insert(std::make_pair(cond, SmallVector<BasicBlock*>(0)));
                        samePrivateCond[cond].push_back(pred);
                    }
                    else {
                        samePrivateCond[cond].push_back(pred);
                    }
                }

                if (!loopHeader) {
                    for (int i = SortedBBs.size() - 1; i >= 0; --i) {
                        for (auto pair : samePrivateCond) {
                            auto itr = std::find(pair.second.begin(), pair.second.end(), SortedBBs[i]);
                            if (itr != pair.second.end()) {
                                if (samePrivateCondOrdered.find(pair.first) == samePrivateCondOrdered.end()) {
                                    samePrivateCondOrdered.insert(
                                        std::make_pair(pair.first, SmallVector<BasicBlock*>(0)));
                                }
                                samePrivateCondOrdered[pair.first].push_back(SortedBBs[i]);
                                break;
                            }
                        }
                    }
                    samePrivateCond.clear();
                    for (int i = SortedBBs.size() - 1; i >= 0; --i) {
                        for (auto pair : samePrivateCondOrdered) {
                            if (pair.second.back() == SortedBBs[i]) {
                                orderedPrivateConditions.push_back(pair.first);
                                break;
                            }
                        }
                    }
                }
            }

            if (!loopHeader) {
                Instruction* insertBefore = BB->getFirstNonPHI();
                bool moveLater            = false;
                if (private_pc[BB] == phi || public_pc[BB] == phi)
                    moveLater = true;

                MapVector<Value*, Value*> privateCondSel;
                for (auto privPair : samePrivateCondOrdered) {
                    Value* finalInst = phi->getIncomingValueForBlock(privPair.second[0]);
                    for (size_t i = 1; i < privPair.second.size(); ++i) {
                        finalInst = SelectInst::Create(public_pc[privPair.second[i]],
                                                       phi->getIncomingValueForBlock(privPair.second[i]), finalInst, "",
                                                       insertBefore);
                        if (moveLater)
                            PHICondSelIntrs.push_back(finalInst);
                    }
                    privateCondSel.insert(std::make_pair(privPair.first, finalInst));
                }

                if (orderedPrivateConditions.empty()) {
                    removeInsts.push_back(phi);
                    continue;
                }

                Value* finalInst = privateCondSel[orderedPrivateConditions[0]];
                for (size_t i = 1; i < orderedPrivateConditions.size(); ++i) {
                    finalInst =
                        SelectInst::Create(orderedPrivateConditions[i], privateCondSel[orderedPrivateConditions[i]],
                                           finalInst, "", insertBefore);
                    setSecretShared(finalInst);
                }
                phi->replaceAllUsesWith(finalInst);
                for (auto item : private_pc)
                    if (item.second == phi)
                        private_pc[item.first] = finalInst;
                for (auto item : public_pc)
                    if (item.second == phi)
                        public_pc[item.first] = finalInst;
                removeInsts.push_back(phi);
            }
        }
        else {
            break;
        }
    }

    oldPreds.clear();
    samePrivateCondOrdered.clear();
    orderedPrivateConditions.clear();
    for (auto I : removeInsts)
        I->eraseFromParent();
}

// ============================================================
// updatePhiLoopHeader
// ============================================================

void MPCHierarchicalPass::updatePhiLoopHeader(BasicBlock* BB, SmallVector<BasicBlock*>& oldBBs,
                                              SmallVector<BasicBlock*>& newBBs) {
    for (auto& I : *BB) {
        if (PHINode* phi = dyn_cast<PHINode>(&I)) {
            int n = phi->getNumIncomingValues();
            for (int i = 0; i < n; ++i) {
                auto pred = phi->getIncomingBlock(i);
                auto itr  = std::find(oldBBs.begin(), oldBBs.end(), pred);
                if (itr != oldBBs.end()) {
                    int idx = itr - oldBBs.begin();
                    phi->replaceIncomingBlockWith(pred, newBBs[idx]);
                }
            }
        }
    }
}

// ============================================================
// runPhiCondSelRelocate — move PHICondSel instrs to pred of parent
// ============================================================

void MPCHierarchicalPass::runPhiCondSelRelocate(SmallVector<Value*>& PHICondSelIntrs, DominatorTree* DT) {
    for (Value* val : PHICondSelIntrs) {
        if (Instruction* I = dyn_cast<Instruction>(val)) {
            BasicBlock* parent = I->getParent();
            BasicBlock* moveTo = nullptr;
            for (auto Pred : predecessors(parent)) {
                auto itr          = std::find(newBBs.begin(), newBBs.end(), Pred);
                BasicBlock* check = Pred;
                if (itr != newBBs.end()) {
                    int idx = itr - newBBs.begin();
                    check   = oldBBs[idx];
                }
                if (!DT->dominates(parent, check))
                    moveTo = Pred;
            }
            if (moveTo)
                I->moveBefore(moveTo->getTerminator());
        }
    }
}

BasicBlock* MPCHierarchicalPass::visit_loop(BasicBlock* BB, SmallVector<BasicBlock*>& oldBBs,
                                            SmallVector<BasicBlock*>& newBBs, BasicBlock* lastVisited,
                                            PostDominatorTree* PDT, DominatorTree* DT, LoopInfo* LI, Loop* L) {
    // skipping loop header, or inner loop exits
    if (BB == lastVisited)
        return lastVisited;

    if (oldBBs.size() > 0) {
        if (oldBBs[oldBBs.size() - 1] == BB && newBBs[newBBs.size() - 1] == lastVisited)
            return lastVisited;
    }

    /*
      Runs after the loop simplify and unify exits pass, 
      thus only one latch, only one exit                             
    */

    Value* trueCond = trueValue;
    if (L)
        trueCond = public_pc[L->getHeader()];
    auto terminator = lastVisited->getTerminator();

    if (public_pc[BB] == trueCond) {
        auto newTerminator = BranchInst::Create(BB);
        ReplaceInstWithInst(terminator, newTerminator);
        if (LI->isLoopHeader(BB))
            return LI->getLoopFor(BB)->getExitBlock();
        return BB;
    }

    Function* F = BB->getParent();

    BasicBlock* newBB = BasicBlock::Create(F->getContext(), "", F);
    auto newTerm      = BranchInst::Create(BB, newBB, public_pc[BB]);
    ReplaceInstWithInst(lastVisited->getTerminator(), newTerm);

    newTerm = BranchInst::Create(newBB);
    // duplicate terminator from oldBB to newBB
    auto oldTerm = BB->getTerminator();
    if (LI->isLoopHeader(BB)) {
        oldTerm = LI->getLoopFor(BB)->getExitBlock()->getTerminator();
    }
    BranchInst* bTerm = dyn_cast<BranchInst>(oldTerm);
    if (bTerm->isConditional())
        BranchInst::Create(oldTerm->getSuccessor(0), oldTerm->getSuccessor(1), oldTerm->getOperand(0), newBB);
    else
        BranchInst::Create(oldTerm->getSuccessor(0), newBB);

    ReplaceInstWithInst(oldTerm, newTerm);

    newBBs.push_back(newBB);
    if (LI->isLoopHeader(BB))
        BB = LI->getLoopFor(BB)->getExitBlock();
    oldBBs.push_back(BB);

    for (auto& I : *BB) {
        // bcz we will move these later
        if (public_pc[BB] == &I || private_pc[BB] == &I)
            continue;

        SmallVector<Instruction*> updateInstrs;
        for (auto User : I.users())
            if (auto UserInst = dyn_cast<Instruction>(User)) {
                auto x = UserInst->getParent();
                if (BB != x || BB->getTerminator() == UserInst)
                    updateInstrs.push_back(UserInst);
            }

        PHINode* phi = nullptr;
        for (Instruction* User : updateInstrs) {
            if (phi == nullptr)
                phi = createPhi(I, BB, lastVisited, newBB);
            replaceOperand(User, &I, phi);
        }
        for (auto itr : public_pc)
            if (itr.second == &I) {
                if (phi == nullptr)
                    phi = createPhi(I, BB, lastVisited, newBB);
                public_pc[itr.first] = phi;
            }
        for (auto itr : private_pc)
            if (itr.second == &I) {
                if (phi == nullptr)
                    phi = createPhi(I, BB, lastVisited, newBB);
                private_pc[itr.first] = phi;
            }
    }
    if(L){
        L->addBasicBlockToLoop(newBB, *LI);
    }

    return newBB;
}

// ============================================================
// transformLoop — updatePhi + updateStore + CFG visit, innermost-first.
// ============================================================

void MPCHierarchicalPass::transformLoop(Loop* L, DominatorTree* DT, PostDominatorTree* PDT) {
    SmallVector<BasicBlock*> order;
    topologicalOrderForLoop(L, order);

    SmallVector<Value*> PHICondSelIntrs_L;
    for (auto itr = order.rbegin(); itr != order.rend(); ++itr) {
        BasicBlock* BB = *itr;
        updatePhi(BB, PDT, DT, order, PHICondSelIntrs_L, LI);
        updateSecretStoreInst(*BB, private_pc[BB]);
    }

    // Stash the per-loop PHICondSelIntrs so the global relocation pass sees them.
    for (Value* v : PHICondSelIntrs_L)
        globalPHICondSelIntrs.push_back(v);

    if (order.size() <= 1)
        return;

    BasicBlock* lastVisited = L->getHeader();

    for (auto itr = order.rbegin(); itr != order.rend(); ++itr) {
        BasicBlock* BB = *itr;
        lastVisited    = visit_loop(BB, oldBBs, newBBs, lastVisited, PDT, DT, LI, L);
    }

    for (auto itr = order.rbegin(); itr != order.rend(); ++itr)
        if (LI->isLoopHeader(*itr))
            updatePhiLoopHeader(*itr, oldBBs, newBBs);
}

// ============================================================
// runImpl
// ============================================================

void MPCHierarchicalPass::runImpl(Function& F, LoopInfo* LI_, DominatorTree* DT, PostDominatorTree* PDT) {
    this->LI = LI_;
    phiMap.clear();
    newBBs.clear();
    oldBBs.clear();
    globalPHICondSelIntrs.clear();

    // Step 1: collect all loops in post-order (innermost first)
    SmallVector<Loop*> allLoops;
    std::function<void(Loop*)> collectPostOrder = [&](Loop* L) {
        for (Loop* SubL : L->getSubLoops())
            collectPostOrder(SubL);
        allLoops.push_back(L);
    };
    for (Loop* TL : *LI)
        collectPostOrder(TL);

    // Step 2: run updatePhi + updateStore + CFG visit on each loop's direct
    //         blocks, innermost-first.
    for (Loop* L : allLoops)
        transformLoop(L, DT, PDT);

    // Step 3: global topological order (same as MPCTransformPass)
    SmallVector<BasicBlock*> order;
    topologicalOrderFunctionLevel(F, order, LI);

    // Step 4: updatePhi + updateStore for blocks outside any loop
    for (auto itr = order.rbegin(); itr != order.rend(); ++itr) {
        BasicBlock* BB = *itr;
        Loop* L = LI->getLoopFor(BB);
        if (L == nullptr) {
            updatePhi(BB, PDT, DT, order, globalPHICondSelIntrs, LI);
            updateSecretStoreInst(*BB, private_pc[BB]);
        }
    }

    // Step 5: CFG visit for function-level blocks (loop headers collapsed)
    BasicBlock* lastVisited = nullptr;
    for (auto itr = order.rbegin(); itr != order.rend(); ++itr) {
        BasicBlock *BB = *itr;
        if(lastVisited == nullptr)
          lastVisited = BB;
        lastVisited = visit_loop(BB, oldBBs, newBBs, lastVisited, PDT, DT, LI);
    }

    // Step 6: relocate PHI-condition select instructions
    runPhiCondSelRelocate(globalPHICondSelIntrs, DT);

    // Step 7: fix loop header PHIs
    for (auto itr = order.rbegin(); itr != order.rend(); ++itr)
        if (LI->isLoopHeader(*itr))
            updatePhiLoopHeader(*itr, oldBBs, newBBs);
}

// ============================================================
// run — pass entry point
// ============================================================

llvm::PreservedAnalyses MPCHierarchicalPass::run(llvm::Function& F, llvm::FunctionAnalysisManager& FAM) {
    bool hasSecShared = false;
    for (auto& BB : F) {
        Instruction* I = BB.getTerminator();
        if (isa<ReturnInst>(I))
            continue;
        if (isa<BranchInst>(I) && I->hasMetadata("secret_shared")) {
            hasSecShared = true;
            break;
        }
    }
    if (!hasSecShared)
        return PreservedAnalyses::all();

    if (!removePanics(F))
        return PreservedAnalyses::all();

    auto& LI  = FAM.getResult<LoopAnalysis>(F);
    auto& DT  = FAM.getResult<DominatorTreeAnalysis>(F);
    auto& PDT = FAM.getResult<PostDominatorTreeAnalysis>(F);

    LLVMContext& ctx = F.getContext();
    this->context    = &ctx;
    trueValue        = ConstantInt::getTrue(*(this->context));

    this->pc = new PathCondition();
    pc->getConditions(F);

    getConditionVals(F);

    hasSecShared = false;
    for (auto itr : private_pc) {
        if (itr.second != trueValue) {
            hasSecShared = true;
            break;
        }
    }

    if (!hasSecShared) {
        private_pc.clear();
        public_pc.clear();
        conditionValMap.clear();
        delete pc;
        return PreservedAnalyses::all();
    }

    runImpl(F, &LI, &DT, &PDT);


    private_pc.clear();
    public_pc.clear();
    conditionValMap.clear();
    phiMap.clear();
    newBBs.clear();
    oldBBs.clear();
    delete pc;
    return PreservedAnalyses::none();
}
