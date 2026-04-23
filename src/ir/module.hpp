#pragma once
#include "function.hpp"
#include <memory>
#include <string>
#include <vector>

namespace zir
{

  class Module
  {
  public:
    std::string name;
    std::vector<std::shared_ptr<Type>> types;
    std::vector<std::shared_ptr<Global>> globals;
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::unique_ptr<Function>> externalFunctions;

    Module(std::string name) : name(std::move(name)) {}

    void addType(std::shared_ptr<Type> type) { types.push_back(std::move(type)); }

    void addFunction(std::unique_ptr<Function> func)
    {
      functions.push_back(std::move(func));
    }

    void addGlobal(std::shared_ptr<Global> global)
    {
      globals.push_back(std::move(global));
    }

    void addExternalFunction(std::unique_ptr<Function> func)
    {
      externalFunctions.push_back(std::move(func));
    }

    const std::vector<std::shared_ptr<Type>> &getTypes() const { return types; }

    const std::vector<std::shared_ptr<Global>> &getGlobals() const {
      return globals;
    }

    const std::vector<std::unique_ptr<Function>> &getFunctions() const {
      return functions;
    }

    const std::vector<std::unique_ptr<Function>> &getExternalFunctions() const {
      return externalFunctions;
    }

    Function *findFunction(const std::string &functionName) const
    {
      for (const auto &func : functions)
      {
        if (func->name == functionName)
        {
          return func.get();
        }
      }
      for (const auto &func : externalFunctions)
      {
        if (func->name == functionName)
        {
          return func.get();
        }
      }
      return nullptr;
    }

    std::string toString() const
    {
      std::string res = "; Module: " + name + "\n";
      for (const auto &type : types)
      {
        if (type->getKind() == TypeKind::Record)
        {
          auto rt = std::static_pointer_cast<RecordType>(type);
          res += rt->toString() + " = type { ";
          const auto &fields = rt->getFields();
          for (size_t i = 0; i < fields.size(); ++i)
          {
            res +=
                fields[i].type->toString() + (i < fields.size() - 1 ? ", " : "");
          }
          res += " }\n";
        }
        else if (type->getKind() == TypeKind::Enum)
        {
          auto et = std::static_pointer_cast<EnumType>(type);
          res += et->toString() + " { ";
          const auto &variants = et->getVariants();
          for (size_t i = 0; i < variants.size(); ++i)
          {
            res += variants[i] + (i < variants.size() - 1 ? ", " : "");
          }
          res += " }\n";
        }
      }
      res += "\n";
      if (!globals.empty())
      {
        res += "; Globals\n";
        for (const auto &global : globals)
        {
          res += global->getName() + " = global " +
                 global->getValueType()->toString();
          if (global->getInitializer())
          {
            res += " " + global->getInitializer()->getName();
          }
          res += "\n";
        }
        res += "\n";
      }
      res += "; External Functions\n";
      for (const auto &func : externalFunctions)
      {
        res += "extern " + func->toString() + "\n";
      }
      res += "\n";
      for (const auto &func : functions)
      {
        res += func->toString() + "\n";
      }
      return res;
    }
  };

} // namespace zir
