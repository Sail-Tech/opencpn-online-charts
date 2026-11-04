// Standalone check of the depth colours and the WebP elevation decoding:
//   c++ -std=c++17 -Isrc -Ilibs/libwebp tests/depth_test.cpp src/online_charts_depth.cpp \
//     libs/libwebp/src/*/*.c -o depth_test && ./depth_test [tile.webp]
// With a tile from https://tiles.openwaters.io/seascape/{z}/{x}/{y}.webp, its
// elevation range and colours are printed.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>

#include "online_charts_depth.h"

using namespace online_charts;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

static bool Same(Rgba c, unsigned rgb, unsigned alpha = 255) {
  return c.r == ((rgb >> 16) & 0xff) && c.g == ((rgb >> 8) & 0xff) && c.b == (rgb & 0xff) && c.a == alpha;
}

int main(int argc, char** argv) {
  Check(TerrariumElevation(128, 0, 0) == 0.0, "terrarium sea level");
  Check(TerrariumElevation(127, 239, 0) == -17.0, "terrarium -17 m");

  // Land is clear in both schemes; drying areas green (Chart) or pink (Topo).
  Check(DepthRelief(2.0, kDepthChart).a == 0 && DepthRelief(150.0, kDepthTopo).a == 0, "land clear");
  Check(Same(DepthRelief(1.0, kDepthChart), 0x58af9c), "chart drying");
  Check(Same(DepthRelief(1.0, kDepthTopo), 0xf4b6c8), "topo drying");

  // Chart: the extension's depth-shading bands.
  Check(Same(DepthRelief(-1.0, kDepthChart), 0x1f86cb), "chart 0-2 m");
  Check(Same(DepthRelief(-3.0, kDepthChart), 0x5db5f0), "chart 2-5 m");
  Check(Same(DepthRelief(-7.0, kDepthChart), 0x7fc7f8), "chart 5-10 m");
  Check(Same(DepthRelief(-15.0, kDepthChart), 0xa5d9fb), "chart 10-20 m");
  Check(Same(DepthRelief(-30.0, kDepthChart), 0xc9e9fd), "chart 20-50 m");
  Check(Same(DepthRelief(-500.0, kDepthChart), 0xe9f7ff), "chart deep");

  // Topo: the extension's stops, interpolated between them.
  Check(Same(DepthRelief(-5.0, kDepthTopo), 0xe8402f), "topo 5 m stop");
  Check(Same(DepthRelief(-35.0, kDepthTopo), 0xf2e33a), "topo 35 m stop");
  Check(Same(DepthRelief(-11000.0, kDepthTopo), 0x0d1340), "topo deepest");
  const Rgba mid = DepthRelief(-72.5, kDepthTopo);  // Halfway between 55 m and 90 m.
  Check(mid.r == 89 && mid.g == 205 && mid.b == 129, "topo interpolation");

  const unsigned char not_webp[] = {1, 2, 3, 4};
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  Check(!DecodeDepthTile(not_webp, sizeof(not_webp), kDepthTopo, &w, &h, &rgba), "rejects non-WebP data");

  if (argc > 1) {
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    Check(DecodeDepthTile(bytes.data(), bytes.size(), kDepthTopo, &w, &h, &rgba), "decodes the tile");
    std::map<unsigned, size_t> colours;
    size_t clear = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      if (rgba[i + 3] == 0) {
        ++clear;
      } else {
        ++colours[(rgba[i] << 16) | (rgba[i + 1] << 8) | rgba[i + 2]];
      }
    }
    std::printf("%dx%d: %zu land pixels, %zu Topo colours\n", w, h, clear, colours.size());
  }

  if (failures) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "depth_test: all checks passed\n";
  return 0;
}
