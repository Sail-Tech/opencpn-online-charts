#include "online_charts_land.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include <wx/bitmap.h>
#include <wx/log.h>
#include <wx/mstream.h>

#include "ocpn_plugin.h"
#include "online_charts_http.h"
#include "pidc.h"

namespace {

constexpr int kMinZoom = 4;
constexpr int kMaxZoom = 15;
constexpr size_t kMaxVisibleTiles = 48;
constexpr size_t kMaxMemoryTiles = 128;
constexpr size_t kMaxRequests = 4;
constexpr std::time_t kRetryDelay = 60;
constexpr double kMercatorLatitude = 85.05112878;
constexpr double kEarthCircumference = 40075016.68557849;
constexpr double kPi = 3.14159265358979323846;

std::string TileId(int z, int x, int y) {
  return std::to_string(z) + '/' + std::to_string(x) + '/' + std::to_string(y);
}

double MercatorY(double latitude, int zoom) {
  latitude = std::max(-kMercatorLatitude, std::min(kMercatorLatitude, latitude));
  const double radians = latitude * kPi / 180.0;
  const double n = static_cast<double>(1 << zoom);
  return (1.0 - std::asinh(std::tan(radians)) / kPi) * 0.5 * n;
}

double TileLatitude(int y, int zoom) {
  const double n = static_cast<double>(1 << zoom);
  const double mercator = kPi * (1.0 - 2.0 * y / n);
  return std::atan(std::sinh(mercator)) * 180.0 / kPi;
}

}  // namespace

OnlineChartsLandLayer::OnlineChartsLandLayer()
    : http_(std::make_unique<OnlineChartsHttp>(
          "OnlineCharts-OpenCPN/1.5.3 (+https://github.com/Sail-Tech/opencpn-online-charts)")) {}

OnlineChartsLandLayer::~OnlineChartsLandLayer() { Shutdown(); }

size_t OnlineChartsLandLayer::ActiveRequests() const { return pending_tiles_.size(); }

size_t OnlineChartsLandLayer::QueuedRequests() const { return download_queue_.size(); }

void OnlineChartsLandLayer::Configure(bool enabled, int provider,
                                      bool allow_network) {
  provider = std::max(0, std::min(2, provider));
  if (provider != provider_) {
    CancelRequests();
    download_queue_.clear();
    queued_tiles_.clear();
    ClearImages();
    failed_tiles_.clear();
    last_error_.clear();
  }
  provider_ = provider;
  enabled_ = enabled;
  allow_network_ = allow_network;
  if (!enabled_ || !allow_network_) {
    CancelRequests();
    download_queue_.clear();
    queued_tiles_.clear();
  }
}

void OnlineChartsLandLayer::CancelRequests() {
  if (http_) http_->CancelAll();
  pending_tiles_.clear();
}

void OnlineChartsLandLayer::Shutdown() {
  if (shutdown_) return;
  shutdown_ = true;
  CancelRequests();
  download_queue_.clear();
  queued_tiles_.clear();
  // Textures are not deleted here: the OpenGL context may not be current
  // during shutdown, and OpenCPN releases the context's textures with it.
  images_.clear();
}

int OnlineChartsLandLayer::ZoomForViewport(PlugIn_ViewPort* viewport) const {
  if (!viewport || !viewport->bValid || viewport->pix_width <= 0 ||
      viewport->pix_height <= 0 || !std::isfinite(viewport->view_scale_ppm) ||
      viewport->view_scale_ppm <= 0.0) {
    return -1;
  }
  const double centre_latitude =
      std::max(-kMercatorLatitude, std::min(kMercatorLatitude, viewport->clat));
  // Measured in logical pixels: a Retina canvas needs the same tiles as a
  // standard one, each drawn over twice as many canvas pixels.
  const double metres_per_pixel = pixel_scale_ / viewport->view_scale_ppm;
  const double precise_zoom =
      std::log2(kEarthCircumference * std::cos(centre_latitude * kPi / 180.0) /
                (256.0 * metres_per_pixel));
  if (!std::isfinite(precise_zoom)) return -1;
  return static_cast<int>(std::floor(precise_zoom + 0.15));
}

std::vector<OnlineChartsLandLayer::TileKey>
OnlineChartsLandLayer::VisibleTiles(PlugIn_ViewPort* viewport) const {
  std::vector<TileKey> keys;
  int zoom = ZoomForViewport(viewport);
  if (zoom < kMinZoom || !viewport) return keys;
  // World Ocean Base has global coverage through z10, but detailed tiles are
  // regional. Overzoom z10 instead of leaving oceans blank when z11+ returns 404.
  zoom = std::min(provider_ == 1 ? 10 : (provider_ == 2 ? 18 : kMaxZoom), zoom);

  double lat_min = viewport->lat_min;
  double lat_max = viewport->lat_max;
  if (!std::isfinite(lat_min) || !std::isfinite(lat_max)) return keys;
  if (lat_min > lat_max) std::swap(lat_min, lat_max);
  lat_min = std::max(-kMercatorLatitude, lat_min);
  lat_max = std::min(kMercatorLatitude, lat_max);
  if (lat_min > lat_max) return keys;

  const int n = 1 << zoom;
  const int y_min = std::max(0, static_cast<int>(std::floor(MercatorY(lat_max, zoom))));
  const int y_max = std::min(n - 1, static_cast<int>(std::floor(MercatorY(lat_min, zoom))));
  if (y_min > y_max) return keys;

  std::vector<std::pair<double, double>> ranges;
  const double lon_min = viewport->lon_min;
  const double lon_max = viewport->lon_max;
  if (!std::isfinite(lon_min) || !std::isfinite(lon_max)) return keys;
  if (lon_max - lon_min >= 360.0 || lon_min < -180.0 || lon_max > 180.0) {
    ranges.push_back(std::make_pair(-180.0, 180.0));
  } else if (lon_min <= lon_max) {
    ranges.push_back(std::make_pair(lon_min, lon_max));
  } else {
    ranges.push_back(std::make_pair(lon_min, 180.0));
    ranges.push_back(std::make_pair(-180.0, lon_max));
  }

  std::set<std::string> unique;
  for (const auto& range : ranges) {
    const double min = std::max(-180.0, range.first);
    const double max = std::min(180.0, range.second);
    const int x_min = static_cast<int>(std::floor((min + 180.0) / 360.0 * n));
    const int x_max = static_cast<int>(std::floor((max + 180.0) / 360.0 * n));
    for (int x = x_min; x <= x_max; ++x) {
      int wrapped = x % n;
      if (wrapped < 0) wrapped += n;
      for (int y = y_min; y <= y_max; ++y) {
        TileKey key{zoom, wrapped, y, TileId(zoom, wrapped, y)};
        if (unique.insert(key.id).second) keys.push_back(key);
      }
    }
  }

  if (keys.size() > kMaxVisibleTiles) {
    const double centre_x = (viewport->clon + 180.0) / 360.0 * n;
    const double centre_y = MercatorY(viewport->clat, zoom);
    std::sort(keys.begin(), keys.end(), [&](const TileKey& a, const TileKey& b) {
      double ax = a.x + 0.5 - centre_x;
      double bx = b.x + 0.5 - centre_x;
      if (ax > n / 2) ax -= n;
      if (ax < -n / 2) ax += n;
      if (bx > n / 2) bx -= n;
      if (bx < -n / 2) bx += n;
      const double ad = ax * ax + std::pow(a.y + 0.5 - centre_y, 2);
      const double bd = bx * bx + std::pow(b.y + 0.5 - centre_y, 2);
      return ad < bd;
    });
    keys.resize(kMaxVisibleTiles);
  }
  return keys;
}

void OnlineChartsLandLayer::TrimMemoryCache() {
  while (images_.size() > kMaxMemoryTiles) {
    auto oldest = images_.begin();
    for (auto it = images_.begin(); it != images_.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) oldest = it;
    }
    if (oldest->second.texture) textures_to_delete_.push_back(oldest->second.texture);
    images_.erase(oldest);
  }
}

void OnlineChartsLandLayer::ClearImages() {
  for (const auto& entry : images_) {
    if (entry.second.texture) textures_to_delete_.push_back(entry.second.texture);
  }
  images_.clear();
}

size_t OnlineChartsLandLayer::Render(piDC* dc, PlugIn_ViewPort* viewport,
                                     double pixel_scale) {
  if (shutdown_) return 0;
  pixel_scale_ = std::max(1.0, pixel_scale);
  if (!dc && !textures_to_delete_.empty()) {
    glDeleteTextures(static_cast<GLsizei>(textures_to_delete_.size()), textures_to_delete_.data());
    textures_to_delete_.clear();
  }
  if (!enabled_) return 0;
  const auto keys = VisibleTiles(viewport);
  const size_t drawn = dc ? RenderDc(*dc, viewport, keys) : RenderGl(viewport, keys);
  QueueDownloads(keys);
  return drawn;
}

size_t OnlineChartsLandLayer::RenderGl(PlugIn_ViewPort* viewport,
                                       const std::vector<TileKey>& keys) {
  size_t drawn = 0;
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glColor4ub(255, 255, 255, 255);
  for (const auto& key : keys) {
    // Use the tile itself, or a loaded parent tile magnified while it downloads.
    CachedImage* cached = nullptr;
    int level = 0;
    for (; level <= 4 && key.z - level >= 0; ++level) {
      auto found = images_.find(TileId(key.z - level, key.x >> level, key.y >> level));
      if (found != images_.end() && found->second.image.IsOk()) {
        cached = &found->second;
        break;
      }
    }
    if (!cached) continue;
    cached->last_used = ++use_counter_;
    if (!cached->texture) {
      const wxImage& image = cached->image;
      GLuint texture = 0;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (image.HasAlpha()) {
        // Transparent tiles (OpenSeaMap seamarks) keep their alpha channel.
        const int pixels = image.GetWidth() * image.GetHeight();
        std::vector<unsigned char> rgba(static_cast<size_t>(pixels) * 4);
        const unsigned char* rgb = image.GetData();
        const unsigned char* alpha = image.GetAlpha();
        for (int i = 0; i < pixels; ++i) {
          rgba[i * 4 + 0] = rgb[i * 3 + 0];
          rgba[i * 4 + 1] = rgb[i * 3 + 1];
          rgba[i * 4 + 2] = rgb[i * 3 + 2];
          rgba[i * 4 + 3] = alpha[i];
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.GetWidth(), image.GetHeight(), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
      } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, image.GetWidth(), image.GetHeight(), 0, GL_RGB,
                     GL_UNSIGNED_BYTE, image.GetData());
      }
      cached->texture = texture;
    }

    const double n = static_cast<double>(1 << key.z);
    const double lon_west = (key.x / n) * 360.0 - 180.0;
    const double lon_east = ((key.x + 1) / n) * 360.0 - 180.0;
    const double lat_north = TileLatitude(key.y, key.z);
    const double lat_south = TileLatitude(key.y + 1, key.z);
    wxPoint2DDouble corners[4];
    GetDoubleCanvasPixLL(viewport, &corners[0], lat_north, lon_west);
    GetDoubleCanvasPixLL(viewport, &corners[1], lat_north, lon_east);
    GetDoubleCanvasPixLL(viewport, &corners[2], lat_south, lon_east);
    GetDoubleCanvasPixLL(viewport, &corners[3], lat_south, lon_west);
    double min_x = corners[0].m_x, max_x = min_x, min_y = corners[0].m_y, max_y = min_y;
    for (const auto& c : corners) {
      min_x = std::min(min_x, c.m_x);
      max_x = std::max(max_x, c.m_x);
      min_y = std::min(min_y, c.m_y);
      max_y = std::max(max_y, c.m_y);
    }
    if (!std::isfinite(min_x) || !std::isfinite(min_y) || max_x < 0 || max_y < 0 ||
        min_x > viewport->pix_width || min_y > viewport->pix_height) {
      continue;
    }
    // Texture coordinates of this tile inside the (possibly parent) image.
    const double span = 1.0 / (1 << level);
    const double u0 = (key.x - ((key.x >> level) << level)) * span;
    const double v0 = (key.y - ((key.y >> level) << level)) * span;
    glBindTexture(GL_TEXTURE_2D, cached->texture);
    glBegin(GL_QUADS);
    glTexCoord2d(u0, v0);
    glVertex2d(corners[0].m_x, corners[0].m_y);
    glTexCoord2d(u0 + span, v0);
    glVertex2d(corners[1].m_x, corners[1].m_y);
    glTexCoord2d(u0 + span, v0 + span);
    glVertex2d(corners[2].m_x, corners[2].m_y);
    glTexCoord2d(u0, v0 + span);
    glVertex2d(corners[3].m_x, corners[3].m_y);
    glEnd();
    if (level == 0) ++drawn;
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  return drawn;
}

size_t OnlineChartsLandLayer::RenderDc(piDC& dc, PlugIn_ViewPort* viewport,
                                       const std::vector<TileKey>& keys) {
  size_t drawn = 0;
  for (const auto& key : keys) {
    auto found = images_.find(key.id);
    if (found == images_.end() || !found->second.image.IsOk()) continue;
    found->second.last_used = ++use_counter_;

    const double n = static_cast<double>(1 << key.z);
    const double lon_west = (key.x / n) * 360.0 - 180.0;
    const double lon_east = ((key.x + 1) / n) * 360.0 - 180.0;
    const double lat_north = TileLatitude(key.y, key.z);
    const double lat_south = TileLatitude(key.y + 1, key.z);
    wxPoint north_west;
    wxPoint south_east;
    GetCanvasPixLL(viewport, &north_west, lat_north, lon_west);
    GetCanvasPixLL(viewport, &south_east, lat_south, lon_east);
    const int width = south_east.x - north_west.x;
    const int height = south_east.y - north_west.y;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) continue;
    if (north_west.x + width < 0 || north_west.y + height < 0 ||
        north_west.x > viewport->pix_width || north_west.y > viewport->pix_height) {
      continue;
    }
    // Rescale only when the drawn size changes, not on every frame.
    wxBitmap& bitmap = found->second.scaled;
    if (!bitmap.IsOk() || bitmap.GetWidth() != width || bitmap.GetHeight() != height) {
      bitmap = wxBitmap(found->second.image.Scale(width, height, wxIMAGE_QUALITY_BILINEAR));
    }
    if (bitmap.IsOk()) {
      dc.DrawBitmap(bitmap, north_west.x, north_west.y, found->second.image.HasAlpha());
      ++drawn;
    }
  }
  return drawn;
}

void OnlineChartsLandLayer::QueueDownloads(const std::vector<TileKey>& keys) {
  if (shutdown_ || !enabled_ || !allow_network_) return;
  const std::time_t now = std::time(nullptr);
  for (const auto& key : keys) {
    if (images_.count(key.id) || pending_tiles_.count(key.id) || queued_tiles_.count(key.id)) {
      continue;
    }
    auto failed = failed_tiles_.find(key.id);
    if (failed != failed_tiles_.end() && failed->second + kRetryDelay > now) continue;
    if (download_queue_.size() >= kMaxVisibleTiles) break;
    download_queue_.push_back(key);
    queued_tiles_.insert(key.id);
  }
  StartQueuedDownloads();
}

void OnlineChartsLandLayer::StartQueuedDownloads() {
  if (shutdown_ || !enabled_ || !allow_network_) return;
  while (pending_tiles_.size() < kMaxRequests && !download_queue_.empty()) {
    const TileKey key = download_queue_.front();
    download_queue_.pop_front();
    queued_tiles_.erase(key.id);
    if (images_.count(key.id) || pending_tiles_.count(key.id)) continue;
    StartDownload(key);
  }
}

void OnlineChartsLandLayer::StartDownload(const TileKey& key) {
  wxString url;
  if (provider_ == 0) {
    url = wxString::Format("https://tile.openstreetmap.org/%d/%d/%d.png", key.z, key.x, key.y);
  } else if (provider_ == 1) {
    url = wxString::Format(
        "https://server.arcgisonline.com/ArcGIS/rest/services/Ocean/"
        "World_Ocean_Base/MapServer/tile/%d/%d/%d?blankTile=false",
        key.z, key.y, key.x);
  } else {
    url = wxString::Format("https://tiles.openseamap.org/seamark/%d/%d/%d.png", key.z, key.x, key.y);
  }
  pending_tiles_.insert(key.id);
  const bool started = http_->Get(
      url,
      [this, key](bool ok, long status, std::string body) {
        CompleteDownload(key, ok || status > 0, status, body);
      },
      "image/png, image/jpeg, image/*;q=0.8");
  if (!started) {
    pending_tiles_.erase(key.id);
    failed_tiles_[key.id] = std::time(nullptr);
    last_error_ = "Could not create the base map HTTPS request";
  }
}

void OnlineChartsLandLayer::CompleteDownload(const TileKey& key, bool transport_ok,
                                             long http_status, const std::string& bytes) {
  pending_tiles_.erase(key.id);
  if (shutdown_) return;
  bool success = false;
  if (provider_ == 2 && transport_ok && (http_status == 404 || http_status == 204)) {
    // No seamarks in this tile: remember it as empty instead of retrying.
    images_[key.id] = CachedImage{wxImage(), ++use_counter_};
    success = true;
  } else if (transport_ok && http_status == 200 && !bytes.empty()) {
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage image;
    if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.IsOk()) {
      if (image.HasMask() && !image.HasAlpha()) image.InitAlpha();
      images_[key.id] = CachedImage{image, ++use_counter_};
      TrimMemoryCache();
      failed_tiles_.erase(key.id);
      last_error_.clear();
      success = true;
    }
  }
  if (!success) {
    failed_tiles_[key.id] = std::time(nullptr);
    if (http_status > 0) {
      last_error_ = wxString::Format("%s server returned HTTP %ld",
                                     provider_ == 2 ? "Seamark" : "Base map", http_status);
    } else if (!transport_ok) {
      last_error_ = "Base map network request failed";
    } else {
      last_error_ = "The base map response was not a valid PNG or JPEG image";
    }
    wxLogWarning("Online Charts: %s tile %s failed; HTTP=%ld transport=%d",
                 provider_ == 2 ? "seamark" : "base map",
                 wxString::FromUTF8(key.id.c_str()), http_status, transport_ok ? 1 : 0);
  }
  StartQueuedDownloads();
  if (success || !download_queue_.empty()) RequestRefresh(GetOCPNCanvasWindow());
}
