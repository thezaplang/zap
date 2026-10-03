#include "macros/ctfe_interpreter.hpp"
#include "token/token.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace zap::ctfe;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

SyntaxMacroRequest request() {
  SyntaxMacroRequest value;
  value.definitionId = "test-macro";
  value.invocation = {"call.zp", 3, 5, 20, 8};
  return value;
}

bool failsWith(const SyntaxMacroResult &result, const std::string &code) {
  return !result.output && result.diagnostics.size() == 1 &&
         result.diagnostics[0].code == code &&
         result.diagnostics[0].span.sourceName == "call.zp" &&
         result.diagnostics[0].span.offset == 20;
}

void testExecutionAndCache() {
  const std::string source = R"(
fun answer(input: SyntaxTokens) SyntaxExpr {
    var count: Int = 0;
    while count < 3 { count = count + 1; }
    if count == 3 { return syntaxExpr("40 + 2"); }
    return syntaxExpr("0");
}
)";
  CtfeInterpreter interpreter;
  auto first = interpreter.execute(source, "answer", request());
  require(first.output && first.diagnostics.empty() &&
              std::holds_alternative<SyntaxExpr>(*first.output) &&
              std::get<SyntaxExpr>(*first.output).syntax.tokens.size() == 3,
          "safe Zap function did not produce a validated syntax expression");
  auto second = interpreter.execute(source, "answer", request());
  require(second.output && interpreter.cacheHits() == 1 &&
              interpreter.cacheEntries() == 1,
          "identical CTFE request missed the deterministic cache");
  auto changedRequest = request();
  changedRequest.expected = SyntaxContext::Item;
  interpreter.execute(source, "answer", changedRequest);
  require(interpreter.cacheEntries() == 2,
          "expected output context was omitted from the cache key");
  CtfeLimits changedLimits;
  changedLimits.maxSteps += 1;
  interpreter.execute(source, "answer", request(), changedLimits);
  require(interpreter.cacheEntries() == 3,
          "CTFE configuration was omitted from the cache key");
  interpreter.execute("fun answer(input: SyntaxTokens) SyntaxExpr { "
                      "return syntaxExpr(\"43\"); }",
                      "answer", request());
  require(interpreter.cacheEntries() == 4,
          "function definition was omitted from the cache key");

  CtfeInterpreter boundedCache;
  CtfeLimits cacheLimits;
  cacheLimits.maxCacheEntries = 1;
  boundedCache.execute(source, "answer", request(), cacheLimits);
  boundedCache.execute(source, "answer", changedRequest, cacheLimits);
  require(boundedCache.cacheEntries() == 1,
          "CTFE cache exceeded its entry limit");
}

void testSourceInput() {
  auto input = request();
  SyntaxSource source;
  source.text = "abc";
  source.sourceName = "call.zp";
  for (uint64_t offset = 0; offset <= source.text.size(); ++offset)
    source.offsets.push_back({1, offset + 1, offset});
  input.input = std::move(source);
  CtfeInterpreter interpreter;
  auto result = interpreter.execute(
      "fun run(input: SyntaxSource) SyntaxExpr { "
      "if input.text == \"abc\" { return syntaxExpr(\"42\"); } "
      "return syntaxExpr(\"0\"); }",
      "run", input);
  require(result.output &&
              std::get<SyntaxExpr>(*result.output).syntax.tokens[0].value ==
                  "42",
          "CTFE source capture or member access failed");
}

void testLimitsAndFailures() {
  CtfeInterpreter interpreter;
  CtfeLimits limits;
  limits.maxSteps = 20;
  auto spin = interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { "
                                  "while true {} return syntaxExpr(\"0\"); }",
                                  "run", request(), limits);
  require(failsWith(spin, "M3003"),
          "infinite compile-time loop did not hit the instruction budget");

  limits = {};
  limits.maxCallDepth = 3;
  auto recurse = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { return run(input); }", "run",
      request(), limits);
  require(failsWith(recurse, "M3003"),
          "recursive macro function did not hit the call-depth budget");

  limits = {};
  limits.maxMemoryBytes = 64;
  auto memory = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", request(), limits);
  require(failsWith(memory, "M3003"),
          "CTFE request did not respect its memory budget");

  const std::string generatedSource =
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"" +
      std::string(100, 'x') + "\"); }";
  limits = {};
  limits.maxMemoryBytes = generatedSource.size() * 256 + 10'000;
  auto generatedMemory =
      interpreter.execute(generatedSource, "run", request(), limits);
  require(failsWith(generatedMemory, "M3003"),
          "generated syntax parser bypassed the CTFE memory budget");

  limits = {};
  limits.maxSyntaxDepth = 1;
  auto deep = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", request(), limits);
  require(failsWith(deep, "M3003"),
          "CTFE parser accepted syntax deeper than its configured limit");

  auto panic = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { panic(\"stop\"); "
      "return syntaxExpr(\"0\"); }",
      "run", request());
  require(failsWith(panic, "M3004") && panic.diagnostics[0].message == "stop",
          "compile-time panic did not become a call-site diagnostic");

  auto invalid =
      interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { "
                          "return syntaxExpr(\"1 +\"); }",
                          "run", request());
  require(failsWith(invalid, "M3001"),
          "invalid generated expression was not rejected");

  auto overflow =
      interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { "
                          "var value: Int = 9223372036854775807 + 1; "
                          "return syntaxExpr(\"0\"); }",
                          "run", request());
  require(failsWith(overflow, "M3001"),
          "compile-time integer overflow was not diagnosed");
}

void testForbiddenCapabilities() {
  CtfeInterpreter interpreter;
  auto external =
      interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { "
                          "return readFile(\"/etc/passwd\"); }",
                          "run", request());
  require(failsWith(external, "M3002"),
          "unapproved host function call escaped the CTFE allowlist");

  auto imported = interpreter.execute(
      "import \"std/fs\"; "
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", request());
  require(failsWith(imported, "M3002"), "CTFE accepted a module import");

  auto declaration = interpreter.execute(
      "macro hidden() { 1 } "
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", request());
  require(failsWith(declaration, "M3002"),
          "CTFE accepted a nested macro declaration");

  auto forged = request();
  forged.version = SyntaxProtocolVersion + 1;
  auto unsupported = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", forged);
  require(failsWith(unsupported, "M3001"),
          "CTFE accepted an unsupported protocol version");
}

void testForwardedOutputLimits() {
  const std::string source =
      "fun run(input: SyntaxTokens) SyntaxTokens { return input; }";
  auto input = request();
  SyntaxTokens tokens;
  tokens.tokens.resize(CtfeLimits{}.maxDefinitionTokens + 1,
                       {static_cast<uint32_t>(TokenType::ID), "x", "x",
                        input.invocation, 0, nullptr});
  input.input = std::move(tokens);
  CtfeInterpreter interpreter;
  require(failsWith(interpreter.execute(source, "run", input), "M3003"),
          "forwarded syntax bypassed the CTFE output token limit");

  SyntaxTokens nested;
  for (size_t index = 0; index < 4; ++index)
    nested.tokens.push_back({static_cast<uint32_t>(TokenType::LPAREN), "(", "(",
                             input.invocation, 0, nullptr});
  for (size_t index = 0; index < 4; ++index)
    nested.tokens.push_back({static_cast<uint32_t>(TokenType::RPAREN), ")", ")",
                             input.invocation, 0, nullptr});
  input.input = std::move(nested);
  CtfeLimits limits;
  limits.maxSyntaxDepth = 3;
  require(failsWith(interpreter.execute(source, "run", input, limits), "M3003"),
          "forwarded syntax bypassed the CTFE output nesting limit");
}

void testTypeValidation() {
  CtfeInterpreter interpreter;
  for (const std::string body :
       {"var number: String = 42; return syntaxExpr(\"0\");",
        "var number: Int = 42; number = \"wrong\"; return syntaxExpr(\"0\");",
        "let number: Int = 42; number = 43; return syntaxExpr(\"0\");",
        "if false { var number: String = 42; } return syntaxExpr(\"0\");",
        "if false { readFile(\"bad\"); } return syntaxExpr(\"0\");",
        "return syntaxExpr(42);", "return syntaxItem(\"\");", "return;",
        "if 1 { return syntaxExpr(\"0\"); } return syntaxExpr(\"0\");",
        "return true ? syntaxExpr(\"0\") : \"bad\";",
        "var syntaxExpr: Int = 1; return syntaxExpr(\"0\");",
        "var value: *Int = 1; return syntaxExpr(\"0\");",
        "syntaxExpr(\"0\")"}) {
    auto result = interpreter.execute(
        "fun run(input: SyntaxTokens) SyntaxExpr { " + body + " }", "run",
        request());
    require(
        failsWith(result, "M3002"),
        "invalid CTFE definition escaped static type/control-flow validation");
  }
  auto wrongParameter =
      interpreter.execute("fun helper(number: String) Int { return 1; } "
                          "fun run(input: SyntaxTokens) SyntaxExpr { "
                          "helper(42); return syntaxExpr(\"0\"); }",
                          "run", request());
  require(failsWith(wrongParameter, "M3002"),
          "CTFE helper call ignored its parameter type");
  auto wrongReturn = interpreter.execute(
      "fun unused() Int { return \"bad\"; } "
      "fun run(input: SyntaxTokens) SyntaxExpr { return syntaxExpr(\"0\"); }",
      "run", request());
  require(failsWith(wrongReturn, "M3002"),
          "unused CTFE helper ignored its return type");
  auto wrongInput = request();
  wrongInput.input = SyntaxExpr{};
  require(
      failsWith(interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { "
                                    "return syntaxExpr(\"0\"); }",
                                    "run", wrongInput),
                "M3001"),
      "CTFE protocol boundary ignored the entry parameter type");
}

void testBlockValuesAreNotReturns() {
  CtfeInterpreter interpreter;
  for (const std::string body :
       {"if true { syntaxExpr(\"42\") } return syntaxExpr(\"0\");",
        "var count: Int = 0; while count < 2 { count = count + 1; "
        "syntaxExpr(\"42\") } return syntaxExpr(\"0\");",
        "noop(); return syntaxExpr(\"0\");"}) {
    auto result =
        interpreter.execute("fun noop() { if true { 123 } } "
                            "fun run(input: SyntaxTokens) SyntaxExpr { " +
                                body + " }",
                            "run", request());
    require(result.output &&
                std::get<SyntaxExpr>(*result.output).syntax.tokens[0].value ==
                    "0",
            "CTFE treated a block value as an explicit function return");
  }
  auto nestedReturn =
      interpreter.execute("fun run(input: SyntaxTokens) SyntaxExpr { while "
                          "true { if true { return syntaxExpr(\"42\"); } } }",
                          "run", request());
  require(
      nestedReturn.output &&
          std::get<SyntaxExpr>(*nestedReturn.output).syntax.tokens[0].value ==
              "42",
      "CTFE failed to propagate an explicit return across blocks");
  auto minimum = interpreter.execute(
      "fun run(input: SyntaxTokens) SyntaxExpr { var n: Int = "
      "-9223372036854775808; "
      "if n < 0 { return syntaxExpr(\"42\"); } return syntaxExpr(\"0\"); }",
      "run", request());
  require(minimum.output &&
              std::get<SyntaxExpr>(*minimum.output).syntax.tokens[0].value ==
                  "42",
          "typed CTFE rejected the minimum signed Int literal");
}

} // namespace

int main() {
  testExecutionAndCache();
  testSourceInput();
  testLimitsAndFailures();
  testForbiddenCapabilities();
  testForwardedOutputLimits();
  testTypeValidation();
  testBlockValuesAreNotReturns();
}
