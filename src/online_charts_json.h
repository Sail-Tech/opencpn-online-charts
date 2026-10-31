#ifndef ONLINE_CHARTS_JSON_H_
#define ONLINE_CHARTS_JSON_H_

#include <string>
#include <utility>
#include <vector>

namespace online_charts {

// Minimal JSON reader for the Open-Meteo responses: objects, arrays, numbers,
// strings, booleans and null. Avoids a JSON library dependency on every
// platform the plugin is built for.
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  const JsonValue* Get(const char* key) const;
  bool IsNumber() const { return type == Type::kNumber; }
  bool IsArray() const { return type == Type::kArray; }
  bool IsObject() const { return type == Type::kObject; }
};

// Returns false for malformed input or nesting deeper than 64 levels.
bool ParseJson(const std::string& text, JsonValue* out);

}  // namespace online_charts

#endif  // ONLINE_CHARTS_JSON_H_
