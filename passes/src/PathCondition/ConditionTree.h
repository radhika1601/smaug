#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;


enum ConditionOP { AND = 0, OR = 1, NOT = 2, EMPTY = 3 };

class ConditionTree {
public:
  Value *val = nullptr;
  ConditionOP op = ConditionOP::EMPTY;
  llvm::SmallVector<ConditionTree *> children;

  ConditionTree(Value *val) : val(val) {}

  ConditionTree(Value *val, ConditionOP op) {
    if (op != ConditionOP::NOT) {
      LLVM_DEBUG(dbgs() << "No constructor for ConditionTree with one val "
                           "and non-NOT operand\n");
      exit(1);
    }
    this->op = op;
    auto c = new ConditionTree(val);
    children.push_back(c);
  }

  ConditionTree(Value *left, Value *right, ConditionOP op) {
    if (op == ConditionOP::NOT) {
      LLVM_DEBUG(dbgs() << "NOT operator, 2 operands\n");
      exit(1);
    }
    this->op = op;
    auto c = new ConditionTree(left);
    children.push_back(c);

    c = new ConditionTree(right);
    children.push_back(c);
  }

  ConditionTree(llvm::SmallVector<ConditionTree *> &children, ConditionOP op)
      : op(op), children(children) {}

  std::string getShortValueName(Value *v) {
    if (v->getName().str().length() > 0) {
      return "%" + v->getName().str();
    } else if (isa<Instruction>(v)) {
      std::string s = "";
      raw_string_ostream *strm = new raw_string_ostream(s);
      v->print(*strm);
      std::string inst = strm->str();
      size_t idx1 = inst.find("%");
      size_t idx2 = inst.find(" ", idx1);
      if (idx1 != std::string::npos && idx2 != std::string::npos) {
        return inst.substr(idx1, idx2 - idx1);
      } else {
        return "\"" + inst + "\"";
      }
    } else if (ConstantInt *cint = dyn_cast<ConstantInt>(v)) {
      std::string s = "";
      raw_string_ostream *strm = new raw_string_ostream(s);
      cint->getValue().print(*strm, true);
      return strm->str();
    } else {
      std::string s = "";
      raw_string_ostream *strm = new raw_string_ostream(s);
      v->print(*strm);
      std::string inst = strm->str();
      return "\"" + inst + "\"";
    }
  }

  void print(std::string prefix = "", bool isLeft = true) {
    (errs() << prefix);
    (errs() << (isLeft ? "├──" : "└──"));
    if (val != nullptr) {
      (errs() << formatv("{0}\n", getShortValueName(this->val)));

      return;
    }
    if (this->op == ConditionOP::AND) {
      (errs() << "AND");
    } else if (this->op == ConditionOP::OR) {
      (errs() << "OR");

    } else if (this->op == ConditionOP::NOT) {
      (errs() << "NOT");
    }
    (errs() << "\n");

    for (auto child : children) {
      child->print(prefix + "|   ");
    }
  }
};
