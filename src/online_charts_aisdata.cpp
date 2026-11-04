#include "online_charts_aisdata.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "online_charts_json.h"

namespace online_charts {

namespace {

// AIS message types that report a position, which is stale after a while:
// class A (1-3), base station (4), SAR aircraft (9), class B (18, 19), aid to
// navigation (21) and long range (27).
bool IsPositionMessage(int id) {
  return id == 1 || id == 2 || id == 3 || id == 4 || id == 9 || id == 18 || id == 19 ||
         id == 21 || id == 27;
}

int MessageIdFromName(const std::string& name) {
  if (name == "PositionReport") return 1;
  if (name == "BaseStationReport") return 4;
  if (name == "ShipStaticData") return 5;
  if (name == "StandardSearchAndRescueAircraftReport") return 9;
  if (name == "StandardClassBPositionReport") return 18;
  if (name == "ExtendedClassBPositionReport") return 19;
  if (name == "AidsToNavigationReport") return 21;
  if (name == "StaticDataReport") return 24;
  if (name == "LongRangeAisBroadcastMessage") return 27;
  return 0;
}

double Number(const JsonValue* value, double fallback = 0.0) {
  return value && value->IsNumber() && std::isfinite(value->number) ? value->number : fallback;
}

std::string Text(const JsonValue* value) {
  return value && value->type == JsonValue::Type::kString ? value->text : std::string();
}

class BitWriter {
 public:
  void Put(unsigned long long value, int bits) {
    for (int i = bits - 1; i >= 0; --i) bits_.push_back(((value >> i) & 1ULL) != 0);
  }
  void PutInt(double value, int bits, long long max) {
    long long v = std::isfinite(value) ? static_cast<long long>(std::llround(value)) : 0;
    v = std::max(0LL, std::min(max, v));
    Put(static_cast<unsigned long long>(v), bits);
  }
  // AIS six-bit text, upper case, padded with '@'.
  void PutText(const std::string& text, int characters) {
    for (int i = 0; i < characters; ++i) {
      int c = i < static_cast<int>(text.size()) ? static_cast<unsigned char>(text[i]) : '@';
      c = std::toupper(c);
      int code;
      if (c >= 64 && c < 96) {
        code = c - 64;
      } else if (c >= 32 && c < 64) {
        code = c;
      } else {
        code = 32;  // Anything else becomes a space.
      }
      Put(static_cast<unsigned long long>(code), 6);
    }
  }
  // Armoured payload and the number of fill bits.
  std::string Payload(int* fill) const {
    std::string out;
    const size_t chars = (bits_.size() + 5) / 6;
    *fill = static_cast<int>(chars * 6 - bits_.size());
    for (size_t c = 0; c < chars; ++c) {
      int value = 0;
      for (size_t b = 0; b < 6; ++b) {
        const size_t index = c * 6 + b;
        value = (value << 1) | (index < bits_.size() && bits_[index] ? 1 : 0);
      }
      out.push_back(static_cast<char>(value < 40 ? value + 48 : value + 56));
    }
    return out;
  }

 private:
  std::vector<bool> bits_;
};

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
long long DaysFromCivil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

}  // namespace

std::vector<std::string> SseParser::Feed(const std::string& chunk) {
  std::vector<std::string> events;
  for (char c : chunk) {
    if (c != '\n') {
      line_.push_back(c);
      if (line_.size() > 1 << 20) line_.clear();  // A runaway line: drop it.
      continue;
    }
    if (!line_.empty() && line_.back() == '\r') line_.pop_back();
    if (line_.empty()) {
      if (has_data_) events.push_back(data_);
      data_.clear();
      has_data_ = false;
    } else if (line_.compare(0, 5, "data:") == 0) {
      size_t start = 5;
      if (line_.size() > 5 && line_[5] == ' ') start = 6;
      if (has_data_) data_.push_back('\n');
      data_.append(line_, start, std::string::npos);
      has_data_ = true;
    }  // Comments (":") and other fields are ignored.
    line_.clear();
  }
  return events;
}

void SseParser::Reset() {
  line_.clear();
  data_.clear();
  has_data_ = false;
}

std::string NmeaChecksum(const std::string& body) {
  unsigned char sum = 0;
  for (char c : body) sum ^= static_cast<unsigned char>(c);
  char text[4];
  std::snprintf(text, sizeof(text), "%02X", sum);
  return text;
}

std::string PrepareSentence(const std::string& input) {
  std::string s = input;
  // Tag blocks: one or more "\...\" groups before the sentence.
  while (!s.empty() && s[0] == '\\') {
    const size_t end = s.find('\\', 1);
    if (end == std::string::npos) return {};
    s.erase(0, end + 1);
  }
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  if (s.size() < 10 || (s[0] != '!' && s[0] != '$')) return {};
  const size_t star = s.rfind('*');
  if (star == std::string::npos || star + 3 != s.size()) return {};
  std::string body = s.substr(1, star - 1);
  std::string given = s.substr(star + 1);
  for (char& c : given) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (NmeaChecksum(body) != given) return {};
  if (body.size() > 5 && body.compare(2, 3, "VDO") == 0) body.replace(2, 3, "VDM");
  if (body.compare(2, 3, "VDM") != 0) return {};
  return std::string(1, s[0]) + body + "*" + NmeaChecksum(body);
}

std::vector<std::string> EncodeShipStatic(const JsonValue& message, int sequence) {
  const JsonValue* dimension = message.Get("Dimension");
  const JsonValue* eta = message.Get("Eta");
  const auto dim = [&](const char* key) { return dimension ? Number(dimension->Get(key)) : 0.0; };
  const auto when = [&](const char* key, double fallback) {
    return eta ? Number(eta->Get(key), fallback) : fallback;
  };
  const double mmsi = Number(message.Get("UserID"));
  if (mmsi <= 0) return {};
  BitWriter bits;
  bits.Put(5, 6);
  bits.PutInt(Number(message.Get("RepeatIndicator")), 2, 3);
  bits.PutInt(mmsi, 30, 999999999);
  bits.PutInt(Number(message.Get("AisVersion")), 2, 3);
  bits.PutInt(Number(message.Get("ImoNumber")), 30, 1073741823);
  bits.PutText(Text(message.Get("CallSign")), 7);
  bits.PutText(Text(message.Get("Name")), 20);
  bits.PutInt(Number(message.Get("Type")), 8, 255);
  bits.PutInt(dim("A"), 9, 511);
  bits.PutInt(dim("B"), 9, 511);
  bits.PutInt(dim("C"), 6, 63);
  bits.PutInt(dim("D"), 6, 63);
  bits.PutInt(Number(message.Get("FixType")), 4, 15);
  bits.PutInt(when("Month", 0), 4, 15);
  bits.PutInt(when("Day", 0), 5, 31);
  bits.PutInt(when("Hour", 24), 5, 31);
  bits.PutInt(when("Minute", 60), 6, 63);
  bits.PutInt(Number(message.Get("MaximumStaticDraught")) * 10.0, 8, 255);
  bits.PutText(Text(message.Get("Destination")), 20);
  const JsonValue* dte = message.Get("Dte");
  bits.Put(dte && dte->boolean ? 1 : 0, 1);
  bits.Put(0, 1);
  int fill = 0;
  const std::string payload = bits.Payload(&fill);  // 71 characters, 2 fill bits.
  const std::string seq = std::to_string(((sequence % 10) + 10) % 10);
  const std::string first = "AIVDM,2,1," + seq + ",A," + payload.substr(0, 60) + ",0";
  const std::string second =
      "AIVDM,2,2," + seq + ",A," + payload.substr(60) + "," + std::to_string(fill);
  return {"!" + first + "*" + NmeaChecksum(first), "!" + second + "*" + NmeaChecksum(second)};
}

bool ParseAisEvent(const std::string& json, AisEvent* event, int* sequence) {
  JsonValue root;
  if (!event || !ParseJson(json, &root) || !root.IsObject()) return false;
  *event = AisEvent();
  event->type = Text(root.Get("type"));
  if (event->type == "error") {
    event->error = Text(root.Get("message"));
    if (event->error.empty()) event->error = Text(root.Get("error"));
    return true;
  }
  if (event->type != "event") return true;
  event->attribution = Text(root.Get("attribution"));
  event->time = ParseUtcTime(Text(root.Get("time")));
  const JsonValue* message = root.Get("message");
  if (message && message->IsObject()) {
    event->message_id = static_cast<int>(Number(message->Get("MessageID")));
  }
  if (event->message_id == 0) event->message_id = MessageIdFromName(Text(root.Get("msg_type")));
  event->position = IsPositionMessage(event->message_id);
  const JsonValue* nmea = root.Get("nmea");
  if (nmea && nmea->IsArray()) {
    for (const auto& item : nmea->items) {
      const std::string sentence = PrepareSentence(Text(&item));
      if (!sentence.empty()) event->sentences.push_back(sentence);
    }
  }
  if (event->sentences.empty() && event->message_id == 5 && message && message->IsObject()) {
    const int seq = sequence ? (*sequence)++ : 0;
    event->sentences = EncodeShipStatic(*message, seq);
  }
  return true;
}

bool AisBox(double south, double west, double north, double east, double max_area,
            std::array<double, 4>* box) {
  if (!std::isfinite(south) || !std::isfinite(west) || !std::isfinite(north) ||
      !std::isfinite(east) || !box) {
    return false;
  }
  double lat0 = std::max(-90.0, std::min(south, north));
  double lat1 = std::min(90.0, std::max(south, north));
  double lon0 = std::max(-180.0, west);
  double lon1 = std::min(180.0, east);
  if (lon1 <= lon0 || lat1 <= lat0) return false;
  const double area = (lat1 - lat0) * (lon1 - lon0);
  if (area > max_area) {
    const double f = std::sqrt(max_area / area);
    const double c_lat = (lat0 + lat1) / 2, c_lon = (lon0 + lon1) / 2;
    const double h_lat = (lat1 - lat0) * f / 2, h_lon = (lon1 - lon0) * f / 2;
    lat0 = c_lat - h_lat;
    lat1 = c_lat + h_lat;
    lon0 = c_lon - h_lon;
    lon1 = c_lon + h_lon;
  }
  const auto round = [](double v) { return std::round(v * 1e4) / 1e4; };
  *box = {round(lat0), round(lon0), round(lat1), round(lon1)};
  return true;
}

std::vector<std::string> AttributionParts(const std::string& text) {
  std::vector<std::string> parts;
  size_t start = 0;
  auto add = [&](size_t end) {
    std::string part = text.substr(start, end - start);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.erase(0, 1);
    while (!part.empty() && (std::isspace(static_cast<unsigned char>(part.back())) || part.back() == '.')) {
      part.pop_back();
    }
    if (!part.empty()) parts.push_back(part);
  };
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != '.' || !std::isspace(static_cast<unsigned char>(text[i + 1]))) continue;
    // A sentence ends after ")" or a word ("aisstream.io. AISHub"), not inside "(…)".
    const bool after_bracket = i > 0 && text[i - 1] == ')';
    const bool after_word = i > 1 && std::islower(static_cast<unsigned char>(text[i - 1])) &&
                            std::islower(static_cast<unsigned char>(text[i - 2]));
    if (!after_bracket && !after_word) continue;
    add(i);
    start = i + 1;
  }
  add(text.size());
  return parts;
}

double ParseUtcTime(const std::string& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  double s = 0;
  if (text.size() < 20 ||
      std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:", &y, &mo, &d, &h, &mi) != 5) {
    return 0.0;
  }
  // Seconds, with any fraction, read without the locale's decimal separator.
  const char* p = text.c_str() + 17;
  if (!std::isdigit(static_cast<unsigned char>(p[0])) || !std::isdigit(static_cast<unsigned char>(p[1]))) {
    return 0.0;
  }
  s = (p[0] - '0') * 10 + (p[1] - '0');
  p += 2;
  if (*p == '.') {
    double scale = 0.1;
    for (++p; std::isdigit(static_cast<unsigned char>(*p)); ++p, scale /= 10) s += (*p - '0') * scale;
  }
  if (*p != 'Z' && *p != 'z') return 0.0;  // Open Waters sends UTC.
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0.0;
  const long long days = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return static_cast<double>(days) * 86400.0 + h * 3600.0 + mi * 60.0 + s;
}

}  // namespace online_charts
