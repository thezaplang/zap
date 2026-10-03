#include "lexer/lexer.hpp"
#include "macros/ctfe_interpreter.hpp"
#include "macros/syntax_bridge.hpp"
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
  changedRequest.invocation = {"other.zp", 20, 2, 500, 12};
  auto relocated = interpreter.execute(source, "answer", changedRequest);
  require(relocated.output && interpreter.cacheHits() == 2 &&
              interpreter.cacheEntries() == 1 &&
              std::get<SyntaxExpr>(*relocated.output)
                      .syntax.tokens.front()
                      .span.sourceName == "other.zp" &&
              std::get<SyntaxExpr>(*relocated.output)
                      .syntax.tokens.front()
                      .span.offset == 500,
          "semantic cache did not rebase generated syntax to the current call");
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

void testCapturedCacheOutput() {
  CtfeInterpreter interpreter;
  const std::string definition =
      "fun run(input: SyntaxTokens) SyntaxTokens { return input; }";
  auto first = request();
  first.input = SyntaxTokens{
      {{TokenType::ID, "name", "name", first.invocation, 7, nullptr}}};
  auto original = interpreter.execute(definition, "run", first);
  require(original.output.has_value(), "CTFE capture passthrough failed");
  auto second = request();
  second.invocation = {"second.zp", 8, 1, 100, 4};
  second.input = SyntaxTokens{
      {{TokenType::ID, "name", "name", second.invocation, 99, nullptr}}};
  auto relocated = interpreter.execute(definition, "run", second);
  require(
      relocated.output && interpreter.cacheHits() == 1 &&
          std::get<SyntaxTokens>(*relocated.output).tokens.front().context ==
              99 &&
          std::get<SyntaxTokens>(*relocated.output)
                  .tokens.front()
                  .span.offset == 100,
      "cache reused another call's capture or hygiene");
  std::get<SyntaxTokens>(second.input).tokens.front().value = "different";
  interpreter.execute(definition, "run", second);
  require(interpreter.cacheEntries() == 2,
          "semantic cache omitted captured token contents");
}

void testCacheRebindingBudget() {
  std::string tokens;
  for (size_t i = 0; i < 600; ++i)
    tokens += "x ";
  const std::string definition =
      "fun run(input: SyntaxTokens) SyntaxTokens { return syntaxTokens(\"" +
      tokens + "\"); }";
  CtfeInterpreter warm;
  require(warm.execute(definition, "run", request()).output.has_value(),
          "small-location cache budget setup failed");
  auto relocated = request();
  relocated.invocation.sourceName = std::string(4'000, 'p');
  CtfeInterpreter cold;
  const auto hit = warm.execute(definition, "run", relocated);
  const auto miss = cold.execute(definition, "run", relocated);
  require(!hit.output && !miss.output &&
              hit.diagnostics.front().code == "M3003" &&
              miss.diagnostics.front().code == "M3003",
          "cache rebinding bypassed the current output allocation budget");
  CtfeLimits largeBudget;
  largeBudget.maxMemoryBytes = 128 * 1024 * 1024;
  CtfeInterpreter protocolCache;
  require(protocolCache.execute(definition, "run", request(), largeBudget)
              .output.has_value(),
          "large-budget cache setup failed");
  relocated.invocation.sourceName = std::string(30'000, 'p');
  const auto oversized =
      protocolCache.execute(definition, "run", relocated, largeBudget);
  require(!oversized.output && oversized.diagnostics.front().code == "M3003",
          "rebased cache output bypassed the wire protocol size limit");
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

void testStructuredSyntax() {
  CtfeInterpreter helpers;
  auto prefixedHelper = helpers.execute(
      "fun syntaxTokenHelper(value: Int) Int { return value * 2; } "
      "fun run(input: SyntaxTokens) SyntaxExpr { if syntaxTokenHelper(21) == "
      "42 { "
      "return syntaxExpr(\"42\"); } return syntaxExpr(\"0\"); }",
      "run", request());
  require(
      prefixedHelper.output &&
          std::get<SyntaxExpr>(*prefixedHelper.output).syntax.tokens[0].value ==
              "42",
      "syntax builtin dispatcher intercepted an ordinary helper name");
  auto input = request();
  input.input = SyntaxTokens{
      {{TokenType::ID, "name", "name", input.invocation, 7, nullptr}}};
  const std::string definition = R"zp(
fun run(input: SyntaxTokens) SyntaxExpr {
  var selected = syntaxSlice(input, 0, 1);
  if syntaxCount(selected) != 1 { panic("count"); }
  if syntaxTokenText(selected, 0) != "name" { panic("text"); }
  if syntaxTokenSpelling(selected, 0) != "name" { panic("spelling"); }
  if syntaxTokenKind(selected, 0) != "identifier" { panic("kind"); }
  var result = syntaxConcat(syntaxTokens("("), syntaxConcat(selected, syntaxTokens(" + 2)")));
  return syntaxExprFromTokens(result);
}
)zp";
  CtfeInterpreter interpreter;
  auto first = interpreter.execute(definition, "run", input);
  require(first.output.has_value(), "structured token composition failed");
  input.invocation = {"next.zp", 20, 1, 100, 8};
  std::get<SyntaxTokens>(input.input).tokens[0].span = {"next.zp", 20, 5, 104,
                                                        4};
  std::get<SyntaxTokens>(input.input).tokens[0].context = 99;
  auto second = interpreter.execute(definition, "run", input);
  require(second.output && interpreter.cacheHits() == 1,
          "composed syntax did not share its semantic cache");
  const auto &tokens = std::get<SyntaxExpr>(*second.output).syntax.tokens;
  require(tokens.size() == 5 && tokens[1].context == 99 &&
              tokens[1].span.offset == 104 &&
              tokens[0].context == GeneratedSyntaxContext &&
              tokens[0].span.offset == 100,
          "composed cache lost captured versus generated token provenance");

  for (const std::string body :
       {"return syntaxExprFromTokens(syntaxTokens(\"1 +\"));",
        "return syntaxExprFromTokens(syntaxSlice(input, -1, 0));",
        "return syntaxExprFromTokens(syntaxSlice(input, 1, 0));"}) {
    auto failure = interpreter.execute(
        "fun run(input: SyntaxTokens) SyntaxExpr { " + body + " }", "run",
        request());
    require(failsWith(failure, "M3001"),
            "invalid structured syntax or index was accepted");
  }
}

void testLocationObservingCache() {
  auto input = request();
  input.input = SyntaxTokens{
      {{TokenType::ID, "name", "name", input.invocation, 7, nullptr}}};
  const std::string definition = R"(
fun run(input: SyntaxTokens) SyntaxExpr {
  if syntaxTokenSourceName(input, 0) == "" { panic("source"); }
  if syntaxTokenColumn(input, 0) < 1 { panic("column"); }
  if syntaxTokenOffset(input, 0) < 0 { panic("offset"); }
  if syntaxTokenLength(input, 0) < 0 { panic("length"); }
  if syntaxTokenLine(input, 0) == 3 { return syntaxExpr("42"); }
  return syntaxExpr("0");
}
)";
  CtfeInterpreter interpreter;
  auto first = interpreter.execute(definition, "run", input);
  require(first.output &&
              std::get<SyntaxExpr>(*first.output).syntax.tokens[0].value ==
                  "42",
          "span read failed");
  std::get<SyntaxTokens>(input.input).tokens[0].span.line = 50;
  auto second = interpreter.execute(definition, "run", input);
  require(second.output &&
              std::get<SyntaxExpr>(*second.output).syntax.tokens[0].value ==
                  "0" &&
              interpreter.cacheHits() == 0 && interpreter.cacheEntries() == 1,
          "span-sensitive macro reused a location-independent result");
  interpreter.execute(definition, "run", input);
  require(interpreter.cacheHits() == 1,
          "identical span-sensitive call missed its cache");
}

void testNestedSourceCache() {
  auto nestedRequest = [](const std::string &name, size_t padding) {
    const std::string text =
        std::string(padding, ' ') + "raw!{${inner!{${name}}}}";
    zap::DiagnosticEngine diagnostics(text, name);
    Lexer lexer(diagnostics);
    auto tokens = lexer.tokenize(text);
    require(!diagnostics.hadErrors() && tokens[2].sourceFragment,
            "nested cache test input failed to lex");
    auto input = zap::ctfe_bridge::syntaxSource(*tokens[2].sourceFragment);
    require(input.has_value(), "nested cache input failed conversion");
    auto result = request();
    result.invocation = {name, 1, padding + 1, padding, text.size() - padding};
    result.input = std::move(input->value);
    return result;
  };
  auto first = nestedRequest("first.zp", 0);
  auto second = nestedRequest("second.zp", 50);
  CtfeInterpreter interpreter;
  const std::string definition =
      "fun run(input: SyntaxSource) SyntaxSource { return input; }";
  require(interpreter.execute(definition, "run", first).output.has_value(),
          "nested source passthrough failed");
  auto result = interpreter.execute(definition, "run", second);
  require(result.output && interpreter.cacheHits() == 1,
          "source cache key retained call-site metadata");
  const auto &source = std::get<SyntaxSource>(*result.output);
  const auto &nested =
      *source.interpolations.front().expression.tokens[2].sourceFragment;
  require(
      source.sourceName == "second.zp" && nested.sourceName == "second.zp" &&
          nested.interpolations.front().expression.tokens.front().span.offset >
              50,
      "nested source cache leaked old interpolation spans");

  const std::string extract = "fun run(input: SyntaxSource) SyntaxExpr { "
                              "return sourceInterpolation(input, 0); }";
  require(interpreter.execute(extract, "run", first).output.has_value(),
          "nested interpolation extraction failed");
  const auto oldHits = interpreter.cacheHits();
  result = interpreter.execute(extract, "run", second);
  require(result.output && interpreter.cacheHits() == oldHits + 1 &&
              std::get<SyntaxExpr>(*result.output)
                      .syntax.tokens[2]
                      .sourceFragment->sourceName == "second.zp",
          "structured cache lost nested source capture provenance");
  result = interpreter.execute("fun run(input: SyntaxSource) SyntaxExpr { "
                               "return sourceInterpolation(input, -1); }",
                               "run", first);
  require(!result.output && result.diagnostics.front().code == "M3001",
          "source interpolation accepted a negative index");

  const std::string failing =
      "fun run(input: SyntaxSource) SyntaxExpr { panic(\"failed\"); }";
  interpreter.execute(failing, "run", first);
  result = interpreter.execute(failing, "run", second);
  require(!result.output &&
              result.diagnostics.front().span.sourceName == "second.zp" &&
              result.diagnostics.front().span.offset == 50,
          "CTFE failure retained a previous invocation's location");
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
  testCapturedCacheOutput();
  testCacheRebindingBudget();
  testSourceInput();
  testStructuredSyntax();
  testLocationObservingCache();
  testNestedSourceCache();
  testLimitsAndFailures();
  testForbiddenCapabilities();
  testForwardedOutputLimits();
  testTypeValidation();
  testBlockValuesAreNotReturns();
}
