#include "online_charts_json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace online_charts {
namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : p_(text.c_str()), end_(text.c_str() + text.size()) {}

  bool Document(JsonValue* out) {
    if (!Value(out, 0)) return false;
    Space();
    return p_ == end_;
  }

 private:
  void Space() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool Literal(const char* word) {
    const size_t length = std::strlen(word);
    if (static_cast<size_t>(end_ - p_) < length || std::strncmp(p_, word, length) != 0) return false;
    p_ += length;
    return true;
  }

  static void AppendUtf8(unsigned code, std::string* out) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool String(std::string* out) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ != '\\') {
        out->push_back(*p_++);
        continue;
      }
      if (++p_ >= end_) return false;
      const char c = *p_++;
      switch (c) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          if (end_ - p_ < 4) return false;
          char hex[5] = {p_[0], p_[1], p_[2], p_[3], 0};
          char* stop = nullptr;
          const unsigned long code = std::strtoul(hex, &stop, 16);
          if (stop != hex + 4) return false;
          p_ += 4;
          AppendUtf8(static_cast<unsigned>(code), out);
          break;
        }
        default:
          return false;
      }
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }

  // Parsed by hand rather than with strtod, which follows the process locale
  // and would stop at the '.' under a locale with comma decimals.
  bool Number(double* out) {
    bool negative = false;
    if (p_ < end_ && (*p_ == '-' || *p_ == '+')) negative = *p_++ == '-';
    double value = 0.0;
    bool digits = false;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
      value = value * 10.0 + (*p_++ - '0');
      digits = true;
    }
    if (p_ < end_ && *p_ == '.') {
      ++p_;
      double scale = 0.1;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
        value += (*p_++ - '0') * scale;
        scale *= 0.1;
        digits = true;
      }
    }
    if (!digits) return false;
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      bool negative_exponent = false;
      if (p_ < end_ && (*p_ == '-' || *p_ == '+')) negative_exponent = *p_++ == '-';
      int exponent = 0;
      bool exponent_digits = false;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9' && exponent < 400) {
        exponent = exponent * 10 + (*p_++ - '0');
        exponent_digits = true;
      }
      if (!exponent_digits) return false;
      value *= std::pow(10.0, negative_exponent ? -exponent : exponent);
    }
    *out = negative ? -value : value;
    return true;
  }

  bool Value(JsonValue* out, int depth) {
    if (depth > 64) return false;
    Space();
    if (p_ >= end_) return false;
    const char c = *p_;
    if (c == '{') {
      ++p_;
      out->type = JsonValue::Type::kObject;
      Space();
      if (p_ < end_ && *p_ == '}') {
        ++p_;
        return true;
      }
      while (true) {
        Space();
        std::pair<std::string, JsonValue> member;
        if (!String(&member.first)) return false;
        Space();
        if (p_ >= end_ || *p_ != ':') return false;
        ++p_;
        if (!Value(&member.second, depth + 1)) return false;
        out->members.push_back(std::move(member));
        Space();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == '}') {
          ++p_;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      ++p_;
      out->type = JsonValue::Type::kArray;
      Space();
      if (p_ < end_ && *p_ == ']') {
        ++p_;
        return true;
      }
      while (true) {
        JsonValue item;
        if (!Value(&item, depth + 1)) return false;
        out->items.push_back(std::move(item));
        Space();
        if (p_ < end_ && *p_ == ',') {
          ++p_;
          continue;
        }
        if (p_ < end_ && *p_ == ']') {
          ++p_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      out->type = JsonValue::Type::kString;
      return String(&out->text);
    }
    if (Literal("true")) {
      out->type = JsonValue::Type::kBool;
      out->boolean = true;
      return true;
    }
    if (Literal("false")) {
      out->type = JsonValue::Type::kBool;
      return true;
    }
    if (Literal("null")) {
      out->type = JsonValue::Type::kNull;
      return true;
    }
    out->type = JsonValue::Type::kNumber;
    return Number(&out->number);
  }

  const char* p_;
  const char* end_;
};

}  // namespace

const JsonValue* JsonValue::Get(const char* key) const {
  for (const auto& member : members) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

bool ParseJson(const std::string& text, JsonValue* out) {
  *out = JsonValue();
  Parser parser(text);
  return parser.Document(out);
}

}  // namespace online_charts
