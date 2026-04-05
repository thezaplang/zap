#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace zap::lsp {

class JsonObject {
public:
  using Object = std::unordered_map<std::string, JsonObject>;
  using List = std::vector<JsonObject>;
  using Integer = int64_t;
  using String = std::string;
  using Boolean = bool;
  using Null = std::nullptr_t;

  enum class ObjectType { OBJECT_T, LIST_T, INTEGER_T, STRING_T, BOOLEAN_T, NULL_T };

private:
  std::variant<Object, List, Integer, String, Boolean, Null> storage;

public:
  JsonObject() noexcept : storage(nullptr) {}
  JsonObject(Null) noexcept : storage(nullptr) {}
  JsonObject(Boolean boolean) noexcept : storage(boolean) {}
  JsonObject(Integer integer) noexcept : storage(integer) {}
  JsonObject(const char *string) : storage(String(string ? string : "")) {}
  JsonObject(std::string_view string) : storage(String(string)) {}
  JsonObject(std::string string) : storage(std::move(string)) {}
  JsonObject(List list) : storage(std::move(list)) {}
  JsonObject(Object object) : storage(std::move(object)) {}

  JsonObject(const JsonObject &) = default;
  JsonObject &operator=(const JsonObject &) = default;
  JsonObject(JsonObject &&) noexcept = default;
  JsonObject &operator=(JsonObject &&) noexcept = default;
  ~JsonObject() noexcept = default;

  ObjectType getType() const noexcept;

  Object &getAsObject();
  List &getAsList();
  const Object &getAsObject() const;
  const List &getAsList() const;
  Integer getAsInteger() const;
  const String &getAsString() const;
  Boolean getAsBoolean() const;
  Null getAsNull() const;

  bool isObject() const noexcept;
  bool isList() const noexcept;
  bool isInteger() const noexcept;
  bool isString() const noexcept;
  bool isBoolean() const noexcept;
  bool isNull() const noexcept;
};

class JsonParser {
public:
  static JsonObject parse(std::string_view);
  static void toString(std::string &dest, const JsonObject &json);
};

class JsonRPC {
public:
  enum ErrorCodes {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
  };

private:
  std::string serialized_;
  JsonObject rpc_;

public:
  JsonRPC(const std::string &message);
  JsonRPC(JsonObject object) noexcept;

  std::string_view getStr();
  const JsonObject &object() const noexcept { return rpc_; }
};

class Server {
  std::string buffer;

  void sendMessageRaw(std::string_view message);

public:
  enum class MessageType { Error = 1, Warning = 2, Info = 3, Log = 4 };

  Server() = default;
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = default;
  Server &operator=(Server &&) = default;
  ~Server() noexcept = default;

  void logMessage(MessageType type, std::string_view message);
  std::string processMessage(std::string &line);

  void send() {
    std::cout << buffer;
    buffer.clear();
  }

  void sendMessage(JsonRPC &message) { sendMessageRaw(message.getStr()); }
  void sendMessage(const JsonObject &message);
  void sendMessage(std::string_view rawJson) { sendMessageRaw(rawJson); }
};

} // namespace zap::lsp
