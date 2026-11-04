#ifndef ONLINE_CHARTS_AISDATA_H_
#define ONLINE_CHARTS_AISDATA_H_

// Open Waters AIS (https://openwaters.io/ais/) for the AIS demo: the
// server-sent events stream, and its events turned into NMEA 0183 sentences
// for OpenCPN's own AIS decoder. No wxWidgets dependency, so it can be tested
// on its own (tests/aisdata_test.cpp).

#include <array>
#include <string>
#include <vector>

namespace online_charts {

struct JsonValue;

// Server-sent events: collects the data lines of each event across chunks.
class SseParser {
 public:
  // Returns the data of every event completed by this chunk.
  std::vector<std::string> Feed(const std::string& chunk);
  void Reset();

 private:
  std::string line_;
  std::string data_;
  bool has_data_ = false;
};

struct AisEvent {
  std::string type;                    // "event", "welcome", "error", ...
  std::string error;                   // The message of an "error".
  std::vector<std::string> sentences;  // Ready for OpenCPN: !AIVDM with checksum.
  std::string attribution;
  double time = 0.0;                   // When it was received, Unix seconds; 0 if unknown.
  int message_id = 0;                  // AIS message type, 0 if unknown.
  bool position = false;               // A position report (ages with time).
};

// Parses one event's JSON. Static reports sent without NMEA (decoded only)
// are encoded as an AIS message 5 so OpenCPN learns the vessel's name.
bool ParseAisEvent(const std::string& json, AisEvent* event, int* sequence);

// Strips IEC 61162-450 tag blocks, turns own-ship VDO into VDM (these are
// other vessels for this boat) and checks the checksum. Empty when invalid.
std::string PrepareSentence(const std::string& sentence);

// "*hh" checksum of the characters between the leading ! or $ and the *.
std::string NmeaChecksum(const std::string& body);

// AIS message 5 (static and voyage data) as two !AIVDM sentences.
std::vector<std::string> EncodeShipStatic(const JsonValue& message, int sequence);

// The subscription box [minLat, minLon, maxLat, maxLon] for a view, shrunk
// about its centre to max_area square degrees (anonymous access allows 100).
// Returns false when the bounds are unusable.
bool AisBox(double south, double west, double north, double east, double max_area,
            std::array<double, 4>* box);

// "Open Waters AIS (…). AISHub (…)" -> {"Open Waters AIS (…)", "AISHub (…)"}.
std::vector<std::string> AttributionParts(const std::string& text);

// RFC 3339 UTC time ("2026-11-04T18:21:43.123Z") to Unix seconds; 0 if invalid.
double ParseUtcTime(const std::string& text);

}  // namespace online_charts

#endif  // ONLINE_CHARTS_AISDATA_H_
