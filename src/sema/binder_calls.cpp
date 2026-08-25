#include "binder.hpp"
#include "type_layout.hpp"
#include <algorithm>
#include <unordered_map>

namespace sema {

void Binder::visit(FunCall &node) {
  if (bindSizeOfBuiltinCall(node)) {
    return;
  }

  if (bindWeakBuiltinCall(node)) {
    return;
  }

  auto bindIndirectArguments =
      [&](const zir::FunctionPointerType &functionType,
          std::vector<std::unique_ptr<BoundExpression>> &arguments) {
        if (node.params_.size() != functionType.getParams().size()) {
          error(node.span, "Function pointer call argument count mismatch.");
          return false;
        }
        arguments.reserve(node.params_.size());
        for (size_t i = 0; i < node.params_.size(); ++i) {
          const auto &expectedType = functionType.getParams()[i];
          auto argument = bindExpressionWithExpected(
              node.params_[i]->value.get(), expectedType);
          if (!argument) {
            return false;
          }
          auto conversion =
              conversions_.classifyImplicit(argument->type, expectedType);
          if (!conversion) {
            error(node.params_[i]->value->span,
                  "Function pointer argument is not convertible from '" +
                      renderTypeForUser(argument->type) + "' to '" +
                      renderTypeForUser(expectedType) + "'.");
            return false;
          }
          arguments.push_back(
              applyConversion(std::move(argument), *conversion));
        }
        return true;
      };

  if (auto member = dynamic_cast<MemberAccessNode *>(node.callee_.get())) {
    member->left_->accept(*this);
    if (expressionStack_.empty()) {
      return;
    }
    auto selfExpr = std::move(expressionStack_.top());
    expressionStack_.pop();
    if (selfExpr->type->getKind() == zir::TypeKind::TaggedUnion &&
        dynamic_cast<BoundLiteral *>(selfExpr.get())) {
      auto taggedUnionType =
          std::static_pointer_cast<zir::TaggedUnionType>(selfExpr->type);
      auto variant = taggedUnionType->findVariant(member->member_);
      if (!variant) {
        error(node.span, "Enum '" + taggedUnionType->getName() +
                             "' has no variant '" + member->member_ + "'.");
        return;
      }

      if (variant->payloadType) {
        if (node.params_.size() != 1) {
          error(node.span, "Enum variant '" + member->member_ +
                               "' expects one payload argument.");
          return;
        }
        if (!node.params_[0]->name.empty() || node.params_[0]->isRef ||
            node.params_[0]->isSpread) {
          error(node.params_[0]->value->span,
                "Enum payload arguments must be positional values.");
          return;
        }
        auto payload = bindExpressionWithExpected(node.params_[0]->value.get(),
                                                  variant->payloadType);
        if (!payload) {
          return;
        }
        auto conversion =
            conversions_.classifyImplicit(payload->type, variant->payloadType);
        if (!conversion) {
          error(node.params_[0]->value->span,
                "Cannot convert enum payload from '" +
                    renderTypeForUser(payload->type) + "' to '" +
                    renderTypeForUser(variant->payloadType) + "'");
          return;
        }
        payload = applyConversion(std::move(payload), *conversion);
        expressionStack_.push(std::make_unique<BoundTaggedUnionLiteral>(
            taggedUnionType, variant->name, variant->tag, std::move(payload)));
        return;
      }

      if (!node.params_.empty()) {
        error(node.span, "Enum variant '" + member->member_ +
                             "' does not take a payload argument.");
        return;
      }
      expressionStack_.push(std::make_unique<BoundTaggedUnionLiteral>(
          taggedUnionType, variant->name, variant->tag, nullptr));
      return;
    }

    if (selfExpr->type->getKind() != zir::TypeKind::Class) {
      // Not a class method call. Fall through to the normal qualified
      // function/module call resolution path below.
    } else {
      auto classType = std::static_pointer_cast<zir::ClassType>(selfExpr->type);
      if (classType->isWeak()) {
        error(node.span,
              "Weak references cannot be used to call methods directly.");
        return;
      }
      std::shared_ptr<Symbol> methodSymbol;
      if (classType->isInterface()) {
        auto infoIt = interfaceInfos_.find(classType->getCodegenName());
        if (infoIt == interfaceInfos_.end()) {
          error(node.span, "Unknown interface type: " + classType->getName());
          return;
        }
        auto methodIt = infoIt->second.methods.find(member->member_);
        if (methodIt == infoIt->second.methods.end()) {
          error(node.span, "Interface '" + classType->getName() +
                               "' has no method '" + member->member_ + "'.");
          return;
        }
        methodSymbol = methodIt->second;
      } else {
        auto infoIt = classInfos_.find(classType->getCodegenName());
        if (infoIt == classInfos_.end()) {
          error(node.span, "Unknown class type: " + classType->getName());
          return;
        }
        auto methodIt = infoIt->second.methods.find(member->member_);
        if (methodIt == infoIt->second.methods.end()) {
          error(node.span, "Class '" + classType->getName() +
                               "' has no method '" + member->member_ + "'.");
          return;
        }
        methodSymbol = methodIt->second;
      }
      auto candidates = collectOverloads(methodSymbol);
      if (candidates.empty()) {
        error(node.span, "'" + member->member_ + "' is not a method.");
        return;
      }

      std::vector<std::unique_ptr<BoundExpression>> rawArgs;
      rawArgs.reserve(node.params_.size());
      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg =
            bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
        if (!arg) {
          return;
        }
        if (node.params_[i]->isRef &&
            !requireMutablePlace(*arg, node.params_[i]->value->span,
                                 MutablePlaceUse::MutableReference)) {
          return;
        }
        rawArgs.push_back(std::move(arg));
      }

      struct MethodCandidate {
        std::shared_ptr<FunctionSymbol> symbol;
        std::vector<int> cost;
      };

      const bool calledOnType =
          dynamic_cast<BoundLiteral *>(selfExpr.get()) != nullptr;
      std::vector<MethodCandidate> matches;
      bool inaccessibleMatch = false;
      bool unsafeMatch = false;

      for (auto funcSymbol : candidates) {
        if (!funcSymbol) {
          continue;
        }

        if (funcSymbol->isMethod && calledOnType) {
          continue;
        }

        bool methodAllowed =
            funcSymbol->visibility == Visibility::Public ||
            (!currentClassStack_.empty() &&
             currentClassStack_.back() == classType->getName()) ||
            (funcSymbol->visibility == Visibility::Protected &&
             !currentClassStack_.empty());
        if (!methodAllowed) {
          inaccessibleMatch = true;
          continue;
        }
        if (funcSymbol->isUnsafe && !isUnsafeActive()) {
          unsafeMatch = true;
          continue;
        }

        std::vector<std::unique_ptr<BoundExpression>> inferenceArgs;
        if (funcSymbol->isMethod) {
          inferenceArgs.push_back(selfExpr->clone());
        }
        for (const auto &rawArg : rawArgs) {
          inferenceArgs.push_back(rawArg->clone());
        }

        std::unordered_map<std::string, std::shared_ptr<zir::Type>>
            genericBindings;
        if (!funcSymbol->genericParameterNames.empty()) {
          genericBindings =
              buildGenericBindings(*funcSymbol, inferenceArgs,
                                   node.genericArgs_, node.span, nullptr);
          if (genericBindings.empty()) {
            continue;
          }
          funcSymbol = ensureGenericFunctionInstantiation(
              funcSymbol, orderedGenericBindings(genericBindings), node.span);
          if (!funcSymbol) {
            continue;
          }
        } else if (!node.genericArgs_.empty()) {
          continue;
        }

        size_t paramOffset = funcSymbol->isMethod ? 1 : 0;
        if (node.params_.size() + paramOffset !=
            funcSymbol->parameters.size()) {
          continue;
        }

        MethodCandidate match;
        match.symbol = funcSymbol;
        bool failed = false;
        for (size_t i = 0; i < rawArgs.size(); ++i) {
          auto expectedType = funcSymbol->parameters[i + paramOffset]->type;
          auto conversion =
              conversions_.classifyImplicit(rawArgs[i]->type, expectedType);
          if (!conversion) {
            failed = true;
            break;
          }
          match.cost.push_back(conversion->cost());
        }
        if (!failed) {
          matches.push_back(std::move(match));
        }
      }

      if (matches.empty()) {
        if (inaccessibleMatch) {
          error(node.span,
                "Method '" + member->member_ + "' is not accessible.");
        } else if (unsafeMatch) {
          requireUnsafeContext(node.span, "unsafe function calls");
        } else {
          error(node.span,
                "No matching overload for method '" + member->member_ + "'.");
        }
        return;
      }

      std::sort(matches.begin(), matches.end(),
                [](const MethodCandidate &lhs, const MethodCandidate &rhs) {
                  return lhs.cost < rhs.cost;
                });
      if (matches.size() > 1 && matches[0].cost == matches[1].cost) {
        error(node.span,
              "Ambiguous overload for method '" + member->member_ + "'.");
        return;
      }

      auto funcSymbol = matches.front().symbol;
      std::vector<std::unique_ptr<BoundExpression>> args;
      std::vector<bool> argIsRef;
      if (funcSymbol->isMethod) {
        args.push_back(std::move(selfExpr));
        argIsRef.push_back(false);
      }

      size_t paramOffset = funcSymbol->isMethod ? 1 : 0;
      for (size_t i = 0; i < node.params_.size(); ++i) {
        auto arg = rawArgs[i]->clone();
        auto expectedType = funcSymbol->parameters[i + paramOffset]->type;
        if (auto conversion =
                conversions_.classifyImplicit(arg->type, expectedType)) {
          arg = applyConversion(std::move(arg), *conversion);
        }
        args.push_back(std::move(arg));
        argIsRef.push_back(node.params_[i]->isRef);
      }

      expressionStack_.push(std::make_unique<BoundFunctionCall>(
          funcSymbol, std::move(args), std::move(argIsRef)));
      if (semanticInfo_) {
        semanticInfo_->recordCall(currentModuleId_, node.span, funcSymbol);
      }
      return;
    }
  }

  std::vector<std::string> calleeParts;
  if (!node.callee_ || !extractQualifiedPath(node.callee_.get(), calleeParts)) {
    // Try indirect call through function pointer
    if (node.callee_) {
      node.callee_->accept(*this);
      if (!expressionStack_.empty()) {
        auto calleeExpr = std::move(expressionStack_.top());
        expressionStack_.pop();
        if (calleeExpr->type &&
            calleeExpr->type->getKind() == zir::TypeKind::FunctionPointer) {
          const auto &fpType =
              static_cast<const zir::FunctionPointerType &>(*calleeExpr->type);
          std::vector<std::unique_ptr<BoundExpression>> args;
          if (!bindIndirectArguments(fpType, args)) {
            return;
          }
          expressionStack_.push(std::make_unique<BoundIndirectCall>(
              std::move(calleeExpr), std::move(args), fpType.getReturnType()));
          return;
        }
      }
    }
    error(node.span, "Only direct function calls are supported.");
    return;
  }

  auto symbol =
      resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Function);
  if (!symbol) {
    // Check if it's a variable holding a function pointer
    auto varSym =
        resolveQualifiedSymbol(calleeParts, node.span, SymbolKind::Variable);
    if (varSym && varSym->getKind() == SymbolKind::Variable) {
      auto varSymbol = std::static_pointer_cast<VariableSymbol>(varSym);
      if (varSymbol->type &&
          varSymbol->type->getKind() == zir::TypeKind::FunctionPointer) {
        const auto &fpType =
            static_cast<const zir::FunctionPointerType &>(*varSymbol->type);
        std::vector<std::unique_ptr<BoundExpression>> args;
        if (!bindIndirectArguments(fpType, args)) {
          return;
        }
        auto calleeExpr = std::make_unique<BoundVariableExpression>(varSymbol);
        expressionStack_.push(std::make_unique<BoundIndirectCall>(
            std::move(calleeExpr), std::move(args), fpType.getReturnType()));
        return;
      }
    }
    return;
  }

  auto candidates = collectOverloads(symbol);
  if (candidates.empty()) {
    error(node.span, "'" + calleeParts.back() + "' is not a function.");
    return;
  }

  bool seenSpreadArg = false;
  std::vector<std::unique_ptr<BoundExpression>> rawArgs;
  std::vector<bool> rawArgIsRef;
  std::vector<bool> rawArgIsSpread;
  std::vector<std::string> rawArgNames;
  for (size_t i = 0; i < node.params_.size(); ++i) {
    if (seenSpreadArg) {
      error(node.params_[i]->value->span,
            "Spread argument must be the last argument in a function call.");
      return;
    }

    auto arg =
        bindExpressionWithExpected(node.params_[i]->value.get(), nullptr);
    if (!arg)
      return;
    if (node.params_[i]->isRef &&
        !requireMutablePlace(*arg, node.params_[i]->value->span,
                             MutablePlaceUse::MutableReference)) {
      return;
    }
    rawArgNames.push_back(node.params_[i]->name);
    rawArgIsRef.push_back(node.params_[i]->isRef);
    rawArgIsSpread.push_back(node.params_[i]->isSpread);
    if (node.params_[i]->isSpread) {
      seenSpreadArg = true;
    }
    rawArgs.push_back(std::move(arg));
  }

  struct CandidateMatch {
    std::shared_ptr<FunctionSymbol> symbol;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    std::vector<bool> argumentIsRef;
    std::unique_ptr<BoundExpression> variadicPack;
    std::vector<int> cost;
    bool usedExtraArguments = false;
    int returnCost = 0;
    std::vector<std::string> notes;
  };

  auto compareCost = [](const CandidateMatch &lhs, const CandidateMatch &rhs) {
    if (lhs.cost != rhs.cost) {
      return lhs.cost < rhs.cost;
    }
    if (lhs.returnCost != rhs.returnCost) {
      return lhs.returnCost < rhs.returnCost;
    }
    if (lhs.usedExtraArguments != rhs.usedExtraArguments) {
      return !lhs.usedExtraArguments && rhs.usedExtraArguments;
    }
    if (lhs.symbol->acceptsExtraArguments() !=
        rhs.symbol->acceptsExtraArguments()) {
      return !lhs.symbol->acceptsExtraArguments() &&
             rhs.symbol->acceptsExtraArguments();
    }
    return false;
  };

  std::vector<CandidateMatch> matches;
  std::shared_ptr<FunctionSymbol> blockedUnsafeMatch = nullptr;
  std::vector<std::string> rejectionNotes;
  auto expectedReturnType = currentExpectedExpressionType();

  for (const auto &funcSymbol : candidates) {
    if (!funcSymbol) {
      continue;
    }

    size_t fixedParamCount = funcSymbol->fixedParameterCount();
    bool hasExplicitTypeArgs = !node.genericArgs_.empty();
    if (hasExplicitTypeArgs &&
        node.genericArgs_.size() > funcSymbol->genericParameterNames.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': explicit generic argument count mismatch");
      continue;
    }

    if (!hasExplicitTypeArgs && !funcSymbol->genericParameterNames.empty()) {
      // inference is allowed; no early rejection
    }
    if (!funcSymbol->acceptsExtraArguments() &&
        node.params_.size() != funcSymbol->parameters.size()) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': wrong argument count");
      continue;
    }
    if (funcSymbol->acceptsExtraArguments() &&
        node.params_.size() < fixedParamCount) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': too few arguments");
      continue;
    }

    CandidateMatch match;
    match.symbol = funcSymbol;
    match.arguments.resize(fixedParamCount);
    match.argumentIsRef.resize(fixedParamCount, false);
    std::string genericBindingFailure;
    auto genericBindings =
        buildGenericBindings(*funcSymbol, rawArgs, node.genericArgs_, node.span,
                             &genericBindingFailure);
    if (!funcSymbol->genericParameterNames.empty() && genericBindings.empty()) {
      rejectionNotes.push_back(
          "'" + renderFunctionSignature(*funcSymbol) + "': " +
          (genericBindingFailure.empty() ? "generic type binding failed"
                                         : genericBindingFailure));
      continue;
    }
    bool failed = false;
    std::string failureReason;
    auto variadicParam = funcSymbol->variadicParameter();
    std::vector<int> positionalToParameter(rawArgs.size(), -1);
    std::vector<bool> parameterAssigned(funcSymbol->parameters.size(), false);
    bool seenNamedArgument = false;

    for (size_t i = 0, positionalIndex = 0; i < rawArgs.size(); ++i) {
      const bool isSpread = rawArgIsSpread[i];
      const bool isNamed = !rawArgNames[i].empty();

      if (isSpread) {
        if (isNamed) {
          failed = true;
          failureReason = "named spread arguments are not supported";
          break;
        }
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
        continue;
      }

      if (isNamed) {
        seenNamedArgument = true;
        bool found = false;
        for (size_t paramIndex = 0; paramIndex < fixedParamCount;
             ++paramIndex) {
          if (funcSymbol->parameters[paramIndex]->name != rawArgNames[i]) {
            continue;
          }
          if (parameterAssigned[paramIndex]) {
            failed = true;
            failureReason =
                "parameter '" + rawArgNames[i] + "' provided more than once";
            break;
          }
          positionalToParameter[i] = static_cast<int>(paramIndex);
          parameterAssigned[paramIndex] = true;
          found = true;
          break;
        }
        if (failed) {
          break;
        }
        if (!found) {
          failed = true;
          failureReason = "unknown named argument '" + rawArgNames[i] + "'";
          break;
        }
        continue;
      }

      if (seenNamedArgument) {
        failed = true;
        failureReason = "positional arguments cannot follow named arguments";
        break;
      }

      while (positionalIndex < fixedParamCount &&
             parameterAssigned[positionalIndex]) {
        ++positionalIndex;
      }

      if (positionalIndex < fixedParamCount) {
        positionalToParameter[i] = static_cast<int>(positionalIndex);
        parameterAssigned[positionalIndex] = true;
        ++positionalIndex;
      } else {
        positionalToParameter[i] = static_cast<int>(fixedParamCount);
      }
    }

    if (!failed) {
      for (size_t paramIndex = 0; paramIndex < fixedParamCount; ++paramIndex) {
        if (!parameterAssigned[paramIndex]) {
          failed = true;
          failureReason = "missing argument for parameter '" +
                          funcSymbol->parameters[paramIndex]->name + "'";
          break;
        }
      }
    }

    for (size_t i = 0; i < rawArgs.size(); ++i) {
      auto arg = rawArgs[i]->clone();
      bool argIsRef = rawArgIsRef[i];
      bool argIsSpread = rawArgIsSpread[i];
      int parameterIndex = positionalToParameter[i];

      if (failed) {
        break;
      }

      if (argIsSpread) {
        if (parameterIndex < static_cast<int>(fixedParamCount) || argIsRef ||
            !funcSymbol->hasVariadicParameter()) {
          failed = true;
          failureReason =
              "spread arguments can only target a Zap variadic parameter";
          break;
        }

        if (!variadicParam || !variadicParam->variadic_element_type) {
          failed = true;
          failureReason = "internal error: missing variadic parameter type";
          break;
        }

        auto expectedViewType =
            makeVariadicViewType(variadicParam->variadic_element_type);

        auto conversion =
            conversions_.classifyImplicit(arg->type, expectedViewType);
        if (arg->type && arg->type->getKind() == zir::TypeKind::Array) {
          if (!conversion) {
            failed = true;
            failureReason =
                "spread argument type does not match variadic parameter";
            break;
          }
          arg = applyConversion(std::move(arg), *conversion);
        } else if (!arg->type || !isVariadicViewType(arg->type) ||
                   !conversion) {
          failed = true;
          failureReason =
              "spread argument type does not match variadic parameter";
          break;
        }

        match.variadicPack = std::move(arg);
        match.usedExtraArguments = true;
        match.notes.push_back("spread -> variadic pack");
        continue;
      }

      if (parameterIndex >= 0 &&
          parameterIndex < static_cast<int>(fixedParamCount)) {
        auto expectedType = funcSymbol->parameters[parameterIndex]->type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        const auto &parameter = funcSymbol->parameters[parameterIndex];
        if (argIsRef != parameter->is_ref) {
          failed = true;
          failureReason = "argument for parameter '" + parameter->name +
                          "' has mismatched ref-ness";
          break;
        }

        if (argIsRef) {
          auto varExpr = dynamic_cast<BoundVariableExpression *>(arg.get());
          if (!varExpr || !typeInterner_.same(arg->type, expectedType)) {
            failed = true;
            failureReason = "ref argument for parameter '" + parameter->name +
                            "' must exactly match type '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(0);
          match.notes.push_back("param " + parameter->name +
                                ": exact ref match");
        } else {
          auto conversion =
              conversions_.classifyImplicit(arg->type, expectedType);
          if (!conversion) {
            failed = true;
            failureReason = "argument for parameter '" + parameter->name +
                            "' is not convertible from '" +
                            renderTypeForUser(arg->type) + "' to '" +
                            renderTypeForUser(expectedType) + "'";
            break;
          }
          match.cost.push_back(conversion->cost());
          match.notes.push_back("param " + parameter->name + ": " +
                                std::string(conversion->description()));
          arg = applyConversion(std::move(arg), *conversion);
        }
        match.argumentIsRef[parameterIndex] = argIsRef;
        match.arguments[parameterIndex] = std::move(arg);
      } else if (funcSymbol->hasVariadicParameter()) {
        if (argIsRef) {
          failed = true;
          failureReason = "variadic arguments cannot be passed by ref";
          break;
        }
        auto expectedType = variadicParam->variadic_element_type;
        if (!genericBindings.empty()) {
          expectedType = substituteGenericType(expectedType, genericBindings);
        }
        auto conversion =
            conversions_.classifyImplicit(arg->type, expectedType);
        if (!conversion) {
          failed = true;
          failureReason = "variadic argument is not convertible from '" +
                          renderTypeForUser(arg->type) + "' to '" +
                          renderTypeForUser(expectedType) + "'";
          break;
        }
        match.cost.push_back(conversion->cost());
        match.usedExtraArguments = true;
        match.notes.push_back("variadic: " +
                              std::string(conversion->description()));
        arg = applyConversion(std::move(arg), *conversion);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else if (funcSymbol->isCVariadic) {
        if (argIsRef) {
          failed = true;
          failureReason = "C variadic arguments cannot be passed by ref";
          break;
        }
        auto promotedType = getCVariadicArgumentType(arg->type);
        if (!promotedType) {
          failed = true;
          failureReason = "type '" + renderTypeForUser(arg->type) +
                          "' is not supported in C variadic arguments";
          break;
        }
        auto conversion =
            conversions_.classifyCVariadic(arg->type, promotedType);
        if (!conversion) {
          failed = true;
          failureReason = "type '" + renderTypeForUser(arg->type) +
                          "' cannot undergo its required C promotion";
          break;
        }
        match.cost.push_back(conversion->cost());
        match.usedExtraArguments = true;
        match.notes.push_back("c variadic: " +
                              std::string(conversion->description()));
        arg = applyConversion(std::move(arg), *conversion);
        match.argumentIsRef.push_back(false);
        match.arguments.push_back(std::move(arg));
      } else {
        failed = true;
        failureReason = "too many arguments";
        break;
      }
    }

    if (failed) {
      rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                               "': " + failureReason);
      continue;
    }

    if (expectedReturnType) {
      if (auto conversion = conversions_.classifyImplicit(
              funcSymbol->returnType, expectedReturnType)) {
        match.returnCost = conversion->cost();
        match.notes.push_back(
            "return: " + std::string(conversion->description()));
      } else {
        match.returnCost = 50;
        match.notes.push_back("return: incompatible with expected " +
                              renderTypeForUser(expectedReturnType));
      }
    }

    std::shared_ptr<FunctionSymbol> resolvedSymbol = funcSymbol;
    if (!funcSymbol->genericParameterNames.empty()) {
      resolvedSymbol = ensureGenericFunctionInstantiation(
          funcSymbol, orderedGenericBindings(genericBindings), node.span);
      if (!resolvedSymbol) {
        rejectionNotes.push_back("'" + renderFunctionSignature(*funcSymbol) +
                                 "': failed to instantiate generic function");
        continue;
      }

      std::vector<std::unique_ptr<BoundExpression>> remappedArgs;
      std::vector<bool> remappedRef;
      remappedArgs.reserve(match.arguments.size());
      remappedRef.reserve(match.argumentIsRef.size());

      for (size_t i = 0; i < match.arguments.size(); ++i) {
        auto argClone =
            match.arguments[i] ? match.arguments[i]->clone() : nullptr;
        if (i < resolvedSymbol->parameters.size() && argClone) {
          auto expected = resolvedSymbol->parameters[i]->type;
          if (!resolvedSymbol->parameters[i]->is_ref) {
            if (auto conversion =
                    conversions_.classifyImplicit(argClone->type, expected)) {
              argClone = applyConversion(std::move(argClone), *conversion);
            }
          }
        }
        remappedArgs.push_back(std::move(argClone));
        remappedRef.push_back(
            i < match.argumentIsRef.size() ? match.argumentIsRef[i] : false);
      }

      match.arguments = std::move(remappedArgs);
      match.argumentIsRef = std::move(remappedRef);
      match.symbol = resolvedSymbol;
    }

    if (resolvedSymbol->isUnsafe && !isUnsafeActive()) {
      blockedUnsafeMatch = resolvedSymbol;
      rejectionNotes.push_back("'" + renderFunctionSignature(*resolvedSymbol) +
                               "': requires unsafe context");
      continue;
    }
    matches.push_back(std::move(match));
  }

  if (matches.empty()) {
    if (blockedUnsafeMatch) {
      requireUnsafeContext(node.span, "unsafe function calls");
      return;
    }

    error(
        node.callee_->span,
        "No matching overload for function '" + calleeParts.back() + "'. " +
            (rejectionNotes.empty() ? std::string() : ("Candidates: " + [&]() {
              std::string details;
              for (size_t i = 0; i < rejectionNotes.size(); ++i) {
                if (i != 0) {
                  details += "; ";
                }
                details += rejectionNotes[i];
              }
              return details;
            }())));
    return;
  }

  size_t bestIndex = 0;
  for (size_t i = 1; i < matches.size(); ++i) {
    if (compareCost(matches[i], matches[bestIndex])) {
      bestIndex = i;
    }
  }

  std::vector<size_t> ambiguous = {bestIndex};
  for (size_t i = 0; i < matches.size(); ++i) {
    if (i == bestIndex) {
      continue;
    }
    if (!compareCost(matches[i], matches[bestIndex]) &&
        !compareCost(matches[bestIndex], matches[i])) {
      ambiguous.push_back(i);
    }
  }

  if (ambiguous.size() > 1) {
    std::string message =
        "Call to function '" + calleeParts.back() + "' is ambiguous between ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += i + 1 == ambiguous.size() ? " and " : ", ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "'";
    }
    message += ".";
    if (expectedReturnType) {
      message +=
          " Expected result type: '" + expectedReturnType->toString() + "'.";
    }
    message += " Candidate details: ";
    for (size_t i = 0; i < ambiguous.size(); ++i) {
      if (i != 0) {
        message += "; ";
      }
      message +=
          "'" + renderFunctionSignature(*matches[ambiguous[i]].symbol) + "' [";
      for (size_t j = 0; j < matches[ambiguous[i]].notes.size(); ++j) {
        if (j != 0) {
          message += ", ";
        }
        message += matches[ambiguous[i]].notes[j];
      }
      message += "]";
    }
    error(node.callee_->span, message);
    return;
  }

  auto &best = matches[bestIndex];
  expressionStack_.push(std::make_unique<BoundFunctionCall>(
      best.symbol, std::move(best.arguments), std::move(best.argumentIsRef),
      std::move(best.variadicPack)));
  if (semanticInfo_) {
    semanticInfo_->recordCall(currentModuleId_, node.span, best.symbol);
  }
}

bool Binder::bindSizeOfBuiltinCall(FunCall &node) {
  auto *calleeId = dynamic_cast<ConstId *>(node.callee_.get());
  if (!calleeId || calleeId->value_ != "sizeof") {
    return false;
  }

  if (node.params_.size() != 1 || node.params_[0]->isRef ||
      node.params_[0]->isSpread || !node.params_[0]->name.empty()) {
    error(node.span, "'sizeof' expects exactly one positional argument.");
    return true;
  }

  auto argument =
      bindExpressionWithExpected(node.params_[0]->value.get(), nullptr);
  if (!argument) {
    return true;
  }

  auto layout = computeTypeLayout(argument->type, targetInfo_);
  expressionStack_.push(std::make_unique<BoundLiteral>(
      std::to_string(layout.size),
      std::make_shared<zir::PrimitiveType>(zir::TypeKind::Int)));
  return true;
}

bool Binder::bindWeakBuiltinCall(FunCall &node) {
  auto *calleeId = dynamic_cast<ConstId *>(node.callee_.get());
  if (!calleeId) {
    return false;
  }

  const bool isLock = calleeId->value_ == "lock";
  const bool isAlive = calleeId->value_ == "alive";
  if (!isLock && !isAlive) {
    return false;
  }

  if (node.params_.size() != 1 || node.params_[0]->isRef ||
      node.params_[0]->isSpread || !node.params_[0]->name.empty()) {
    error(node.span, "'" + calleeId->value_ +
                         "' expects exactly one positional argument.");
    return true;
  }

  auto weakExpr =
      bindExpressionWithExpected(node.params_[0]->value.get(), nullptr);
  if (!weakExpr) {
    return true;
  }

  if (weakExpr->type->getKind() != zir::TypeKind::Class) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  auto weakClassType = std::static_pointer_cast<zir::ClassType>(weakExpr->type);
  if (!weakClassType->isWeak()) {
    error(node.params_[0]->value->span,
          "'" + calleeId->value_ + "' expects a weak class reference.");
    return true;
  }

  if (isAlive) {
    expressionStack_.push(
        std::make_unique<BoundWeakAliveExpression>(std::move(weakExpr)));
    return true;
  }

  auto strongType = std::make_shared<zir::ClassType>(*weakClassType);
  strongType->setWeak(false);
  expressionStack_.push(std::make_unique<BoundWeakLockExpression>(
      std::move(weakExpr), strongType));
  return true;
}

} // namespace sema
