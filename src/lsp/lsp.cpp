#include "lsp.hpp"
#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>

namespace zap::lsp{

static inline JsonObject parseJson(const char*& it, const char* end);

static inline JsonObject parseJsonString(const char*& it, const char* end){
  std::string_view str;
  const char* start = it;

  bool escape = false;
  while(it != end){

    if(escape) escape = false;

    if(*it == '\\') escape = true;

    if(*it == '"' && !escape){
      str = std::string_view(start, it - start);
      it++;
      break;
    }

    it++;
  }

  return JsonObject(str);
}

static inline std::string_view parseJsonGetWord(const char*& it, const char* end){
  const char* start = it;

  while(it != end && isalnum((unsigned char)*it))
    it++;

  return std::string_view(start, it - start);
}

static inline void jsonSkipWhitespace(const char*& it, const char* end){
  while(it != end && std::isspace((unsigned char)*it)) it++;
}

static inline JsonObject parseJsonNumber(const char*& it, const char* end, bool negative){
  std::string_view num = parseJsonGetWord(it, end);
  if(num.empty()) return JsonObject(nullptr);

  JsonObject::Integer integer;
  bool err = std::from_chars(num.begin(), num.end(), integer).ec != std::errc();

  return err ? JsonObject(nullptr) : JsonObject(negative ? -integer : integer);
}

static inline JsonObject parseKeyword(const char*& it, const char* end){
  std::string_view kwd = parseJsonGetWord(it, end);

  if(kwd == "false") return JsonObject(false);
  else if(kwd == "true") return JsonObject(true);
  else return JsonObject(nullptr); // Even if the keyword isn't null its still null.
}

static inline JsonObject parseJsonList(const char*& it, const char* end){
  JsonObject list(JsonObject::List{});

  bool comma = false;

  while(it != end){
    jsonSkipWhitespace(it, end);
    if(it == end) return JsonObject(nullptr);

    if(*it == ']'){
      if(comma) return JsonObject(nullptr);
      it++;
      break;
    }

    if(*it == ','){
      if(comma || list.getAsList().empty()) return JsonObject(nullptr);
      it++;
      comma = true;
      continue;
    }

    if(!list.getAsList().empty() && !comma) return JsonObject(nullptr);

    list.getAsList().emplace_back(parseJson(it, end));

    if(comma) comma = false;
  }

  return list;
}

static inline JsonObject parseJsonObject(const char*& it, const char* end){
  JsonObject object(JsonObject::Object{});

  bool comma = false;

  while(it != end){
    jsonSkipWhitespace(it, end);
    if(it == end) return JsonObject(nullptr);

    if(*it == '}'){
      if(comma) return JsonObject(nullptr);
      it++;
      break;
    }

    if(*it == ','){
      if(comma || object.getAsObject().empty()) return JsonObject(nullptr);
      it++;
      comma = true;
      continue;
    }

    if(!object.getAsObject().empty() && !comma) return JsonObject(nullptr);

    JsonObject parsed = parseJson(it, end);
    if(!parsed.isString()) return JsonObject(nullptr);

    jsonSkipWhitespace(it, end);
    if(it == end || *it++ != ':') return JsonObject(nullptr);

    auto [_, res] = object.getAsObject().try_emplace(
      parsed.getAsString(), 
      parseJson(it, end)
    );

    if(!res) return JsonObject(nullptr);
  }

  return object;
}

static inline JsonObject parseJson(const char*& it, const char* end){
  jsonSkipWhitespace(it, end);
  if(it >= end) return JsonObject(nullptr);

  switch(*it){
    case '{':
      return parseJsonObject(++it, end);
    case '[':
      return parseJsonList(++it, end);
    case 't':
      [[fallthrough]];
    case 'f':
      [[fallthrough]];
    case 'n':
      return parseKeyword(it, end);
    case '0':
      [[fallthrough]];
    case '1':
      [[fallthrough]];
    case '2':
      [[fallthrough]];
    case '3':
      [[fallthrough]];
    case '4':
      [[fallthrough]];
    case '5':
      [[fallthrough]];
    case '6':
      [[fallthrough]];
    case '7':
      [[fallthrough]];
    case '8':
      [[fallthrough]];
    case '9':
      return parseJsonNumber(it, end, false);
    case '-':
      return parseJsonNumber(++it, end, true);
    case '"':
      return parseJsonString(++it, end);
    default: return JsonObject(nullptr);
  }
}

static inline void strJsonStr(std::string& s, JsonObject::String sv){
  s += '"';

  s += sv;

  s += '"';
}

static inline void strJsonNull(std::string& s){
  s += "null";
}

static inline void strJsonBool(std::string& s, JsonObject::Boolean b){
  s += b ? "true" : "false";
}

static inline void strJsonInteger(std::string& s, JsonObject::Integer i){
  s += std::to_string(i);
}

static inline void strJsonList(std::string& s, JsonObject::List& list){
  s += '[';
  for(size_t i = 0; i < list.size(); i++){
    if(i){
      s += ", ";
    }
    JsonParser::toString(s, list[i]);
  }
  s += ']';
}

static inline void strJsonObject(std::string& s, JsonObject::Object& obj){
  s += '{';
  bool first = true;
  for(auto& p : obj){
    if(!first){
      s += ", ";
    }
    first = false;
    strJsonStr(s, p.first);
    s += ':';
    JsonParser::toString(s, p.second);
  }
  s += '}';
}

void JsonParser::toString(std::string& dest, JsonObject& j){
  switch(j.getType()){
    case JsonObject::ObjectType::OBJECT_T:
      strJsonObject(dest, j.getAsObject());
      break;
    case JsonObject::ObjectType::LIST_T:
      strJsonList(dest, j.getAsList());
      break;
    case JsonObject::ObjectType::INTEGER_T:
      strJsonInteger(dest, j.getAsInteger());
      break;
    case JsonObject::ObjectType::STRING_T:
      strJsonStr(dest, j.getAsString());
      break;
    case JsonObject::ObjectType::BOOLEAN_T:
      strJsonBool(dest, j.getAsBoolean());
      break;
    case JsonObject::ObjectType::NULL_T:
      strJsonNull(dest);
      break;
    default:
      return;
  }
}

std::string_view JsonRPC::getStr(){
  if(!originalMessage.empty()) return originalMessage;
  if(!heapAllocated.empty()) return heapAllocated;

  JsonParser::toString(heapAllocated, rpc);

  return heapAllocated;
}

JsonObject JsonParser::parse(std::string_view sv){
  const char* it = sv.begin();

  return parseJson(it, sv.end());
}

constexpr std::string_view prefix = "Content-Length: ";

void Server::sendMessageRaw(std::string_view message){
  buffer += prefix;

  char buff[std::numeric_limits<unsigned>::digits10 + 0x20];

  auto result = std::to_chars(buff, buff + sizeof(buff), message.length());
  
  constexpr const char* crlf = "\r\n\r\n";
  std::copy(crlf, crlf + 4, result.ptr);
  result.ptr += 4;

  buffer.append(buff, result.ptr - buff);

  buffer += message;
}

std::string Server::processMessage(std::string& line){
  unsigned content_length = 0;

  while(std::getline(std::cin, line)) {
    if(line.empty() || line == "\r") break;

    if(line.rfind(prefix, 0) == 0) {
      std::from_chars(line.data() + prefix.size(), line.data() + line.size(), content_length);
    }
  }

  std::string message(content_length, '\0');
  std::cin.read(message.data(), content_length);

  return message;
}

}