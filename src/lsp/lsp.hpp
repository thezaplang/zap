#pragma once

#include <cstdint>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <string>
#include <vector>

namespace zap::lsp{

class JsonObject{
public:
  using Object = std::unordered_map<std::string_view, JsonObject>;
  using List = std::vector<JsonObject>;
  using Integer = int64_t;
  using String = std::string_view;
  using Boolean = bool;
  using Null = std::nullptr_t;

  enum class ObjectType{
    OBJECT_T, LIST_T, INTEGER_T, STRING_T, BOOLEAN_T, NULL_T
  };
private:
  std::variant<Object, List, Integer, String, Boolean, Null> storage;
public:

  ObjectType getType() const noexcept{
    if(isObject()) return ObjectType::OBJECT_T;
    if(isList()) return ObjectType::LIST_T;
    if(isInteger()) return ObjectType::INTEGER_T;
    if(isString()) return ObjectType::STRING_T;
    if(isBoolean()) return ObjectType::BOOLEAN_T;
    return ObjectType::NULL_T;
  }

  Object& getAsObject(){
    return std::get<Object>(storage);
  }

  List& getAsList(){
    return std::get<List>(storage);
  }

  const Object& getAsObject() const{
    return std::get<Object>(storage);
  }

  const List& getAsList() const{
    return std::get<List>(storage);
  }

  Integer getAsInteger() const{
    return std::get<Integer>(storage);
  }

  String getAsString() const{
    return std::get<String>(storage);
  }

  Boolean getAsBoolean() const{
    return std::get<Boolean>(storage);
  }

  Null getAsNull() const{
    return std::get<Null>(storage);
  }

  bool isObject() const noexcept{
    return std::holds_alternative<Object>(storage);
  }

  bool isList() const noexcept{
    return std::holds_alternative<List>(storage);
  }

  bool isInteger() const noexcept{
    return std::holds_alternative<Integer>(storage);
  }

  bool isString() const noexcept{
    return std::holds_alternative<String>(storage);
  }

  bool isBoolean() const noexcept{
    return std::holds_alternative<Boolean>(storage);
  }

  bool isNull() const noexcept{
    return std::holds_alternative<Null>(storage);
  }

  JsonObject() noexcept : storage(nullptr) {}
  JsonObject(Null) noexcept : storage(nullptr) {}
  JsonObject(Boolean boolean) noexcept : storage(boolean) {}
  JsonObject(String sv) noexcept : storage(sv) {}
  JsonObject(Integer i) noexcept : storage(i) {}
  JsonObject(List list) : storage(std::move(list)) {}
  JsonObject(Object object) : storage(std::move(object)) {}

  JsonObject(const JsonObject&) = delete;
  JsonObject& operator=(const JsonObject&) = delete;

  JsonObject(JsonObject&&) noexcept = default;
  JsonObject& operator=(JsonObject&&) noexcept = default;

  ~JsonObject() noexcept = default;
};

class JsonParser{
public:
  /// @brief Parses the provided json.
  /// @return Json object.
  static JsonObject parse(std::string_view);
  static void toString(std::string& dest, JsonObject& j);
};

/// @brief The JSON RPC class.
class JsonRPC{
public:
  enum ErrorCodes{
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    /// Zap-lsp's errors range: -32000 to -32099
  };
private:
  std::string_view originalMessage;
  std::string heapAllocated;
  JsonObject rpc;
public:
  /// @brief Create a JSON RPC class from a message.
  /// @param message JSON RPC message.
  JsonRPC(const std::string& message) : originalMessage(message), rpc(JsonParser::parse(message)) {}
  /// @brief Create a JSON RPC class from JsonObject.
  /// @param obj JSON Object.
  JsonRPC(JsonObject obj) noexcept : rpc(std::move(obj)) {}

  /// @brief Returns it as a string view.
  std::string_view getStr();

  ~JsonRPC() noexcept = default;
};

/// @brief The main class for the LSP server.
class Server{
  std::string buffer;

  /// @brief Used internally to send JSON RPC.
  /// @param message JSON RPC.
  ///
  /// This adds content length to the message. 
  void sendMessageRaw(std::string_view message);
public:
  Server() = default;

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Server(Server&&) = default;
  Server& operator=(Server&&) = default;

  ~Server() noexcept = default;

  /// @brief Used for sendMessage().
  enum class MessageType{
    Error = 1,
    Warning = 2,
    Info = 3,
    Log = 4
  };
  /// @brief Request the client to show a message to the user.
  void logMessage(MessageType type, std::string_view message){
    JsonObject rpc(JsonObject::Object{});

    rpc.getAsObject()["jsonrpc"] = JsonObject(std::string_view("2.0"));
    rpc.getAsObject()["method"] = JsonObject(std::string_view("window/logMessage"));

    JsonObject params(JsonObject::Object{});
    params.getAsObject()["type"] = JsonObject(JsonObject::Integer(type));
    params.getAsObject()["message"] = JsonObject(message);

    rpc.getAsObject()["params"] = std::move(params);

    sendMessage(rpc);
  }

  /// @brief Should be called in the while loop in main.
  /// @param line String parameter outside of main, used as an arg for performance.
  /// @return JSON Message.
  std::string processMessage(std::string& line);

  /// @brief Sends all queued messages.
  void send(){
    std::cout << buffer;
    buffer.clear();
  }

  /// @brief Queues a message.
  /// @param msg The message to queue.
  ///
  /// Use send() to flush the messages.
  void sendMessage(JsonRPC& msg){
    sendMessageRaw(msg.getStr());
  }

  /// @brief Queues a json message.
  /// @param msg The message to queue.
  void sendMessage(JsonObject& msg){
    std::string s;
    JsonParser::toString(s, msg);
    sendMessageRaw(s);
  }
};

}