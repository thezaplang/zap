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
  std::string ownerTypeName;
  bool isDestructor = false;
  bool isCVariadic = false;
  int vtableSlot = -1;
  std::vector<std::shared_ptr<Argument>> arguments;
  std::vector<std::unique_ptr<BasicBlock>> blocks;

  Function(std::string name, std::shared_ptr<Type> returnType,
           std::string ownerTypeName = "", bool isDestructor = false,
           int vtableSlot = -1, bool isCVariadic = false)
      : name(std::move(name)), returnType(std::move(returnType)),
        ownerTypeName(std::move(ownerTypeName)),
        isDestructor(isDestructor), isCVariadic(isCVariadic),
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
      res += arguments[i]->getTypeName() + " " + arguments[i]->getName();
      if (i < arguments.size() - 1)
        res += ", ";
    }
    res += ") " + returnType->toString() + " {\n";
    for (const auto &block : blocks) {
      res += block->toString();
    }
    res += "}\n";
    return res;
  }
};

} // namespace zir
