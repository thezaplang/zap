#pragma once
#include "basic_block.hpp"
#include "type.hpp"
#include <memory>
#include <string>
#include <vector>

namespace zir {

class Function {
public:
  std::string name;
  std::shared_ptr<Type> returnType;
  std::string ownerTypeCodegenName;
  bool isDestructor = false;
  bool hasExtensionReceiver = false;
  bool isCVariadic = false;
  bool returnsRef = false;
  ResultBorrowContract resultBorrow;
  int vtableSlot = -1;
  std::vector<std::shared_ptr<Argument>> arguments;
  std::vector<std::unique_ptr<BasicBlock>> blocks;

  Function(std::string name, std::shared_ptr<Type> returnType,
           std::string ownerTypeCodegenName = "", bool isDestructor = false,
           int vtableSlot = -1, bool isCVariadic = false,
           bool hasExtensionReceiver = false)
      : name(std::move(name)), returnType(std::move(returnType)),
        ownerTypeCodegenName(std::move(ownerTypeCodegenName)),
        isDestructor(isDestructor),
        hasExtensionReceiver(hasExtensionReceiver), isCVariadic(isCVariadic),
        vtableSlot(vtableSlot) {}

  void addBlock(std::unique_ptr<BasicBlock> block) {
    blocks.push_back(std::move(block));
  }

  const std::shared_ptr<Type> &getReturnType() const { return returnType; }

  const std::vector<std::shared_ptr<Argument>> &getArguments() const {
    return arguments;
  }

  const std::vector<std::unique_ptr<BasicBlock>> &getBlocks() const {
    return blocks;
  }

  BasicBlock *findBlock(const std::string &label) const {
    for (const auto &block : blocks) {
      if (block->label == label) {
        return block.get();
      }
    }
    return nullptr;
  }

  std::string toString() const {
    std::string res = "@" + name + "(";
    for (size_t i = 0; i < arguments.size(); ++i) {
      if (containsManagedValues(arguments[i]->getType())) {
        switch (arguments[i]->getParameterOwnership()) {
        case ParameterOwnership::Borrow:
          res += "borrow ";
          break;
        case ParameterOwnership::Transfer:
          res += "transfer ";
          break;
        case ParameterOwnership::Sink:
          res += "sink ";
          break;
        }
      }
      if (arguments[i]->getParameterEscape() == ParameterEscape::NoEscape) {
        res += "noescape ";
      }
      res += arguments[i]->getTypeName() + " " + arguments[i]->getName();
      if (i < arguments.size() - 1)
        res += ", ";
    }
    res += ") " + returnType->toString();
    if (returnsRef)
      res += "*";
    if (resultBorrow.hasSource()) {
      res += " borrows(" +
             std::to_string(*resultBorrow.sourceParameter()) + ")";
    }
    res += " {\n";
    for (const auto &block : blocks) {
      res += block->toString();
    }
    res += "}\n";
    return res;
  }
};

} // namespace zir
