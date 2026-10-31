#include "online_charts_render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>

#include "ocpn_plugin.h"
#include "pidc.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif !defined(__APPLE__) && !defined(__ANDROID__)
#include <GL/glx.h>
#endif

#ifdef _WIN32
#define ONLINE_CHARTS_GLU_CALLBACK __stdcall
#else
#define ONLINE_CHARTS_GLU_CALLBACK
#endif

namespace online_charts {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMercatorLatitude = 85.05112878;
constexpr double kEarthCircumference = 40075016.68557849;

double TileLatitude(double y, int z) {
  const double n = static_cast<double>(1 << z);
  return std::atan(std::sinh(kPi * (1.0 - 2.0 * y / n))) * 180.0 / kPi;
}

double TileLongitude(double x, int z) {
  const double n = static_cast<double>(1 << z);
  return x / n * 360.0 - 180.0;
}

int NextPow2(int value) {
  int result = 1;
  while (result < value) result <<= 1;
  return result;
}

/* ---------- GLU tessellation ---------- */

struct TessState {
  std::vector<float>* out = nullptr;
  std::deque<std::array<double, 3>> combined;
  bool failed = false;
};

void ONLINE_CHARTS_GLU_CALLBACK TessBegin(GLenum, void*) {}
void ONLINE_CHARTS_GLU_CALLBACK TessEnd(void*) {}
// Registering an edge-flag callback makes GLU emit independent triangles only.
void ONLINE_CHARTS_GLU_CALLBACK TessEdgeFlag(GLboolean, void*) {}

void ONLINE_CHARTS_GLU_CALLBACK TessVertex(void* vertex, void* data) {
  const auto* v = static_cast<const double*>(vertex);
  auto* state = static_cast<TessState*>(data);
  state->out->push_back(static_cast<float>(v[0]));
  state->out->push_back(static_cast<float>(v[1]));
}

void ONLINE_CHARTS_GLU_CALLBACK TessCombine(GLdouble coords[3], void*[4], GLfloat[4],
                                            void** out, void* data) {
  auto* state = static_cast<TessState*>(data);
  state->combined.push_back({coords[0], coords[1], coords[2]});
  *out = state->combined.back().data();
}

void ONLINE_CHARTS_GLU_CALLBACK TessError(GLenum, void* data) {
  static_cast<TessState*>(data)->failed = true;
}

}  // namespace

/* ---------- Projection ---------- */

TileProjector::TileProjector(PlugIn_ViewPort* viewport, int z, int x, int y)
    : viewport_(viewport), z_(z), x_(x), y_(y) {
  fast_ = viewport && (viewport->m_projection_type == PI_PROJECTION_MERCATOR ||
                       viewport->m_projection_type == PI_PROJECTION_EQUIRECTANGULAR);
  if (!fast_) return;
  const double west = TileLongitude(x, z);
  const double east = TileLongitude(x + 1, z);
  for (int row = 0; row <= kRows; ++row) {
    const double latitude = TileLatitude(y + static_cast<double>(row) / kRows, z);
    wxPoint2DDouble left;
    wxPoint2DDouble right;
    GetDoubleCanvasPixLL(viewport, &left, latitude, west);
    GetDoubleCanvasPixLL(viewport, &right, latitude, east);
    if (!std::isfinite(left.m_x) || !std::isfinite(left.m_y) ||
        !std::isfinite(right.m_x) || !std::isfinite(right.m_y)) {
      fast_ = false;
      return;
    }
    origin_x_[row] = left.m_x;
    origin_y_[row] = left.m_y;
    step_x_[row] = right.m_x - left.m_x;
    step_y_[row] = right.m_y - left.m_y;
  }
}

void TileProjector::Project(double tx, double ty, float* sx, float* sy) const {
  if (fast_) {
    // Piecewise-linear along y between sampled rows, extrapolated for buffers.
    const double position = ty * kRows;
    const int row = std::max(0, std::min(kRows - 1, static_cast<int>(std::floor(position))));
    const double f = position - row;
    const double ox = origin_x_[row] + (origin_x_[row + 1] - origin_x_[row]) * f;
    const double oy = origin_y_[row] + (origin_y_[row + 1] - origin_y_[row]) * f;
    const double kx = step_x_[row] + (step_x_[row + 1] - step_x_[row]) * f;
    const double ky = step_y_[row] + (step_y_[row + 1] - step_y_[row]) * f;
    *sx = static_cast<float>(ox + kx * tx);
    *sy = static_cast<float>(oy + ky * tx);
    return;
  }
  wxPoint2DDouble point;
  GetDoubleCanvasPixLL(viewport_, &point, TileLatitude(y_ + ty, z_), TileLongitude(x_ + tx, z_));
  *sx = static_cast<float>(point.m_x);
  *sy = static_cast<float>(point.m_y);
}

double ViewportZoom(PlugIn_ViewPort* viewport, double pixel_scale) {
  if (!viewport || !viewport->bValid || viewport->pix_width <= 0 ||
      viewport->pix_height <= 0 || !std::isfinite(viewport->view_scale_ppm) ||
      viewport->view_scale_ppm <= 0.0 || pixel_scale <= 0.0) {
    return -1.0;
  }
  const double latitude =
      std::max(-kMercatorLatitude, std::min(kMercatorLatitude, viewport->clat));
  const double zoom = std::log2(kEarthCircumference * std::cos(latitude * kPi / 180.0) *
                                viewport->view_scale_ppm / (256.0 * pixel_scale));
  return std::isfinite(zoom) ? zoom : -1.0;
}

double GlPixelScale() {
  const double scale = OCPN_GetDisplayContentScaleFactor();
  return std::isfinite(scale) ? std::max(1.0, std::min(4.0, scale)) : 1.0;
}

#ifndef APIENTRY
#define APIENTRY
#endif

namespace {
using UseProgramFn = void(APIENTRY*)(GLuint);

UseProgramFn UseProgramFunction() {
#if defined(__APPLE__)
  return &glUseProgram;
#else
  static bool looked_up = false;
  static UseProgramFn function = nullptr;
  if (!looked_up) {
    looked_up = true;
#if defined(_WIN32)
    function = reinterpret_cast<UseProgramFn>(wglGetProcAddress("glUseProgram"));
#elif !defined(__ANDROID__)
    function = reinterpret_cast<UseProgramFn>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glUseProgram")));
#endif
  }
  return function;
#endif
}
}  // namespace

int SaveAndClearProgram() {
  UseProgramFn use = UseProgramFunction();
  if (!use) return 0;
  GLint previous = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
  if (previous > 0) use(0);
  return previous;
}

void RestoreProgram(int program) {
  UseProgramFn use = UseProgramFunction();
  if (use && program > 0) use(static_cast<GLuint>(program));
}

bool Tessellate(const std::vector<std::vector<MvtPoint>>& rings, std::vector<float>* triangles) {
  GLUtesselator* tess = gluNewTess();
  if (!tess) return false;
  TessState state;
  state.out = triangles;
  gluTessCallback(tess, GLU_TESS_BEGIN_DATA, (_GLUfuncptr)&TessBegin);
  gluTessCallback(tess, GLU_TESS_END_DATA, (_GLUfuncptr)&TessEnd);
  gluTessCallback(tess, GLU_TESS_EDGE_FLAG_DATA, (_GLUfuncptr)&TessEdgeFlag);
  gluTessCallback(tess, GLU_TESS_VERTEX_DATA, (_GLUfuncptr)&TessVertex);
  gluTessCallback(tess, GLU_TESS_COMBINE_DATA, (_GLUfuncptr)&TessCombine);
  gluTessCallback(tess, GLU_TESS_ERROR_DATA, (_GLUfuncptr)&TessError);
  gluTessProperty(tess, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
  gluTessNormal(tess, 0, 0, 1);

  std::deque<std::array<double, 3>> vertices;
  gluTessBeginPolygon(tess, &state);
  for (const auto& ring : rings) {
    if (ring.size() < 3) continue;
    gluTessBeginContour(tess);
    for (const auto& point : ring) {
      vertices.push_back({point.x, point.y, 0.0});
      gluTessVertex(tess, vertices.back().data(), vertices.back().data());
    }
    gluTessEndContour(tess);
  }
  gluTessEndPolygon(tess);
  gluDeleteTess(tess);
  if (state.failed) triangles->clear();
  return !state.failed;
}

/* ---------- Text atlas ---------- */

int GlTextAtlas::GlyphIndex(wxChar c) const {
  // Printable ASCII, then Latin-1 (accented letters for localised day names,
  // the degree sign, middle dot and copyright sign).
  if (c >= 32 && c <= 126) return c - 32;
  if (c >= 160 && c <= 255) return 95 + (c - 160);
  return '?' - 32;
}

bool GlTextAtlas::Ensure(const wxFont& source) {
  wxFont font(source);
  if (!font.IsOk()) return false;
  font.SetPixelSize(wxSize(0, kBasePixels));
  const wxString key = font.GetFaceName() + wxString::Format(
      "|%d|%d", static_cast<int>(font.GetWeight()), static_cast<int>(font.GetStyle()));
  if (fill_texture_ && key == font_key_) return true;
  Release();

  constexpr int kGlyphs = 95 + 96;
  constexpr int kColumns = 16;
  wxString chars;
  for (int c = 32; c <= 126; ++c) chars += static_cast<wxChar>(c);
  for (int c = 160; c <= 255; ++c) chars += static_cast<wxChar>(c);

  wxMemoryDC measure;
  wxBitmap probe(8, 8, 24);
  measure.SelectObject(probe);
  measure.SetFont(font);
  std::vector<wxCoord> advances(kGlyphs, 0);
  wxCoord max_width = 0;
  wxCoord line_height = 0;
  for (int i = 0; i < kGlyphs; ++i) {
    wxCoord w = 0;
    wxCoord h = 0;
    measure.GetTextExtent(wxString(chars[i]), &w, &h);
    advances[i] = w;
    max_width = std::max(max_width, w);
    line_height = std::max(line_height, h);
  }
  measure.SelectObject(wxNullBitmap);
  if (max_width <= 0 || line_height <= 0) return false;

  const int cell_w = max_width + 2 * kPadding;
  const int cell_h = line_height + 2 * kPadding;
  const int rows = (kGlyphs + kColumns - 1) / kColumns;
  const int width = NextPow2(cell_w * kColumns);
  const int height = NextPow2(cell_h * rows);

  wxBitmap bitmap(width, height, 24);
  wxMemoryDC dc(bitmap);
  dc.SetBackground(*wxBLACK_BRUSH);
  dc.Clear();
  dc.SetFont(font);
  dc.SetTextForeground(*wxWHITE);
  dc.SetBackgroundMode(wxTRANSPARENT);
  glyphs_.assign(kGlyphs, Glyph());
  for (int i = 0; i < kGlyphs; ++i) {
    const int col = i % kColumns;
    const int row = i / kColumns;
    dc.DrawText(wxString(chars[i]), col * cell_w + kPadding, row * cell_h + kPadding);
    Glyph& glyph = glyphs_[i];
    glyph.u0 = static_cast<float>(col * cell_w) / width;
    glyph.v0 = static_cast<float>(row * cell_h) / height;
    glyph.u1 = static_cast<float>((col + 1) * cell_w) / width;
    glyph.v1 = static_cast<float>((row + 1) * cell_h) / height;
    glyph.advance = static_cast<float>(advances[i]);
  }
  dc.SelectObject(wxNullBitmap);

  const wxImage image = bitmap.ConvertToImage();
  if (!image.IsOk()) return false;
  const unsigned char* rgb = image.GetData();
  const int pixels = width * height;
  std::vector<unsigned char> alpha(pixels);
  for (int i = 0; i < pixels; ++i) alpha[i] = rgb[i * 3];

  // Halo: the glyph shape dilated by a disc, then softened slightly.
  constexpr int kHalo = 4;
  std::vector<unsigned char> halo(pixels, 0);
  std::vector<std::pair<int, int>> disc;
  for (int dy = -kHalo; dy <= kHalo; ++dy) {
    for (int dx = -kHalo; dx <= kHalo; ++dx) {
      if (dx * dx + dy * dy <= kHalo * kHalo) disc.emplace_back(dx, dy);
    }
  }
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      unsigned char best = 0;
      for (const auto& offset : disc) {
        const int sx = x + offset.first;
        const int sy = y + offset.second;
        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
        best = std::max(best, alpha[sy * width + sx]);
        if (best == 255) break;
      }
      halo[y * width + x] = best;
    }
  }

  auto upload = [&](const std::vector<unsigned char>& a) {
    std::vector<unsigned char> rgba(static_cast<size_t>(pixels) * 4);
    for (int i = 0; i < pixels; ++i) {
      rgba[i * 4 + 0] = 255;
      rgba[i * 4 + 1] = 255;
      rgba[i * 4 + 2] = 255;
      rgba[i * 4 + 3] = a[i];
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
  };
  fill_texture_ = upload(alpha);
  halo_texture_ = upload(halo);
  cell_width_ = static_cast<float>(cell_w);
  cell_height_ = static_cast<float>(cell_h);
  line_height_ = static_cast<float>(line_height);
  font_key_ = key;
  return fill_texture_ != 0;
}

void GlTextAtlas::Release() {
  if (fill_texture_) glDeleteTextures(1, &fill_texture_);
  if (halo_texture_) glDeleteTextures(1, &halo_texture_);
  fill_texture_ = 0;
  halo_texture_ = 0;
  glyphs_.clear();
  font_key_.clear();
}

void GlTextAtlas::Measure(const wxString& text, float height, float* width,
                          float* line_height) const {
  const float scale = line_height_ > 0 ? height / line_height_ : 0.0f;
  float total = 0;
  for (const wxChar c : text) {
    if (!glyphs_.empty()) total += glyphs_[GlyphIndex(c)].advance;
  }
  if (width) *width = total * scale;
  if (line_height) *line_height = line_height_ * scale;
}

void GlTextAtlas::DrawPass(const wxString& text, float x, float y, float scale,
                           unsigned texture) const {
  glBindTexture(GL_TEXTURE_2D, texture);
  glBegin(GL_QUADS);
  float pen = x;
  const float pad = kPadding * scale;
  const float w = cell_width_ * scale;
  const float h = cell_height_ * scale;
  for (const wxChar c : text) {
    const Glyph& g = glyphs_[GlyphIndex(c)];
    if (c != ' ') {
      const float x0 = pen - pad;
      const float y0 = y - pad;
      glTexCoord2f(g.u0, g.v0);
      glVertex2f(x0, y0);
      glTexCoord2f(g.u1, g.v0);
      glVertex2f(x0 + w, y0);
      glTexCoord2f(g.u1, g.v1);
      glVertex2f(x0 + w, y0 + h);
      glTexCoord2f(g.u0, g.v1);
      glVertex2f(x0, y0 + h);
    }
    pen += g.advance * scale;
  }
  glEnd();
}

void GlTextAtlas::Draw(const wxString& text, float x, float y, float height,
                       const wxColour& colour, const wxColour& halo) const {
  if (!fill_texture_ || glyphs_.empty() || line_height_ <= 0 || text.empty()) return;
  const float scale = height / line_height_;
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  if (halo.Alpha() > 0) {
    glColor4ub(halo.Red(), halo.Green(), halo.Blue(), halo.Alpha());
    DrawPass(text, x, y, scale, halo_texture_);
  }
  glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
  DrawPass(text, x, y, scale, fill_texture_);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

}  // namespace online_charts
