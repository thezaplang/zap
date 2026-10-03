#include "frontend/module_outline.hpp"
#include "lexer/lexer.hpp"
#include "utils/diagnostics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testOutlineDiscoversOnlyTopLevelSyntax() {
  const std::string source = R"(
pub import "first.zp" as first { answer as value };
fun main() Int {
  return later!(1);
}
pub macro later($arg: expr) {
  { import "hidden.zp"; $arg }
}
import "second.zp";
)";
  zap::DiagnosticEngine diagnostics(source, "outline.zp");
  Lexer lexer(diagnostics);
  auto tokens = lexer.tokenize(source);
  auto outline = zap::frontend::ModuleOutline::scan(tokens, diagnostics);

  require(!diagnostics.hadErrors(), "valid outline produced diagnostics");
  require(outline.imports.size() == 2 &&
              outline.imports[0]->path == "first.zp" &&
              outline.imports[1]->path == "second.zp",
          "outline did not discover top-level imports in source order");
  require(outline.imports[0]->visibility_ == Visibility::Public &&
              outline.imports[0]->moduleAlias == "first" &&
              outline.imports[0]->bindings.size() == 1 &&
              outline.imports[0]->bindings[0].localName == "value",
          "outline did not preserve import metadata");
  require(outline.macros.size() == 1 &&
              outline.macros[0].name.value == "later" &&
              outline.macros[0].visibility == Visibility::Public,
          "outline did not discover a forward-declared macro");
  require(!outline.hasImportPath("hidden.zp"),
          "outline treated a macro template import as a module dependency");
}

void testMalformedImportDoesNotHideLaterDeclaration() {
  const std::string source = "import \"broken.zp\"\n"
                             "pub import \"helper.zp\";\n"
                             "macro later() {}\n";
  zap::DiagnosticEngine diagnostics(source, "outline.zp");
  Lexer lexer(diagnostics);
  auto outline =
      zap::frontend::ModuleOutline::scan(lexer.tokenize(source), diagnostics);

  require(outline.imports.size() == 1 &&
              outline.imports[0]->path == "helper.zp" &&
              outline.macros.size() == 1,
          "malformed import hid a later top-level declaration");
}

void testHelperAttributes() {
  const std::string source = R"(
@other(ctfe) fun ordinary() Int { return 0; }
@ctfe pub fun one() Int { return 1; }
@{ctfe} fun two() Int { return 2; }
)";
  zap::DiagnosticEngine diagnostics(source, "helpers.zp");
  Lexer lexer(diagnostics);
  const auto outline =
      zap::frontend::ModuleOutline::scan(lexer.tokenize(source), diagnostics);
  require(!diagnostics.hadErrors() && outline.functions.size() == 3 &&
              !outline.functions[0].ctfeOnly && outline.functions[1].ctfeOnly &&
              outline.functions[2].ctfeOnly &&
              outline.functions[1].visibility == Visibility::Public,
          "outline confused attribute arguments with @ctfe or lost grouped "
          "attributes");
}

} // namespace

int main() {
  testOutlineDiscoversOnlyTopLevelSyntax();
  testMalformedImportDoesNotHideLaterDeclaration();
  testHelperAttributes();
  return 0;
}
