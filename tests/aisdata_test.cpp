// Standalone check of the Open Waters AIS helpers:
//   c++ -std=c++17 -Isrc tests/aisdata_test.cpp src/online_charts_aisdata.cpp src/online_charts_json.cpp -o aisdata_test
//   ./aisdata_test [recorded-sse-file]
// With a recording of https://ais.openwaters.io/v1/stream (curl -N), every static report
// that carries both its NMEA and its decoded message is re-encoded and compared.
#include <clocale>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "online_charts_aisdata.h"
#include "online_charts_json.h"

using namespace online_charts;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

// The payload's bits, up to the given count.
static std::string Bits(const std::string& payload, size_t count) {
  std::string bits;
  for (char c : payload) {
    int v = c - 48;
    if (v > 40) v -= 8;
    for (int b = 5; b >= 0; --b) bits.push_back(((v >> b) & 1) ? '1' : '0');
  }
  return bits.substr(0, count);
}

static std::string Payload(const std::string& sentence) {
  size_t comma = 0;
  for (int i = 0; i < 5; ++i) comma = sentence.find(',', comma) + 1;
  return sentence.substr(comma, sentence.find(',', comma) - comma);
}

int main(int argc, char** argv) {
  std::setlocale(LC_NUMERIC, "pt_PT.UTF-8");

  // Checksums and sentence preparation.
  Check(NmeaChecksum("AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0") == "43", "checksum");
  Check(PrepareSentence("!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*43") ==
            "!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*43",
        "valid sentence kept");
  Check(PrepareSentence("\\s:2573345,c:1700000000*7B\\!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*43\r\n") ==
            "!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*43",
        "tag block removed");
  Check(PrepareSentence("!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*44").empty(), "bad checksum refused");
  const std::string vdo = "AIVDO,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0";
  Check(PrepareSentence("!" + vdo + "*" + NmeaChecksum(vdo)) ==
            "!AIVDM,1,1,,A,13P=obm000OqWKFM7I2sQ8cF0000,0*43",
        "VDO becomes VDM");
  Check(PrepareSentence("$GPGGA,1*00").empty(), "not AIS");

  // Server-sent events across chunk boundaries.
  SseParser sse;
  auto first = sse.Feed("data: {\"a\":1}\r\n\r\ndata: {\"b\"");
  auto second = sse.Feed(":2}\n\n: keep-alive\n\n");
  Check(first.size() == 1 && first[0] == "{\"a\":1}", "first event");
  Check(second.size() == 1 && second[0] == "{\"b\":2}", "event split across chunks; comment ignored");

  // Times and boxes.
  Check(std::fabs(ParseUtcTime("2026-11-04T18:21:43Z") - 1793816503.0) < 1e-6, "UTC time");
  Check(std::fabs(ParseUtcTime("2026-11-04T17:45:27.888706292Z") - 1793814327.888706) < 1e-3, "fractional time");
  Check(ParseUtcTime("garbage") == 0.0, "invalid time");
  std::array<double, 4> box{};
  Check(AisBox(50.6, -1.6, 50.9, -1.0, 99, &box) && box[0] == 50.6 && box[3] == -1.0, "small box kept");
  Check(AisBox(30, -30, 60, 10, 99, &box) &&
            std::fabs((box[2] - box[0]) * (box[3] - box[1]) - 99) < 0.1 && std::fabs((box[0] + box[2]) / 2 - 45) < 1e-6,
        "large box shrunk about its centre");
  Check(!AisBox(50, 10, 50, 11, 99, &box), "empty box refused");

  auto parts = AttributionParts("Open Waters AIS (https://openwaters.io/ais/). aisstream.io");
  Check(parts.size() == 2 && parts[1] == "aisstream.io", "attribution after a bracket");
  parts = AttributionParts("Open Waters AIS (https://openwaters.io/ais/). AISHub (https://www.aishub.net)");
  Check(parts.size() == 2 && parts[1] == "AISHub (https://www.aishub.net)", "attribution with a link");

  // A decoded-only static report becomes a two-part message 5.
  int sequence = 3;
  AisEvent event;
  const std::string json =
      R"JSON({"type":"event","time":"2026-11-04T18:20:39Z","source":"aishub","attribution":"Open Waters AIS (https://openwaters.io/ais/). AISHub (https://www.aishub.net)","mmsi":235114752,"msg_type":"ShipStaticData","message":{"MessageID":5,"RepeatIndicator":0,"UserID":235114752,"AisVersion":0,"ImoNumber":0,"CallSign":"","Name":"PICA PICA","Type":37,"Dimension":{"A":0,"B":0,"C":0,"D":0},"FixType":0,"Eta":{"Month":0,"Day":0,"Hour":0,"Minute":0},"MaximumStaticDraught":0,"Destination":"","Dte":false}})JSON";
  Check(ParseAisEvent(json, &event, &sequence), "event parses");
  Check(event.message_id == 5 && !event.position, "static report");
  Check(event.sentences.size() == 2 && event.sentences[0].rfind("!AIVDM,2,1,3,A,", 0) == 0 &&
            event.sentences[1].rfind("!AIVDM,2,2,3,A,", 0) == 0,
        "two sentences with a sequence id");
  for (const auto& s : event.sentences) Check(PrepareSentence(s) == s, "encoded sentence is valid: " + s);
  Check(event.sentences.size() == 2 && Payload(event.sentences[0]).size() == 60 &&
            Payload(event.sentences[1]).size() == 11 && event.sentences[1].find(",2*") != std::string::npos,
        "424 bits in 71 characters with 2 fill bits");

  // Round trip against recorded traffic.
  if (argc > 1) {
    std::ifstream in(argv[1]);
    std::stringstream all;
    all << in.rdbuf();
    SseParser parser;
    int compared = 0, matched = 0, events = 0;
    for (const auto& data : parser.Feed(all.str())) {
      JsonValue root;
      if (!ParseJson(data, &root)) continue;
      AisEvent e;
      int seq = 0;
      if (!ParseAisEvent(data, &e, &seq) || e.type != "event") continue;
      ++events;
      const JsonValue* nmea = root.Get("nmea");
      const JsonValue* message = root.Get("message");
      if (e.message_id != 5 || !nmea || nmea->items.size() != 2 || !message) continue;
      const auto encoded = EncodeShipStatic(*message, 0);
      const std::string original = Payload(PrepareSentence(nmea->items[0].text)) +
                                   Payload(PrepareSentence(nmea->items[1].text));
      const std::string mine = Payload(encoded[0]) + Payload(encoded[1]);
      // Compare every field; text fields may be padded with spaces or '@'.
      const std::string a = Bits(original, 424), b = Bits(mine, 424);
      ++compared;
      auto text = [](const std::string& bits, size_t at, size_t chars) {
        std::string t;
        for (size_t i = 0; i < chars; ++i) {
          int v = std::stoi(bits.substr(at + i * 6, 6), nullptr, 2);
          t.push_back(static_cast<char>(v < 32 ? v + 64 : v));
        }
        while (!t.empty() && (t.back() == '@' || t.back() == ' ')) t.pop_back();
        return t;
      };
      // Bits 0-69: type to IMO; 70: call sign; 112: name; 232-301: type to draught; 302: destination.
      const bool same = a.substr(0, 70) == b.substr(0, 70) && text(a, 70, 7) == text(b, 70, 7) &&
                        text(a, 112, 20) == text(b, 112, 20) &&
                        a.substr(232, 70) == b.substr(232, 70) && text(a, 302, 20) == text(b, 302, 20);
      if (same) {
        ++matched;
      } else if (compared - matched <= 3) {
        std::cerr << "differs: " << data.substr(0, 200) << "\n  orig " << a << "\n  mine " << b << '\n';
      }
    }
    std::cout << events << " events; message 5 re-encoded: " << matched << " of " << compared << " match\n";
    Check(compared == 0 || matched * 10 >= compared * 9, "re-encoded static reports match the originals");
  }

  if (failures) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "aisdata_test: all checks passed\n";
  return 0;
}
