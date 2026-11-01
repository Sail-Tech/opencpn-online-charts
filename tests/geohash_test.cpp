// Standalone check of the Garmin geohash URL, with the reference values of the
// GPS Charts extension's tests/provider.test.js:
//   c++ -std=c++17 -Isrc tests/geohash_test.cpp -o geohash_test && ./geohash_test
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "online_charts_geohash.h"

using online_charts::GarminMarineUrl;
using online_charts::Geohash;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

int main() {
  Check(Geohash(50.7629, -1.298) == "gcp0f3ng66mj", "Cowes reference value");
  Check(Geohash(0, 0) == "s00000000000", "origin");
  Check(Geohash(-90, -180) == "000000000000", "south-west corner");
  Check(Geohash(90, 180) == "zzzzzzzzzzzz", "north-east corner");
  Check(Geohash(39.11206464283168, -74.70647351816297) == "dqfys00kkcrc", "second reference value");
  Check(Geohash(50.7629, -1.298, 5) == "gcp0f", "shorter precision");

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  Check(Geohash(nan, 0).empty() && Geohash(0, nan).empty(), "NaN rejected");
  Check(Geohash(inf, 0).empty() && Geohash(0, -inf).empty(), "infinity rejected");
  Check(Geohash(90.000001, 0).empty() && Geohash(-90.000001, 0).empty(), "latitude limits");
  Check(Geohash(0, 180.000001).empty() && Geohash(0, -180.000001).empty(), "longitude limits");

  Check(GarminMarineUrl(50.7629, -1.298) ==
            "https://maps.garmin.com/en-GB/marine/?maps=another-brand&overlay=false&key=gcp0f3ng66mj",
        "Garmin URL");
  Check(GarminMarineUrl(nan, 0).empty(), "no Garmin URL without a position");

  if (failures) return 1;
  std::cout << "Geohash and Garmin URL tests passed.\n";
  return 0;
}
