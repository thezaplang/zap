#include "ast/nodes.hpp"
#include "frontend/macro_registry.hpp"
#include "macros/ctfe_program.hpp"
#include "macros/ctfe_runtime.hpp"

namespace zap::frontend {

const ctfe::CtfeProgram *
MacroRegistrySet::program(const MacroDefinition &definition) const {
  const auto found = programs_.find(&definition);
  return found == programs_.end() ? nullptr : found->second.get();
}

void MacroRegistrySet::prepareCtfe(
    const std::map<std::string, ModuleOutline> &outlines,
    std::vector<MacroResolutionError> &errors) {
  for (const auto &[moduleId, outline] : outlines) {
    auto prepare = [&](const MacroDefinition *macro,
                       const FunctionOutline *helper) {
      const std::string emptySource;
      DiagnosticEngine diagnostics(emptySource, moduleId);
      ctfe::CtfeProgramBuilder builder(diagnostics);
      std::map<const FunctionOutline *, ctfe::FunctionDefinition> parsed;
      std::map<const FunDecl *, std::string> owners;
      auto parseHelper = [&](const FunctionBinding &binding) {
        auto known = parsed.find(binding.definition);
        if (known != parsed.end())
          return known->second;
        const auto &source = *binding.definition;
        const auto identity = binding.definingModuleId + ":" +
                              std::to_string(source.name.span.offset);
        const auto *function =
            builder.addFunction(source.tokens, source.source, identity);
        if (!function)
          throw ctfe::CtfeFailure{"M3002", "Invalid CTFE helper declaration."};
        if (!function->isCtfeOnly()) {
          auto syntaxType = [](const TypeNode *type) {
            return type && (type->typeName == "SyntaxSource" ||
                            type->typeName == "SyntaxTokens" ||
                            type->typeName == "SyntaxExpr" ||
                            type->typeName == "SyntaxItem");
          };
          bool syntaxSignature = syntaxType(function->returnType_.get());
          for (const auto &parameter : function->params_)
            syntaxSignature |= syntaxType(parameter->type.get());
          if (syntaxSignature) {
            diagnostics.report(
                function->span, DiagnosticLevel::Error, "M3002",
                "Helpers with Syntax* signatures require @ctfe.");
            throw ctfe::CtfeFailure{
                "M3002", "Helpers with Syntax* signatures require @ctfe."};
          }
        }
        ctfe::FunctionDefinition definition{identity, function};
        owners.emplace(function, binding.definingModuleId);
        parsed.emplace(binding.definition, definition);
        return definition;
      };
      const SourceSpan span =
          macro ? macro->expansion.span() : helper->name.span;
      try {
        ctfe::FunctionDefinition entry;
        if (macro) {
          const auto *root =
              builder.addSource(macro->procedural->functionSource, &span);
          const auto *function =
              root && root->children.size() == 1
                  ? dynamic_cast<const FunDecl *>(root->children.front().get())
                  : nullptr;
          if (!function)
            throw ctfe::CtfeFailure{"M3002", "Invalid CTFE macro declaration."};
          owners.emplace(function, moduleId);
          entry = {"__syntax_macro__", function};
        } else
          entry = parseHelper({helper, moduleId});
        auto lookup = [&](const FunDecl &caller, const ExpressionNode &callee)
            -> std::optional<ctfe::FunctionDefinition> {
          const auto *registry = module(owners.at(&caller));
          const std::vector<FunctionBinding> *candidates = nullptr;
          if (const auto *id = dynamic_cast<const ConstId *>(&callee)) {
            const auto found = registry->functions_.find(id->value_);
            if (found != registry->functions_.end())
              candidates = &found->second;
          } else if (const auto *member =
                         dynamic_cast<const MemberAccessNode *>(&callee)) {
            const auto *alias =
                dynamic_cast<const ConstId *>(member->left_.get());
            const auto *targetId =
                alias ? registry->findModule(alias->value_) : nullptr;
            const auto *target = targetId ? module(*targetId) : nullptr;
            if (target) {
              const auto found =
                  target->exportedFunctions_.find(member->member_);
              if (found != target->exportedFunctions_.end())
                candidates = &found->second;
            }
          }
          if (!candidates)
            return std::nullopt;
          if (candidates->size() != 1)
            throw ctfe::CtfeFailure{
                "M3002", "Ambiguous or overloaded CTFE helper call."};
          return parseHelper(candidates->front());
        };
        auto program = builder.finish({entry}, lookup);
        if (macro && program)
          programs_.emplace(macro, std::move(program));
      } catch (const ctfe::CtfeFailure &failure) {
        if (!diagnostics.hadErrors())
          diagnostics.report(span, DiagnosticLevel::Error, failure.code,
                             failure.message);
      }
      for (const auto &diagnostic : diagnostics.diagnostics())
        errors.push_back(
            {moduleId, diagnostic.span, diagnostic.message, diagnostic.code});
    };
    for (const auto &macro : outline.macros)
      if (macro.procedural)
        prepare(&macro, nullptr);
    for (const auto &helper : outline.functions)
      if (helper.ctfeOnly)
        prepare(nullptr, &helper);
  }
}

} // namespace zap::frontend
