#pragma once

#include "ast/root_node.hpp"
#include "macros/ctfe_interpreter.hpp"
#include "macros/ctfe_semantics.hpp"

#include <memory>

namespace zap::ctfe {

class CtfeProgram {
public:
  const ValidatedFunctions &functions() const { return functions_; }
  const std::string &cacheKey() const { return cacheKey_; }
  size_t memoryUsed() const { return memoryUsed_; }
  size_t sourceBytes() const { return sourceBytes_; }
  size_t tokenCount() const { return tokenCount_; }
  size_t syntaxDepth() const { return syntaxDepth_; }

private:
  friend class CtfeProgramBuilder;
  std::vector<std::unique_ptr<RootNode>> roots_;
  ValidatedFunctions functions_;
  std::string cacheKey_;
  size_t memoryUsed_ = 0;
  size_t sourceBytes_ = 0;
  size_t tokenCount_ = 0;
  size_t syntaxDepth_ = 0;
};

// Owns the parsed ASTs; only finish() can publish a validated program.
class CtfeProgramBuilder {
public:
  CtfeProgramBuilder(DiagnosticEngine &diagnostics, CtfeLimits limits = {});
  const RootNode *addSource(const std::string &source,
                            const SourceSpan *bodySpan = nullptr);
  const FunDecl *addFunction(const std::vector<Token> &tokens,
                             const std::string &source,
                             const std::string &identity);
  std::shared_ptr<const CtfeProgram>
  finish(const std::vector<FunctionDefinition> &entries,
         const FunctionLookup &lookup);

private:
  const RootNode *parse(std::vector<Token> tokens);
  void reserveSource(const std::string &source);
  DiagnosticEngine &diagnostics_;
  CtfeLimits limits_;
  std::shared_ptr<CtfeProgram> program_;
};

} // namespace zap::ctfe
