#include "lsp/protocol_messages.hpp"

#include <algorithm>

namespace zap::lsp {

JsonObject makeResponse(const JsonObject *id, JsonObject result) {
  JsonObject::Object object;
  object.emplace("jsonrpc", JsonObject("2.0"));
  object.emplace("id", id ? *id : JsonObject(nullptr));
  object.emplace("result", std::move(result));
  return JsonObject(std::move(object));
}

JsonObject makeNotification(std::string method, JsonObject params) {
  JsonObject::Object object;
  object.emplace("jsonrpc", JsonObject("2.0"));
  object.emplace("method", JsonObject(std::move(method)));
  object.emplace("params", std::move(params));
  return JsonObject(std::move(object));
}

JsonObject makeRange(const zap::DiagnosticRange &range) {
  JsonObject::Object start;
  start.emplace("line", JsonObject(static_cast<int64_t>(
                            range.start.line > 0 ? range.start.line - 1 : 0)));
  start.emplace("character",
                JsonObject(static_cast<int64_t>(
                    range.start.column > 0 ? range.start.column - 1 : 0)));

  JsonObject::Object end;
  end.emplace("line", JsonObject(static_cast<int64_t>(
                          range.end.line > 0 ? range.end.line - 1 : 0)));
  end.emplace("character",
              JsonObject(static_cast<int64_t>(
                  range.end.column > 0 ? range.end.column - 1 : 0)));

  JsonObject::Object object;
  object.emplace("start", JsonObject(std::move(start)));
  object.emplace("end", JsonObject(std::move(end)));
  return JsonObject(std::move(object));
}

JsonObject makeLocation(const std::string &uri, const SourceSpan &span) {
  zap::DiagnosticRange range{{span.line, span.column, span.offset},
                             {span.line,
                              span.column + std::max<size_t>(span.length, 1),
                              span.offset + span.length}};
  JsonObject::Object object;
  object.emplace("uri", JsonObject(uri));
  object.emplace("range", makeRange(range));
  return JsonObject(std::move(object));
}

JsonObject makeCompletionItem(const LspSymbol &symbol,
                              const std::string &detail) {
  JsonObject::Object item;
  item.emplace("label", JsonObject(symbol.name));
  item.emplace("kind", JsonObject(symbol.completionKind));
  if (!detail.empty()) {
    item.emplace("detail", JsonObject(detail));
  }
  return JsonObject(std::move(item));
}

JsonObject makeHover(const HoverInfo &hover) {
  JsonObject::Object contents;
  contents.emplace("kind", JsonObject("markdown"));
  contents.emplace("value", JsonObject("```" + hover.language + "\n" +
                                       hover.value + "\n```"));

  JsonObject::Object result;
  result.emplace("contents", JsonObject(std::move(contents)));
  return JsonObject(std::move(result));
}

JsonObject makeSignatureHelp(const std::vector<LspSignature> &signatures,
                             int64_t activeSignature, int64_t activeParameter) {
  JsonObject::List signatureItems;
  signatureItems.reserve(signatures.size());
  for (const auto &signature : signatures) {
    JsonObject::List parameters;
    parameters.reserve(signature.parameters.size());
    for (const auto &param : signature.parameters) {
      JsonObject::Object parameter;
      parameter.emplace("label", JsonObject(param));
      parameters.push_back(JsonObject(std::move(parameter)));
    }

    JsonObject::Object sig;
    sig.emplace("label", JsonObject(signature.label));
    sig.emplace("parameters", JsonObject(std::move(parameters)));
    signatureItems.push_back(JsonObject(std::move(sig)));
  }

  int64_t clampedSignature =
      signatures.empty()
          ? 0
          : std::max<int64_t>(0, std::min<int64_t>(activeSignature,
                                                   static_cast<int64_t>(
                                                       signatures.size() - 1)));
  int64_t clampedParameter = 0;
  if (!signatures.empty()) {
    clampedParameter = std::max<int64_t>(
        0, std::min<int64_t>(
               activeParameter,
               static_cast<int64_t>(
                   signatures[clampedSignature].parameters.empty()
                       ? 0
                       : signatures[clampedSignature].parameters.size() - 1)));
  }

  JsonObject::Object result;
  result.emplace("signatures", JsonObject(std::move(signatureItems)));
  result.emplace("activeSignature", JsonObject(clampedSignature));
  result.emplace("activeParameter", JsonObject(clampedParameter));
  return JsonObject(std::move(result));
}

int64_t toLspSeverity(zap::DiagnosticLevel level) {
  switch (level) {
  case zap::DiagnosticLevel::Error:
    return 1;
  case zap::DiagnosticLevel::Warning:
    return 2;
  case zap::DiagnosticLevel::Note:
    return 3;
  }
  return 1;
}

JsonObject makeDiagnostic(const zap::Diagnostic &diagnostic) {
  JsonObject::Object object;
  object.emplace("range", makeRange(diagnostic.range));
  object.emplace("severity", JsonObject(toLspSeverity(diagnostic.level)));
  object.emplace("source", JsonObject("zap-lsp"));
  object.emplace("message", JsonObject(diagnostic.message));
  if (!diagnostic.code.empty()) {
    object.emplace("code", JsonObject(diagnostic.code));
  }
  return JsonObject(std::move(object));
}

JsonObject
makePublishDiagnostics(std::string uri,
                       const std::vector<zap::Diagnostic> &diagnostics) {
  JsonObject::List items;
  items.reserve(diagnostics.size());
  for (const auto &diagnostic : diagnostics) {
    items.push_back(makeDiagnostic(diagnostic));
  }

  JsonObject::Object params;
  params.emplace("uri", JsonObject(std::move(uri)));
  params.emplace("diagnostics", JsonObject(std::move(items)));
  return makeNotification("textDocument/publishDiagnostics",
                          JsonObject(std::move(params)));
}

void publishAnalysis(Server &server, const AnalysisResult &result) {
  for (const auto &[uri, diagnostics] : result.diagnosticsByUri) {
    server.sendMessage(makePublishDiagnostics(uri, diagnostics));
  }
}

} // namespace zap::lsp
