#ifndef ONLINE_CHARTS_RENDER_H_
#define ONLINE_CHARTS_RENDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/font.h>
#include <wx/string.h>

#include "online_charts_mvt.h"

class PlugIn_ViewPort;

namespace online_charts {

// Converts normalised tile coordinates (0..1, buffers may exceed) to canvas
// pixels. For Mercator viewports the screen position of a tile varies
// linearly along x and smoothly along y, so a handful of host projections per
// tile replace one host call and two trigonometric functions per vertex.
// Other projections fall back to an exact host projection per point.
class TileProjector {
 public:
  TileProjector(PlugIn_ViewPort* viewport, int z, int x, int y);
  void Project(double tx, double ty, float* sx, float* sy) const;

 private:
  static constexpr int kRows = 8;
  PlugIn_ViewPort* viewport_;
  int z_;
  int x_;
  int y_;
  bool fast_ = false;
  double origin_x_[kRows + 1] = {};
  double origin_y_[kRows + 1] = {};
  double step_x_[kRows + 1] = {};
  double step_y_[kRows + 1] = {};
};

// Web Mercator zoom of the viewport for 256-pixel tiles, measured in logical
// (device-independent) pixels so Retina canvases do not request four times
// the tiles. Returns -1 for an unusable viewport.
double ViewportZoom(PlugIn_ViewPort* viewport, double pixel_scale);

// Ratio between canvas pixels and logical pixels for the OpenGL canvas
// (2 on Retina displays). The wxDC canvas is already in logical pixels.
double GlPixelScale();

// Saves the shader program the host left current and switches to the
// fixed-function pipeline; RestoreProgram puts it back. glUseProgram is
// looked up at run time on Windows and Linux, where the system OpenGL library
// does not export it directly.
int SaveAndClearProgram();
void RestoreProgram(int program);

// Triangulates polygon rings (odd-even fill) into a flat x,y triangle list in
// the rings' own coordinates. Returns false when tessellation fails.
bool Tessellate(const std::vector<std::vector<MvtPoint>>& rings, std::vector<float>* triangles);

// Renders text from a texture atlas built once per font, so any size can be
// drawn per frame without rebuilding font textures. Glyphs are scaled with
// mipmaps, and a pre-computed halo keeps them legible over the chart.
// Covers printable ASCII and Latin-1.
class GlTextAtlas {
 public:
  GlTextAtlas() = default;
  GlTextAtlas(const GlTextAtlas&) = delete;
  GlTextAtlas& operator=(const GlTextAtlas&) = delete;

  // Must be called with the OpenGL context current. Rebuilds only when the
  // face, weight or style changes.
  bool Ensure(const wxFont& font);
  bool Ready() const { return fill_texture_ != 0; }
  // Text size measured at the given glyph height, in canvas pixels.
  void Measure(const wxString& text, float height, float* width, float* line_height) const;
  // Draws text with its top-left corner at x,y.
  void Draw(const wxString& text, float x, float y, float height, const wxColour& colour,
            const wxColour& halo) const;
  // Releases the textures. Only call with the OpenGL context current.
  void Release();

 private:
  struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // texture coordinates of the cell
    float advance = 0;                     // in base pixels
  };
  int GlyphIndex(wxChar c) const;
  void DrawPass(const wxString& text, float x, float y, float scale, unsigned texture) const;

  static constexpr int kBasePixels = 48;  // glyph height the atlas is drawn at
  static constexpr int kPadding = 8;      // halo radius plus mipmap margin
  std::vector<Glyph> glyphs_;
  wxString font_key_;
  float cell_width_ = 0;
  float cell_height_ = 0;
  float line_height_ = 0;
  unsigned fill_texture_ = 0;
  unsigned halo_texture_ = 0;
};

}  // namespace online_charts

#endif  // ONLINE_CHARTS_RENDER_H_
