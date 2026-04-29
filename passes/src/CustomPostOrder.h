#pragma once
#include "llvm/ADT/PostOrderIterator.h"

class custom_po_iterator
    : public po_iterator_storage<
          SmallPtrSet<typename GraphTraits<BasicBlock *>::NodeRef, 8>, false> {
public:
  using GT = GraphTraits<BasicBlock *>;
  using iterator_category = std::forward_iterator_tag;
  using value_type = typename GT::NodeRef;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type *;
  using reference = const value_type &;

private:
  using NodeRef = typename GT::NodeRef;
  using ChildItTy = typename GT::ChildIteratorType;

  /// Used to maintain the ordering.
  /// First element is basic block pointer, second is iterator for the next
  /// child to visit, third is the end iterator.
  SmallVector<std::tuple<NodeRef, ChildItTy, ChildItTy>, 8> VisitStack;

  custom_po_iterator(NodeRef BB, BasicBlock *exit = nullptr) {
    this->insertEdge(std::optional<NodeRef>(), BB);
    if (BB == exit)
      return;
    VisitStack.emplace_back(BB, GT::child_begin(BB), GT::child_end(BB));
    traverseChild(exit);
  }

  custom_po_iterator() = default; // End is when stack is empty.

  void traverseChild(BasicBlock *exit = nullptr) {
    while (true) {
      auto &Entry = VisitStack.back();
      if (std::get<1>(Entry) == std::get<2>(Entry))
        break;
      NodeRef BB = *std::get<1>(Entry)++;
      //   if (BB == exit) {
      //     break;
      //   }
      if (this->insertEdge(std::optional<NodeRef>(std::get<0>(Entry)), BB)) {
        // If the block is not visited...
        if (BB != exit)
          VisitStack.emplace_back(BB, GT::child_begin(BB), GT::child_end(BB));
      }
    }
  }

public:
  // Provide static "constructors"...
  static custom_po_iterator begin(BasicBlock *G, BasicBlock *exit = nullptr) {
    return custom_po_iterator(GT::getEntryNode(G), exit);
  }

  static custom_po_iterator end(BasicBlock *G) { return custom_po_iterator(); }

  bool operator==(const custom_po_iterator &x) const {
    return VisitStack == x.VisitStack;
  }
  bool operator!=(const custom_po_iterator &x) const { return !(*this == x); }

  reference operator*() const { return std::get<0>(VisitStack.back()); }

  // This is a nonstandard operator-> that dereferences the pointer an extra
  // time... so that you can actually call methods ON the BasicBlock, because
  // the contained type is a pointer.  This allows BBIt->getTerminator() f.e.
  //
  NodeRef operator->() const { return **this; }

  custom_po_iterator &operator++() { // Preincrement
    this->finishPostorder(std::get<0>(VisitStack.back()));
    VisitStack.pop_back();
    if (!VisitStack.empty())
      traverseChild();
    return *this;
  }

  custom_po_iterator operator++(int) { // Postincrement
    custom_po_iterator tmp = *this;
    ++*this;
    return tmp;
  }
};

class loop_po_iterator
    : public po_iterator_storage<
          SmallPtrSet<typename GraphTraits<BasicBlock *>::NodeRef, 8>, false> {
public:
  using GT = GraphTraits<BasicBlock *>;
  using iterator_category = std::forward_iterator_tag;
  using value_type = BasicBlock *;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type *;
  using reference = const value_type &;

private:
  using NodeRef =   BasicBlock *;
  using ChildItTy = BasicBlock **;

  LoopInfo *LI;
  SmallVector<BasicBlock *> exitBlocks;
  /// Used to maintain the ordering.
  /// First element is basic block pointer, second is iterator for the next
  /// child to visit, third is the end iterator.
  SmallVector<std::tuple<NodeRef, ChildItTy, ChildItTy>, 8> VisitStack;

  loop_po_iterator(NodeRef BB, LoopInfo *LI, Loop *L = nullptr) {
    this->insertEdge(std::optional<NodeRef>(), BB);
    exitBlocks.clear();
    if(L)
      L->getExitBlocks(exitBlocks);
    SmallVector<BasicBlock *> * children = new SmallVector<BasicBlock *>(0, nullptr);

    for(auto succ: successors(BB)){
      children->push_back(succ);
    }
    VisitStack.emplace_back(BB, children->begin(), children->end());
    this->LI = LI;
    traverseChild();
  }

  loop_po_iterator() = default; // End is when stack is empty.

  void traverseChild() {
    while (true) {
      auto &Entry = VisitStack.back();
      if (std::get<1>(Entry) == std::get<2>(Entry))
        break;

      NodeRef BB = *std::get<1>(Entry)++;

      // If the child is outside the loop skip it
      if (std::find(exitBlocks.begin(), exitBlocks.end(), BB) !=
          exitBlocks.end())
        continue;

      if (this->insertEdge(std::optional<NodeRef>(std::get<0>(Entry)), BB)) {
        // If the block is not visited...
        // is loop header then at directly go to the exits, i.e., add loop exits from BB
        if (LI->isLoopHeader(BB)) {
          Loop *innerLoop = LI->getLoopFor(BB);
          SmallVector<BasicBlock *> * tmp = new SmallVector<BasicBlock *>(0, nullptr);
          innerLoop->getExitBlocks(*tmp);
          VisitStack.emplace_back(BB, tmp->begin(), tmp->end());
        } else {
          SmallVector<BasicBlock *> * tmp = new SmallVector<BasicBlock *>(0, nullptr);
          for(auto child: successors(BB)){
            tmp->push_back(child);
          }
          VisitStack.emplace_back(BB, tmp->begin(), tmp->end());
        }
      }
    }
  }

public:
  // Provide static "constructors"...
  static loop_po_iterator begin(BasicBlock *BB, LoopInfo &LI, Loop *L = nullptr) {
    return loop_po_iterator(BB, &LI, L);
  }

  static loop_po_iterator end(BasicBlock *G) { return loop_po_iterator(); }

  bool operator==(const loop_po_iterator &x) const {
    return VisitStack == x.VisitStack;
  }
  bool operator!=(const loop_po_iterator &x) const { return !(*this == x); }

  reference operator*() const { return std::get<0>(VisitStack.back()); }

  // This is a nonstandard operator-> that dereferences the pointer an extra
  // time... so that you can actually call methods ON the BasicBlock, because
  // the contained type is a pointer.  This allows BBIt->getTerminator() f.e.
  //
  NodeRef operator->() const { return **this; }

  loop_po_iterator &operator++() { // Preincrement
    this->finishPostorder(std::get<0>(VisitStack.back()));
    VisitStack.pop_back();
    if (!VisitStack.empty())
      traverseChild();
    return *this;
  }

  loop_po_iterator operator++(int) { // Postincrement
    loop_po_iterator tmp = *this;
    ++*this;
    return tmp;
  }
};
