#pragma once
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"

#include "ConditionTree.h"
#include <map>
using namespace llvm;

class PathCondition {
public:
  std::map<BasicBlock *, ConditionTree *> conditions;
  MapVector<Value *, ConditionTree *> singleNodeConditions;
  MapVector<Value *, ConditionTree *> negSingleNodeConditions;
  SmallVector<Value *> private_cond;
  ConditionTree *trueTree, *falseTree;
  PostDominatorTree *PDT;
  DominatorTree *DT;
  ~PathCondition() {
    conditions.clear();
    singleNodeConditions.clear();
    negSingleNodeConditions.clear();
    private_cond.clear();
  }
  void getConditions(Function &F) {
    LLVM_DEBUG(F.printAsOperand(dbgs()));
    auto dom_tree = DominatorTree(F);
    auto post_dom_tree = PostDominatorTree(F);
    BasicBlock *root = dom_tree.getRoot();
    DT = &dom_tree;
    PDT = &post_dom_tree;


    SmallVector<BasicBlock *> bb_queue;
    // bb_queue.push_back(root);
    LLVMContext &context = F.getContext();

    auto trueValue = ConstantInt::getTrue(context);
    auto falseValue = ConstantInt::getFalse(context);
    trueTree = new ConditionTree(trueValue);
    falseTree = new ConditionTree(falseValue);
    conditions.insert(std::make_pair(root, trueTree));
    conditions[root] = trueTree;
    dom_tree.getDescendants(root, bb_queue);

    auto itr = bb_queue.begin();

    while (!bb_queue.empty()) {
      if (itr == bb_queue.end()) {
        itr = bb_queue.begin();
      }

      BasicBlock *BB = *itr;
      bool visited = visit(BB, &dom_tree, &post_dom_tree);
      if (!visited) {
        itr++;
        continue;
      } else {
        bb_queue.erase(itr);
        push_descendants(BB, bb_queue, &dom_tree);
      }
    }

    for (auto &cond : conditions) {
      LLVM_DEBUG(dbgs() << "NAME: ");
      LLVM_DEBUG(cond.first->printAsOperand(dbgs(), false));
      LLVM_DEBUG(dbgs() << "\n");
      if (cond.second == nullptr) {
        LLVM_DEBUG(dbgs() << "true\n");
      }
      LLVM_DEBUG(dbgs() << "\n\n");
    }

    return;
  }

  void push_descendants(BasicBlock *BB, SmallVector<BasicBlock *> &bb_queue,
                        DominatorTree *dom_tree) {
    llvm::SmallVector<llvm::BasicBlock *> descendants;
    dom_tree->getDescendants(BB, descendants);
    for (auto itr = descendants.begin(), end = descendants.end(); itr != end;
         ++itr) {
      if (dom_tree->dominates(*itr, BB))
        continue;
      else if (conditions.find(*itr) == conditions.end()) {
        if (std::find(bb_queue.begin(), bb_queue.end(), *itr) ==
            bb_queue.end()) {
          bb_queue.push_back(*itr);
        }
      }
    }
  }

  bool visit(BasicBlock *BB, DominatorTree *dom_tree,
             PostDominatorTree *post_dom_tree) {
    SmallVector<BasicBlock *> preds;
    for (auto itr = pred_begin(BB), end = pred_end(BB); itr != end; ++itr) {
      if (dom_tree->dominates(BB, *itr))
        continue;
      else if (conditions.find(*itr) == conditions.end()) {
        return false;
      } else {
        preds.push_back(*itr);
      }
    }

    SmallVector<BasicBlock *> postDominates, unnecessary;
    post_dom_tree->getDescendants(BB, postDominates);
    auto itr = std::find(postDominates.begin(), postDominates.end(), BB);
    postDominates.erase(itr);

    for (auto x : preds) {
      for (auto y : postDominates) {
        if (x != y) {
          if (dom_tree->dominates(y, x)) {
            unnecessary.push_back(x);
            break;
          }
        }
      }
    }

    for (auto x : unnecessary) {
      auto itr = std::find(preds.begin(), preds.end(), x);
      if (itr != preds.end()) {
        preds.erase(itr);
      }
    }

    for (auto x : postDominates) {
      auto itr = std::find(preds.begin(), preds.end(), x);
      if (itr != preds.end()) {
        preds.erase(itr);
      }
    }

    unnecessary.resize(0);
    for (auto x : postDominates) {
      for (auto y : postDominates) {
        if (x != y) {
          if (dom_tree->dominates(y, x)) {
            unnecessary.push_back(x);
          }
        }
      }
    }

    for (auto x : unnecessary) {
      auto *itr = std::find(postDominates.begin(), postDominates.end(), x);
      if (itr != postDominates.end()) {
        postDominates.erase(itr);
      }
    }

    ConditionTree *tree = nullptr;
    llvm::SmallDenseSet<ConditionTree *> children;
    for (auto itr = preds.begin(), end = preds.end(); itr != end; ++itr) {
      BasicBlock *predecessor = *itr;
      if (std::find(postDominates.begin(), postDominates.end(), predecessor) !=
          postDominates.end())
        continue;
      Instruction *last_inst = &(predecessor->back());

      ConditionTree *left = nullptr;
      ConditionTree *right = conditions[predecessor];
      if (isa<BranchInst>(last_inst)) {
        BranchInst *inst = dyn_cast<BranchInst>(last_inst);
        if (inst->isConditional()) {
          Value *cond = inst->getCondition();

          if (inst->hasMetadata("secret_shared")) {
            if (std::find(private_cond.begin(), private_cond.end(), cond) ==
                private_cond.end()) {
              private_cond.push_back(cond);
            }
          }

          if (BB == inst->getSuccessor(0)) {
            if (singleNodeConditions.find(cond) != singleNodeConditions.end())
              left = singleNodeConditions[cond];
            else {
              left = new ConditionTree(cond);
              singleNodeConditions.insert(std::make_pair(cond, left));
            }
          } else {
            if (negSingleNodeConditions.find(cond) !=
                negSingleNodeConditions.end())
              left = negSingleNodeConditions[cond];
            else {
              left = new ConditionTree(cond, ConditionOP::NOT);
              negSingleNodeConditions.insert(std::make_pair(cond, left));
            }
          }
        } else {
          left = trueTree;
        }
      }
      ConditionTree *pred_tree = getAndConditionTree(left, right);
      children.insert(pred_tree);
    }
    for (auto x : postDominates) {
      if (x == BB)
        continue;
      children.insert(conditions[x]);
    }

    tree = getOrConditionTree(children);

    if (tree == nullptr)
      tree = trueTree;
    conditions.insert(std::make_pair(BB, tree));
    conditions[BB] = tree;

    children.clear();
    return true;
  }

  ConditionTree *
  getOrConditionTree(llvm::SmallDenseSet<ConditionTree *> &children) {
    if (children.size() == 0)
      return trueTree;
    if (children.size() == 1)
      return *(children.begin());

    llvm::SmallVector<ConditionTree *> queue;
    for (auto child : children) {
      queue.push_back(child);
    }

    children.clear();
    MapVector<ConditionTree *, bool> removeChildren;

    auto itr = queue.begin();
    while (!queue.empty()) {
      if (itr >= queue.end()) {
        itr = queue.begin();
      }
      ConditionTree *curr = *itr;
      itr = queue.erase(itr);
      if (curr->op == ConditionOP::OR) {
        for (auto child : curr->children) {
          if (std::find(queue.begin(), queue.end(), child) == queue.end())
            if (children.find(child) == children.end())
              queue.push_back(child);
        }
      } else {
        if (std::find(children.begin(), children.end(), curr) ==
            children.end()) {
          children.insert(curr);
          removeChildren.insert({curr, false});
        }
      }
    }

    for (auto A : children) {
      if (removeChildren[A])
        continue;
      if (A == trueTree)
        return trueTree;
      if (A == falseTree)
        removeChildren[A] = true;
      for (auto B : children) {
        if (A == B)
          continue;
        if (removeChildren[A] || removeChildren[B])
          continue;
        // A || (!A) = true
        if (B->op == ConditionOP::NOT) {
          if (A == B->children[0]) {
            return trueTree;
          }
        } else {
          // A || (A & something) = A
          // A || (A || something) = (A || something)
          if (std::find(B->children.begin(), B->children.end(), A) !=
              B->children.end()) {
            if (B->op == ConditionOP::AND) {
              removeChildren[B] = true;
            } else if (B->op == ConditionOP::OR) {
              removeChildren[A] = true;
            }
          } else {
            // (A & B) || (A & C) = A & (B || C)
            // (A & B) && (A & C) = A & (B & C)
          }
        }
      }
    }

    SmallVector<ConditionTree *> newChildren(0);
    for (auto child : children) {
      if (!removeChildren[child])
        newChildren.push_back(child);
    }

    if (newChildren.size() == 0) {
      return falseTree;
    } else if (newChildren.size() == 1) {
      return newChildren[0];
    }

    auto tree = new ConditionTree(newChildren, ConditionOP::OR);

    return tree;
  }

  // Not simplifiying because could lead to potential inconsistencies even when
  // it is the same tree the pointers can be unequal
  ConditionTree *getAndConditionTree(ConditionTree *left,
                                     ConditionTree *right) {
    if (left == right)
      return left;
    if (left == trueTree || left == nullptr)
      return right;
    if (right == trueTree || right == nullptr)
      return left;
    if (left == falseTree || right == falseTree)
      return falseTree;

    llvm::SmallVector<ConditionTree *> children;
    if (std::find(right->children.begin(), right->children.end(), left) !=
        right->children.end()) {

      // left && (left || something)
      if (right->op == ConditionOP::OR)
        return left;

      // left && (left && something)
      if (right->op == ConditionOP::AND)
        return right;
    }

    if (std::find(left->children.begin(), left->children.end(), right) !=
        left->children.end()) {

      // (right || something) && right
      if (left->op == ConditionOP::OR)
        return right;

      // (right && something) && right
      if (left->op == ConditionOP::AND)
        return left;
    }

    children.push_back(left);
    children.push_back(right);
    auto ret = new ConditionTree(children, AND);
    return ret;
  }
};
