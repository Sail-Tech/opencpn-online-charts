#include "online_charts_depth.h"

#include <cmath>

#include "src/webp/decode.h"

namespace online_charts {

namespace {

Rgba Hex(uint32_t rgb, uint8_t alpha = 255) {
  Rgba c;
  c.r = static_cast<uint8_t>((rgb >> 16) & 0xff);
  c.g = static_cast<uint8_t>((rgb >> 8) & 0xff);
  c.b = static_cast<uint8_t>(rgb & 0xff);
  c.a = alpha;
  return c;
}

}  // namespace

Rgba TopoColourAtDepth(double depth) {
  // Open Waters' Topo view (the extension's depth-topo color-relief stops).
  struct Stop {
    double depth;
    uint32_t rgb;
  };
  static const Stop kStops[] = {
      {0, 0xd7263d},    {5, 0xe8402f},    {18, 0xf59331},   {35, 0xf2e33a},
      {55, 0x7fd04a},   {90, 0x33c9b8},   {200, 0x1aa0d8},  {1000, 0x1f6fd6},
      {3000, 0x1f3fb0}, {6000, 0x18246e}, {11000, 0x0d1340}};
  const size_t count = sizeof(kStops) / sizeof(kStops[0]);
  if (!(depth > kStops[0].depth)) return Hex(kStops[0].rgb);
  for (size_t i = 1; i < count; ++i) {
    if (depth <= kStops[i].depth) {
      const double t = (depth - kStops[i - 1].depth) / (kStops[i].depth - kStops[i - 1].depth);
      const auto mix = [&](int shift) {
        const double a = (kStops[i - 1].rgb >> shift) & 0xff;
        const double b = (kStops[i].rgb >> shift) & 0xff;
        return static_cast<uint8_t>(std::lround(a + (b - a) * t));
      };
      Rgba c;
      c.r = mix(16);
      c.g = mix(8);
      c.b = mix(0);
      c.a = 255;
      return c;
    }
  }
  return Hex(kStops[count - 1].rgb);
}

Rgba DepthRelief(double elevation, int scheme) {
  if (!std::isfinite(elevation) || elevation >= 2.0) return Rgba();  // Land: clear.
  if (elevation > 0.0) return Hex(scheme == kDepthTopo ? 0xf4b6c8 : 0x58af9c);  // Drying.
  if (scheme == kDepthTopo) return TopoColourAtDepth(-elevation);
  // The extension's depth-shading steps.
  const double depth = -elevation;
  if (depth < 1.995) return Hex(0x1f86cb);
  if (depth < 4.95) return Hex(0x5db5f0);
  if (depth < 9.95) return Hex(0x7fc7f8);
  if (depth < 19.95) return Hex(0xa5d9fb);
  if (depth < 49.95) return Hex(0xc9e9fd);
  return Hex(0xe9f7ff);
}

bool DecodeDepthTile(const uint8_t* data, size_t size, int scheme, int* width, int* height,
                     std::vector<uint8_t>* rgba) {
  int w = 0, h = 0;
  uint8_t* pixels = WebPDecodeRGB(data, size, &w, &h);
  if (!pixels || w <= 0 || h <= 0) {
    if (pixels) WebPFree(pixels);
    return false;
  }
  const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
  rgba->resize(count * 4);
  // Neighbouring pixels usually share an elevation: reuse the last colour.
  uint32_t last_key = 0xffffffff;
  Rgba last;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = pixels + i * 3;
    const uint32_t key = (static_cast<uint32_t>(p[0]) << 16) | (p[1] << 8) | p[2];
    if (key != last_key) {
      last = DepthRelief(TerrariumElevation(p[0], p[1], p[2]), scheme);
      last_key = key;
    }
    uint8_t* out = rgba->data() + i * 4;
    out[0] = last.r;
    out[1] = last.g;
    out[2] = last.b;
    out[3] = last.a;
  }
  WebPFree(pixels);
  *width = w;
  *height = h;
  return true;
}

}  // namespace online_charts
