#ifndef ONLINE_CHARTS_MVT_H_
#define ONLINE_CHARTS_MVT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace online_charts {

struct MvtPoint {
  double x = 0.0;  // Tile coordinates, normalized to 0..1 (buffers may exceed).
  double y = 0.0;
};

struct MvtArea {
  std::vector<std::vector<MvtPoint>> rings;
  double drval1 = 0.0;
  double rank = 0.0;
  bool has_drval1 = false;
};

struct MvtContour {
  std::vector<std::vector<MvtPoint>> lines;
  double depth_m = 0.0;
  double depth_ft = 0.0;
  bool has_depth_m = false;
  bool has_depth_ft = false;
  // Open Waters publishes a metric and a feet contour set ("sys" = "ft"); the
  // feet lines also carry metre values, so each display unit uses its own set.
  bool feet_system = false;
};

struct MvtSounding {
  MvtPoint point;
  double depth_m = 0.0;
  double depth_ft = 0.0;
  bool has_depth_m = false;
  bool has_depth_ft = false;
};

struct MvtTile {
  std::vector<MvtArea> areas;
  std::vector<MvtContour> contours;
  std::vector<MvtSounding> soundings;
};

// Parses the public Open Waters Seascape layers: depare, contours and soundings.
// Unknown layers and properties are ignored. Returns false for malformed input.
bool ParseMvtTile(const uint8_t* data, size_t size, MvtTile* tile);

}  // namespace online_charts

#endif  // ONLINE_CHARTS_MVT_H_
