#include "lsp/request_dispatcher.hpp"

#include "lsp.hpp"
#include "lsp/document_request.hpp"
#include "lsp/language_features.hpp"
#include "lsp/protocol_codec.hpp"
#include "lsp/protocol_messages.hpp"
#include "lsp/protocol_utils.hpp"
#include "lsp/workspace.hpp"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zap::lsp {
namespace {

std::optional<std::string> requestKey(const JsonObject *id) {
  if (!id) {
    return std::nullopt;
  }
  if (id->isInteger()) {
    return "i:" + std::to_string(id->getAsInteger());
  }
  if (id->isString()) {
    return "s:" + id->getAsString();
  }
  return std::nullopt;
}

JsonObject makeCapabilities() {
  JsonObject::Object syncOptions;
  syncOptions.emplace("openClose", JsonObject(true));
  syncOptions.emplace("change", JsonObject(int64_t(1)));

  JsonObject::Object completionOptions;
  completionOptions.emplace("resolveProvider", JsonObject(false));
  JsonObject::List completionTriggers{JsonObject("."), JsonObject("_")};
  for (char ch = 'a'; ch <= 'z'; ++ch) {
    completionTriggers.emplace_back(std::string(1, ch));
  }
  for (char ch = 'A'; ch <= 'Z'; ++ch) {
    completionTriggers.emplace_back(std::string(1, ch));
  }
  completionOptions.emplace("triggerCharacters",
                            JsonObject(std::move(completionTriggers)));

  JsonObject::Object signatureHelpOptions;
  signatureHelpOptions.emplace(
      "triggerCharacters",
      JsonObject(JsonObject::List{JsonObject("("), JsonObject(",")}));

  JsonObject::Object workspaceFolders;
  workspaceFolders.emplace("supported", JsonObject(true));
  workspaceFolders.emplace("changeNotifications", JsonObject(true));
  JsonObject::Object workspace;
  workspace.emplace("workspaceFolders",
                    JsonObject(std::move(workspaceFolders)));

  JsonObject::Object capabilities;
  capabilities.emplace("textDocumentSync", JsonObject(std::move(syncOptions)));
  capabilities.emplace("definitionProvider", JsonObject(true));
  capabilities.emplace("hoverProvider", JsonObject(true));
  capabilities.emplace("completionProvider",
                       JsonObject(std::move(completionOptions)));
  capabilities.emplace("signatureHelpProvider",
                       JsonObject(std::move(signatureHelpOptions)));
  capabilities.emplace("workspace", JsonObject(std::move(workspace)));

  JsonObject::Object serverInfo;
  serverInfo.emplace("name", JsonObject("zap-lsp"));
  serverInfo.emplace("version", JsonObject(lspVersion));

  JsonObject::Object result;
  result.emplace("capabilities", JsonObject(std::move(capabilities)));
  result.emplace("serverInfo", JsonObject(std::move(serverInfo)));
  return JsonObject(std::move(result));
}

class RequestScheduler {
  Server &server_;
  Workspace workspace_;
  std::mutex queueMutex_;
  std::condition_variable queueReady_;
  std::deque<JsonObject> queue_;
  bool stopping_ = false;
  std::thread worker_;

  std::mutex cancellationMutex_;
  std::unordered_set<std::string> cancelled_;
  std::unordered_set<std::string> outstanding_;
  bool shutdownRequested_ = false;

  bool isCancelled(const std::optional<std::string> &key) {
    if (!key) {
      return false;
    }
    std::lock_guard lock(cancellationMutex_);
    return cancelled_.find(*key) != cancelled_.end();
  }

  bool finishRequest(const std::optional<std::string> &key) {
    if (!key) {
      return false;
    }
    std::lock_guard lock(cancellationMutex_);
    outstanding_.erase(*key);
    return cancelled_.erase(*key) != 0;
  }

  void sendCancelled(const JsonObject *id) {
    server_.discardPendingMessages();
    server_.sendMessage(
        makeErrorResponse(id, JsonRPC::RequestCancelled, "Request cancelled"));
    server_.send();
  }

  void execute(const JsonObject &request) {
    auto method = getStringField(request, {"method"});
    const JsonObject *id = getField(request, "id");
    if (!method) {
      if (id) {
        server_.sendMessage(
            makeErrorResponse(id, JsonRPC::InvalidRequest, "Missing method"));
      }
      return;
    }

    if (*method == "initialize") {
      shutdownRequested_ = false;
      workspace_.configure();
      server_.sendMessage(makeResponse(id, makeCapabilities()));
    } else if (*method == "initialized") {
      return;
    } else if (*method == "shutdown") {
      shutdownRequested_ = true;
      server_.sendMessage(makeResponse(id, JsonObject(nullptr)));
    } else if (*method == "exit") {
      return;
    } else if (*method == "textDocument/didOpen") {
      if (auto params = decodeOpenDocument(request)) {
        if (auto path = uriToPath(params->uri)) {
          workspace_.open(params->uri, *path, std::move(params->text),
                          params->version);
          publishAnalysis(server_, workspace_.analyze(params->uri));
        }
      }
    } else if (*method == "textDocument/didChange") {
      if (auto params = decodeChangeDocument(request);
          params && workspace_.contains(params->uri)) {
        workspace_.update(params->uri, std::move(params->text),
                          params->version);
        publishAnalysis(server_, workspace_.analyze(params->uri));
      }
    } else if (*method == "textDocument/didClose") {
      if (auto uri = decodeCloseDocument(request)) {
        workspace_.close(*uri);
        server_.sendMessage(makePublishDiagnostics(*uri, {}));
      }
    } else if (*method == "workspace/didChangeWatchedFiles") {
      if (auto changes = decodeWatchedFiles(request)) {
        std::vector<std::filesystem::path> paths;
        paths.reserve(changes->size());
        for (const auto &change : *changes) {
          if (auto path = uriToPath(change.uri)) {
            paths.push_back(std::move(*path));
          }
        }
        publishAnalysis(server_, workspace_.watchedFilesChanged(paths));
      }
    } else if (*method == "workspace/didChangeWorkspaceFolders") {
      publishAnalysis(server_, workspace_.workspaceFoldersChanged());
    } else if (*method == "textDocument/completion") {
      if (id) {
        if (auto context = documentRequestContext(workspace_, request)) {
          auto items =
              makeCompletionItems(context->uri, context->query.document->text,
                                  *context->query.project, context->offset);
          server_.sendMessage(makeResponse(id, JsonObject(std::move(items))));
        } else {
          server_.sendMessage(makeErrorResponse(id, JsonRPC::InvalidParams,
                                                "Invalid document position"));
        }
      }
    } else if (*method == "textDocument/definition") {
      JsonObject result(nullptr);
      if (id) {
        if (auto context = documentRequestContext(workspace_, request)) {
          if (auto symbol =
                  resolveDefinition(context->query.document->text, context->uri,
                                    *context->query.project, context->offset)) {
            if (auto source = workspace_.sourceForUri(symbol->uri)) {
              result = makeLocation(symbol->uri, *source, symbol->span);
            }
          }
          server_.sendMessage(makeResponse(id, std::move(result)));
        } else {
          server_.sendMessage(makeErrorResponse(id, JsonRPC::InvalidParams,
                                                "Invalid document position"));
        }
      }
    } else if (*method == "textDocument/hover") {
      JsonObject result(nullptr);
      if (id) {
        if (auto context = documentRequestContext(workspace_, request)) {
          if (auto hover =
                  resolveHover(context->query.document->text, context->uri,
                               *context->query.project, context->offset)) {
            result = makeHover(*hover);
          }
          server_.sendMessage(makeResponse(id, std::move(result)));
        } else {
          server_.sendMessage(makeErrorResponse(id, JsonRPC::InvalidParams,
                                                "Invalid document position"));
        }
      }
    } else if (*method == "textDocument/signatureHelp") {
      JsonObject result(nullptr);
      if (id) {
        if (auto context = documentRequestContext(workspace_, request)) {
          int64_t activeParameter = 0;
          auto signatures = resolveSignatures(
              context->query.document->text, context->uri,
              *context->query.project, context->offset, activeParameter);
          if (!signatures.empty()) {
            result = makeSignatureHelp(
                signatures, chooseActiveSignature(signatures, activeParameter),
                activeParameter);
          }
          server_.sendMessage(makeResponse(id, std::move(result)));
        } else {
          server_.sendMessage(makeErrorResponse(id, JsonRPC::InvalidParams,
                                                "Invalid document position"));
        }
      }
    } else if (id) {
      server_.sendMessage(
          makeErrorResponse(id, JsonRPC::MethodNotFound, "Method not found"));
    }
  }

  void workerLoop() {
    while (true) {
      JsonObject request;
      {
        std::unique_lock lock(queueMutex_);
        queueReady_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;
        }
        request = std::move(queue_.front());
        queue_.pop_front();
      }

      const JsonObject *id = getField(request, "id");
      auto key = requestKey(id);
      if (isCancelled(key)) {
        finishRequest(key);
        sendCancelled(id);
        continue;
      }

      execute(request);
      if (finishRequest(key)) {
        sendCancelled(id);
      } else {
        server_.send();
      }
    }
  }

public:
  explicit RequestScheduler(Server &server) : server_(server) {
    worker_ = std::thread([this] { workerLoop(); });
  }

  RequestScheduler(const RequestScheduler &) = delete;
  RequestScheduler &operator=(const RequestScheduler &) = delete;

  ~RequestScheduler() { stop(); }

  bool submit(JsonObject request) {
    auto method = getStringField(request, {"method"});
    if (method && *method == "$/cancelRequest") {
      auto key = requestKey(getPath(request, {"params", "id"}));
      if (key) {
        std::lock_guard lock(cancellationMutex_);
        if (outstanding_.find(*key) != outstanding_.end()) {
          cancelled_.insert(std::move(*key));
        }
      }
      return true;
    }

    const bool keepReading = !method || *method != "exit";
    if (auto key = requestKey(getField(request, "id"))) {
      std::lock_guard lock(cancellationMutex_);
      outstanding_.insert(std::move(*key));
    }
    {
      std::lock_guard lock(queueMutex_);
      queue_.push_back(std::move(request));
    }
    queueReady_.notify_one();
    return keepReading;
  }

  void stop() {
    {
      std::lock_guard lock(queueMutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
    }
    queueReady_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  bool shutdownRequested() const { return shutdownRequested_; }
};

} // namespace

int runRequestDispatcher() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Server server;
  RequestScheduler scheduler(server);
  std::string line;

  while (true) {
    std::string message = server.processMessage(line);
    if (message.empty()) {
      break;
    }
    JsonRPC rpc(message);
    if (!scheduler.submit(rpc.object())) {
      break;
    }
  }

  scheduler.stop();
  return scheduler.shutdownRequested() ? 0 : 1;
}

} // namespace zap::lsp
