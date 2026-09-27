#pragma once

#include "macros/macro_value.hpp"
#include "utils/diagnostics.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zap {

class MacroMetaEvaluator {
public:
  using FreshContext = std::function<std::optional<SyntaxContextId>()>;

  MacroMetaEvaluator(DiagnosticEngine &diagnostics,
                     const SourceSpan &invocation,
                     std::shared_ptr<const ExpansionOrigin> origin,
                     FreshContext freshContext);

  std::optional<MetaValue> evaluate(const std::vector<TokenTree> &expression,
                                    const MetaScope &scope);
  std::optional<MetaValue> member(const MetaValue &value,
                                  const std::string &name);
  std::optional<std::vector<TokenTree>> emit(const MetaValue &value);
  std::optional<bool> boolean(const MetaValue &value);
  std::optional<std::string> string(const MetaValue &value);
  std::optional<SourceSpan> span(const MetaValue &value);
  void report(const std::string &message);

private:
  struct Parser;
  DiagnosticEngine &diagnostics_;
  const SourceSpan &invocation_;
  std::shared_ptr<const ExpansionOrigin> origin_;
  FreshContext freshContext_;

  std::optional<MetaValue> call(const std::string &name,
                                const TokenTree &arguments,
                                const MetaScope &scope);
  std::string sourceText(const MetaFragment &fragment) const;
  std::string fragmentKind(const MetaFragment &fragment) const;
  TokenTree literal(TokenType type, const std::string &value) const;
};

} // namespace zap
