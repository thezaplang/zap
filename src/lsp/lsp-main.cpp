#include "lsp.hpp"
#include "lsp/language_features.hpp"
#include "lsp/protocol_messages.hpp"
#include "lsp/protocol_utils.hpp"
#include "lsp/workspace.hpp"
#include <cstdio>
#include <string>

using namespace zap::lsp;

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  Server server;
  Workspace workspace;
  bool running = true;
  bool shutdownRequested = false;
  std::string line;

  while (running) {
    std::string message = server.processMessage(line);
    if (message.empty()) {
      break;
    }

    JsonRPC rpc(message);
    const JsonObject &request = rpc.object();
    auto method = getStringField(request, {"method"});
    const JsonObject *id = getField(request, "id");

    if (!method) {
      continue;
    }

    if (*method == "initialize") {
      shutdownRequested = false;

      JsonObject::Object syncOptions;
      syncOptions.emplace("openClose", JsonObject(true));
      syncOptions.emplace("change", JsonObject(int64_t(1)));

      JsonObject::Object capabilities;
      capabilities.emplace("textDocumentSync",
                           JsonObject(std::move(syncOptions)));
      capabilities.emplace("definitionProvider", JsonObject(true));
      capabilities.emplace("hoverProvider", JsonObject(true));

      JsonObject::Object completionOptions;
      completionOptions.emplace("resolveProvider", JsonObject(false));
      completionOptions.emplace(
          "triggerCharacters",
          JsonObject(JsonObject::List{
              JsonObject("."), JsonObject("_"), JsonObject("a"),
              JsonObject("b"), JsonObject("c"), JsonObject("d"),
              JsonObject("e"), JsonObject("f"), JsonObject("g"),
              JsonObject("h"), JsonObject("i"), JsonObject("j"),
              JsonObject("k"), JsonObject("l"), JsonObject("m"),
              JsonObject("n"), JsonObject("o"), JsonObject("p"),
              JsonObject("q"), JsonObject("r"), JsonObject("s"),
              JsonObject("t"), JsonObject("u"), JsonObject("v"),
              JsonObject("w"), JsonObject("x"), JsonObject("y"),
              JsonObject("z"), JsonObject("A"), JsonObject("B"),
              JsonObject("C"), JsonObject("D"), JsonObject("E"),
              JsonObject("F"), JsonObject("G"), JsonObject("H"),
              JsonObject("I"), JsonObject("J"), JsonObject("K"),
              JsonObject("L"), JsonObject("M"), JsonObject("N"),
              JsonObject("O"), JsonObject("P"), JsonObject("Q"),
              JsonObject("R"), JsonObject("S"), JsonObject("T"),
              JsonObject("U"), JsonObject("V"), JsonObject("W"),
              JsonObject("X"), JsonObject("Y"), JsonObject("Z")}));
      capabilities.emplace("completionProvider",
                           JsonObject(std::move(completionOptions)));

      JsonObject::Object signatureHelpOptions;
      signatureHelpOptions.emplace(
          "triggerCharacters",
          JsonObject(JsonObject::List{JsonObject("("), JsonObject(",")}));
      capabilities.emplace("signatureHelpProvider",
                           JsonObject(std::move(signatureHelpOptions)));

      JsonObject::Object serverInfo;
      serverInfo.emplace("name", JsonObject("zap-lsp"));

      JsonObject::Object result;
      result.emplace("capabilities", JsonObject(std::move(capabilities)));
      result.emplace("serverInfo", JsonObject(std::move(serverInfo)));

      server.sendMessage(makeResponse(id, JsonObject(std::move(result))));
    } else if (*method == "initialized") {
      continue;
    } else if (*method == "shutdown") {
      shutdownRequested = true;
      server.sendMessage(makeResponse(id, JsonObject(nullptr)));
    } else if (*method == "exit") {
      running = false;
    } else if (*method == "textDocument/didOpen") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto text = getStringField(request, {"params", "textDocument", "text"});
      auto version =
          getIntegerField(request, {"params", "textDocument", "version"})
              .value_or(0);

      if (uri && text) {
        auto path = uriToPath(*uri);
        if (path) {
          workspace.open(*uri, *path, *text, version);
          publishAnalysis(server, workspace.analyze(*uri));
        }
      }
    } else if (*method == "textDocument/didChange") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto version =
          getIntegerField(request, {"params", "textDocument", "version"})
              .value_or(0);
      const JsonObject *changes =
          getPath(request, {"params", "contentChanges"});

      if (uri && changes && changes->isList() &&
          !changes->getAsList().empty()) {
        const JsonObject &lastChange = changes->getAsList().back();
        auto text = getStringField(lastChange, {"text"});
        if (text && workspace.contains(*uri)) {
          workspace.update(*uri, *text, version);
          publishAnalysis(server, workspace.analyze(*uri));
        }
      }
    } else if (*method == "textDocument/didClose") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      if (uri) {
        workspace.close(*uri);
        server.sendMessage(makePublishDiagnostics(*uri, {}));
      }
    } else if (*method == "textDocument/completion") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject::List items;
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            items = makeCompletionItems(*uri, document->text, *project, offset);
          }
        }
        server.sendMessage(makeResponse(id, JsonObject(std::move(items))));
      }
    } else if (*method == "textDocument/definition") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            auto symbol = resolveDefinition(workspace, *uri, *project, offset);
            if (symbol) {
              result = makeLocation(symbol->uri, symbol->span);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else if (*method == "textDocument/hover") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            auto hover = resolveHover(document->text, *uri, *project, offset);
            if (hover) {
              result = makeHover(*hover);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else if (*method == "textDocument/signatureHelp") {
      auto uri = getStringField(request, {"params", "textDocument", "uri"});
      auto line = getIntegerField(request, {"params", "position", "line"});
      auto character =
          getIntegerField(request, {"params", "position", "character"});
      if (id && uri && line && character) {
        JsonObject result(nullptr);
        if (const auto *document = workspace.document(*uri)) {
          auto project = workspace.loadProject(*uri, true);
          if (project) {
            size_t offset =
                offsetFromPosition(document->text, *line, *character);
            int64_t activeParameter = 0;
            auto signatures = resolveSignatures(document->text, *uri, *project,
                                                offset, activeParameter);
            if (!signatures.empty()) {
              int64_t activeSignature =
                  chooseActiveSignature(signatures, activeParameter);
              result = makeSignatureHelp(signatures, activeSignature,
                                         activeParameter);
            }
          }
        }
        server.sendMessage(makeResponse(id, std::move(result)));
      }
    } else {
      if (id) {
        JsonObject::Object error;
        error.emplace("code", JsonObject(int64_t(JsonRPC::MethodNotFound)));
        error.emplace("message", JsonObject("Method not found"));

        JsonObject::Object response;
        response.emplace("jsonrpc", JsonObject("2.0"));
        response.emplace("id", *id);
        response.emplace("error", JsonObject(std::move(error)));
        server.sendMessage(JsonObject(std::move(response)));
      }
    }

    server.send();
  }

  return shutdownRequested ? 0 : 1;
}
