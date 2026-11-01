#ifndef ONLINE_CHARTS_GEOHASH_H_
#define ONLINE_CHARTS_GEOHASH_H_

#include <cmath>
#include <string>

namespace online_charts {

// Standard base-32 geohash, as used by Garmin's Marine page in its `key`
// parameter. Returns an empty string for a position outside the globe.
inline std::string Geohash(double latitude, double longitude, int precision = 12) {
  static const char kAlphabet[] = "0123456789bcdefghjkmnpqrstuvwxyz";
  if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 ||
      latitude > 90 || longitude < -180 || longitude > 180) {
    return std::string();
  }
  double lat_min = -90, lat_max = 90, lon_min = -180, lon_max = 180;
  bool even = true;
  int bit = 0, value = 0;
  std::string result;
  while (static_cast<int>(result.size()) < precision) {
    double& low = even ? lon_min : lat_min;
    double& high = even ? lon_max : lat_max;
    const double coordinate = even ? longitude : latitude;
    const double middle = (low + high) / 2;
    if (coordinate >= middle) {
      value = value * 2 + 1;
      low = middle;
    } else {
      value *= 2;
      high = middle;
    }
    even = !even;
    if (++bit == 5) {
      result += kAlphabet[value];
      bit = 0;
      value = 0;
    }
  }
  return result;
}

// Garmin Marine's English page centred on a position (the same URL as the
// GPS Charts extension). Empty when the position is not valid.
inline std::string GarminMarineUrl(double latitude, double longitude) {
  const std::string key = Geohash(latitude, longitude);
  if (key.empty()) return key;
  return "https://maps.garmin.com/en-GB/marine/?maps=another-brand&overlay=false&key=" + key;
}

}  // namespace online_charts

#endif  // ONLINE_CHARTS_GEOHASH_H_
