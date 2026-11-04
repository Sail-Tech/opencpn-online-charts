#ifndef ONLINE_CHARTS_DEPTH_H_
#define ONLINE_CHARTS_DEPTH_H_

// Depth colours from the Open Waters Seascape elevation raster (terrarium
// encoding, WebP tiles), as in the GPS Charts extension's "Chart" and "Topo"
// depth colours. No wxWidgets dependency (tests/depth_test.cpp).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace online_charts {

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 0;
};

enum DepthColours { kDepthChart = 0, kDepthTopo = 1 };

// Elevation in metres from a terrarium pixel: R * 256 + G + B / 256 - 32768.
inline double TerrariumElevation(uint8_t r, uint8_t g, uint8_t b) {
  return r * 256.0 + g + b / 256.0 - 32768.0;
}

// Colour of an elevation (negative below the sea). Land (2 m and above, where
// Open Waters clamps it) is transparent, so the OpenCPN chart shows there.
//  - Chart: the light blues of a paper chart, by depth band (50, 20, 10, 5, 2 m),
//    drying areas green.
//  - Topo: red in the shallows through orange, yellow and green to deep blue,
//    drying areas pink.
Rgba DepthRelief(double elevation, int scheme);

// Topo colour of a depth in metres (positive down).
Rgba TopoColourAtDepth(double depth);

// Decodes a WebP terrarium tile into RGBA depth colours. False when the data
// is not a valid WebP image.
bool DecodeDepthTile(const uint8_t* data, size_t size, int scheme, int* width, int* height,
                     std::vector<uint8_t>* rgba);

}  // namespace online_charts

#endif  // ONLINE_CHARTS_DEPTH_H_
