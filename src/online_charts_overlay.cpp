#include "online_charts_overlay.h"

#include "online_charts_land.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/stdpaths.h>
#include <wx/zstream.h>

#include "ocpn_plugin.h"
#include "pidc.h"

#if !wxUSE_WEBREQUEST
#include <curl/curl.h>
#endif

namespace {

constexpr int kMinZoom = 4;
constexpr int kMaxZoom = 15;
constexpr int kMaxRequests = 8;
constexpr int kMaxPrefetchTiles = 5000;
// The disk cache holds several saved areas at the limit, plus browsing tiles.
constexpr size_t kMaxDiskTiles = 4 * kMaxPrefetchTiles;
constexpr unsigned kMaintainEveryWrites = 100;
// Size assumed per tile when the cache is too small to measure an average.
constexpr uint64_t kAssumedTileBytes = 32 * 1024;
constexpr size_t kMaxMemoryTiles = 128;
constexpr size_t kMaxTileBytes = 16 * 1024 * 1024;
constexpr std::time_t kRetryDelay = 60;
constexpr unsigned kMaxNetworkLogs = 128;
constexpr unsigned kMaxRenderLogs = 128;
const char kHttpsSessionError[] = "macOS HTTPS session is unavailable (URLSession)";
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

wxColour AreaColour(const online_charts::MvtArea& area) {
  if (!area.has_drval1) return wxColour(31, 134, 203, 115);
  const unsigned char alpha = 178;
  if (area.drval1 < 0.0) return wxColour(88, 175, 156, alpha);
  if (area.drval1 < 1.99) return wxColour(31, 134, 203, alpha);
  if (area.drval1 < 4.99) return wxColour(93, 181, 240, alpha);
  if (area.drval1 < 9.99) return wxColour(127, 199, 248, alpha);
  if (area.drval1 < 19.99) return wxColour(165, 217, 251, alpha);
  if (area.drval1 < 49.99) return wxColour(201, 233, 253, alpha);
  return wxColour(233, 247, 255, alpha);
}

bool ReadStream(wxInputStream* input, std::vector<uint8_t>* bytes) {
  if (!input) return false;
  char buffer[16384];
  while (input->IsOk()) {
    input->Read(buffer, sizeof(buffer));
    const size_t count = input->LastRead();
    if (count == 0) break;
    if (bytes->size() + count > kMaxTileBytes) return false;
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(buffer);
    bytes->insert(bytes->end(), begin, begin + count);
  }
  return input->Eof() || input->IsOk();
}

bool DecodeTile(std::vector<uint8_t>* bytes) {
  if (bytes->size() < 2 || (*bytes)[0] != 0x1f || (*bytes)[1] != 0x8b) return true;
  wxMemoryInputStream compressed(bytes->data(), bytes->size());
  wxZlibInputStream gzip(compressed, wxZLIB_GZIP);
  std::vector<uint8_t> decoded;
  if (!ReadStream(&gzip, &decoded)) return false;
  bytes->swap(decoded);
  return true;
}

#if !wxUSE_WEBREQUEST
size_t CurlWrite(char* data, size_t size, size_t count, void* context) {
  const size_t length = size * count;
  auto* bytes = static_cast<std::vector<uint8_t>*>(context);
  if (bytes->size() + length > kMaxTileBytes) return 0;
  const uint8_t* begin = reinterpret_cast<const uint8_t*>(data);
  bytes->insert(bytes->end(), begin, begin + length);
  return length;
}
#endif

wxString DepthText(double depth) {
  depth = std::fabs(depth);
  char text[64];
  if (std::fabs(depth - std::round(depth)) < 0.05) {
    std::snprintf(text, sizeof(text), "%lld",
                  static_cast<long long>(std::llround(depth)));
  } else {
    std::snprintf(text, sizeof(text), "%.1f", depth);
  }
  std::string result(text);
  for (char& c : result) {
    if (c == ',') c = '.';
  }
  return wxString::FromUTF8(result.c_str());
}

// Soundings follow the chart convention used by the browser dashboard: whole
// metres with the decimetres as a smaller subscript digit, or whole feet.
void SoundingText(double depth, bool feet, wxString* whole, wxString* decimal) {
  depth = std::fabs(depth);
  if (feet) {
    *whole = wxString::Format("%lld", static_cast<long long>(std::llround(depth)));
    decimal->clear();
    return;
  }
  long long metres = static_cast<long long>(std::floor(depth));
  int tenths = static_cast<int>(std::lround((depth - metres) * 10.0));
  if (tenths == 10) {
    ++metres;
    tenths = 0;
  }
  *whole = wxString::Format("%lld", metres);
  *decimal = tenths ? wxString::Format("%d", tenths) : wxString();
}

// Label height in logical pixels for a MapLibre-style map zoom (the dashboard's
// 512-pixel zoom): it grows steadily as you zoom in, like the browser chart.
double LabelPixels(double map_zoom) {
  static const double kStops[][2] = {{6, 10}, {9, 12}, {12, 15}, {15, 18}};
  if (map_zoom <= kStops[0][0]) return kStops[0][1];
  for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
    if (map_zoom <= kStops[i][0]) {
      const double f = (map_zoom - kStops[i - 1][0]) / (kStops[i][0] - kStops[i - 1][0]);
      return kStops[i - 1][1] + f * (kStops[i][1] - kStops[i - 1][1]);
    }
  }
  return kStops[3][1];
}

wxFont OpenCPNFont(const wxString& element, int default_size,
                   wxFontStyle style = wxFONTSTYLE_NORMAL, int pixel_height = 0) {
  // Keep OpenCPN's configured family. The host font is copied, never modified.
  wxFont* configured = GetOCPNScaledFont_PlugIn(element, default_size);
  wxFont font = configured && configured->IsOk()
                    ? wxFont(*configured)
                    : wxFont(default_size, wxFONTFAMILY_SWISS, style, wxFONTWEIGHT_NORMAL);
  font.SetStyle(style);
  if (pixel_height > 0) font.SetPixelSize(wxSize(0, pixel_height));
  return font;
}

void DrawLabel(piDC& dc, const wxString& text, int x, int y,
               const wxColour& colour = wxColour(79, 104, 116)) {
  dc.SetTextForeground(wxColour(255, 255, 255, 235));
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx || dy) dc.DrawText(text, x + dx, y + dy);
    }
  }
  dc.SetTextForeground(colour);
  dc.DrawText(text, x, y);
}

// Rectangles of the labels already drawn, bucketed in a coarse grid so each
// new label is only tested against its neighbours.
class LabelGrid {
 public:
  bool Free(const wxRect& box) const {
    for (int gx = Cell(box.GetLeft()); gx <= Cell(box.GetRight()); ++gx) {
      for (int gy = Cell(box.GetTop()); gy <= Cell(box.GetBottom()); ++gy) {
        auto found = cells_.find(Key(gx, gy));
        if (found == cells_.end()) continue;
        for (size_t index : found->second) {
          if (boxes_[index].Intersects(box)) return false;
        }
      }
    }
    return true;
  }
  void Add(const wxRect& box) {
    boxes_.push_back(box);
    for (int gx = Cell(box.GetLeft()); gx <= Cell(box.GetRight()); ++gx) {
      for (int gy = Cell(box.GetTop()); gy <= Cell(box.GetBottom()); ++gy) {
        cells_[Key(gx, gy)].push_back(boxes_.size() - 1);
      }
    }
  }

 private:
  static int Cell(int v) { return static_cast<int>(std::floor(v / 96.0)); }
  static long long Key(int x, int y) {
    return (static_cast<long long>(x) << 32) ^ static_cast<unsigned>(y);
  }
  std::vector<wxRect> boxes_;
  std::unordered_map<long long, std::vector<size_t>> cells_;
};

// Functions rather than global wxColour objects: a global with a destructor
// registers an exit handler that can run after OpenCPN unloads the plugin.
wxColour LabelColour() { return wxColour(79, 104, 116); }
wxColour HaloColour() { return wxColour(255, 255, 255, 200); }

}  // namespace


OnlineChartsOverlay::OnlineChartsOverlay()
#if wxUSE_WEBREQUEST
    : session_(wxWebSession::New())
#endif
{
  base_map_ = std::make_unique<OnlineChartsLandLayer>();
  seamarks_ = std::make_unique<OnlineChartsLandLayer>();
  wxFileName dir(wxStandardPaths::Get().GetUserDataDir(), "");
  dir.AppendDir("online_charts_pi");
  dir.AppendDir("vector_cache");
  cache_dir_ = dir.GetFullPath();
  const bool cache_ready = wxFileName::Mkdir(cache_dir_, 0770, wxPATH_MKDIR_FULL) ||
                           wxDirExists(cache_dir_);
  if (!cache_ready) last_error_ = "Cache directory is not writable";
#if wxUSE_WEBREQUEST
  const bool url_session_available =
      wxWebSession::IsBackendAvailable(wxWebSessionBackendURLSession);
  const bool session_ready = session_.IsOpened();
  if (!session_ready) {
    last_error_ = kHttpsSessionError;
    wxLogWarning("Online Charts: %s", kHttpsSessionError);
  }
  wxLogMessage(
      "Online Charts: overlay initialised; HTTPS=wxWebRequest; "
      "URLSession_available=%d; session_ready=%d; cache=%s; cache_ready=%d",
      url_session_available ? 1 : 0, session_ready ? 1 : 0, cache_dir_,
      cache_ready ? 1 : 0);
#else
  wxLogMessage(
      "Online Charts: overlay initialised; HTTPS=libcurl; cache=%s; cache_ready=%d",
      cache_dir_, cache_ready ? 1 : 0);
#endif
  MaintainDiskCache();
#if wxUSE_WEBREQUEST
  Bind(wxEVT_WEBREQUEST_STATE, &OnlineChartsOverlay::OnWebRequestState, this);
#else
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl_multi_ = curl_multi_init();
  curl_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, &OnlineChartsOverlay::OnCurlTimer, this);
#endif
}

OnlineChartsOverlay::~OnlineChartsOverlay() {
  Shutdown();
#if wxUSE_WEBREQUEST
  Unbind(wxEVT_WEBREQUEST_STATE, &OnlineChartsOverlay::OnWebRequestState, this);
#else
  Unbind(wxEVT_TIMER, &OnlineChartsOverlay::OnCurlTimer, this);
#endif
}

void OnlineChartsOverlay::Configure(const OnlineChartsSettings& input) {
  settings_ = input;
  settings_.label_scale = std::max(50, std::min(300, settings_.label_scale));
  settings_.base_map_provider = std::max(0, std::min(1, settings_.base_map_provider));
  settings_.cache_max_mb = std::max(10, std::min(20000, settings_.cache_max_mb));
  settings_.cache_max_age_days = std::max(1, std::min(365, settings_.cache_max_age_days));
  settings_.prefetch_levels = std::max(0, std::min(4, settings_.prefetch_levels));
  if (base_map_) {
    base_map_->Configure(
        settings_.overlay_enabled && settings_.show_base_map,
        settings_.base_map_provider,
        settings_.overlay_enabled &&
            settings_.cache_mode != OnlineChartsCacheMode::kOfflineOnly);
  }
  if (seamarks_) {
    seamarks_->Configure(settings_.overlay_enabled && settings_.show_seamarks, 2,
                         settings_.overlay_enabled &&
                             settings_.cache_mode != OnlineChartsCacheMode::kOfflineOnly);
  }
  if (!settings_.overlay_enabled ||
      settings_.cache_mode == OnlineChartsCacheMode::kOfflineOnly) {
    CancelRequests();
    download_queue_.clear();
    queued_tiles_.clear();
  }
  MaintainDiskCache();
  render_log_count_ = 0;
  last_render_state_.clear();
  RequestRefresh(GetOCPNCanvasWindow());
}

std::time_t OnlineChartsOverlay::TileLifetime() const {
  return static_cast<std::time_t>(settings_.cache_max_age_days) * 24 * 60 * 60;
}

void OnlineChartsOverlay::CancelRequests() {
  auto pending = std::move(requests_);
  requests_.clear();
  pending_tiles_.clear();
#if wxUSE_WEBREQUEST
  // Cancel() may synchronously emit an event. The active map is already empty,
  // so such an event cannot erase an element while this loop is iterating.
  for (auto& entry : pending) entry.second.request.Cancel();
#else
  curl_timer_.Stop();
  for (auto& entry : pending) {
    CURL* easy = static_cast<CURL*>(entry.second.easy);
    if (curl_multi_ && easy) {
      curl_multi_remove_handle(static_cast<CURLM*>(curl_multi_), easy);
    }
    if (easy) curl_easy_cleanup(easy);
  }
#endif
}

void OnlineChartsOverlay::Shutdown() {
  if (shutdown_) return;
  shutdown_ = true;
  if (base_map_) base_map_->Shutdown();
  if (seamarks_) seamarks_->Shutdown();
  CancelRequests();
  download_queue_.clear();
  queued_tiles_.clear();
  cache_.clear();
  last_viewport_.reset();
#if !wxUSE_WEBREQUEST
  if (curl_multi_) {
    curl_multi_cleanup(static_cast<CURLM*>(curl_multi_));
    curl_multi_ = nullptr;
  }
#endif
}

wxString OnlineChartsOverlay::CacheFile(const TileKey& key) const {
  wxString filename = wxString::FromUTF8(key.id.c_str());
  filename.Replace("/", "_");
  return cache_dir_ + wxFileName::GetPathSeparator() + filename + ".pbf";
}

int OnlineChartsOverlay::ZoomForViewport(PlugIn_ViewPort* viewport) const {
  // Open Waters vector tiles are styled for 512-pixel tiles, like the browser
  // dashboard: one tile level below the 256-pixel zoom of the logical canvas.
  const double zoom = online_charts::ViewportZoom(viewport, pixel_scale_);
  return zoom < 0.0 ? -1 : static_cast<int>(std::floor(zoom - 1.0 + 0.15));
}


std::vector<OnlineChartsOverlay::TileKey> OnlineChartsOverlay::VisibleTiles(
    PlugIn_ViewPort* viewport, int forced_zoom, size_t maximum) const {
  std::vector<TileKey> keys;
  int zoom = forced_zoom >= 0 ? forced_zoom : ZoomForViewport(viewport);
  if (zoom < kMinZoom || !viewport) return keys;
  zoom = std::min(kMaxZoom, zoom);

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
  double lon_min = viewport->lon_min;
  double lon_max = viewport->lon_max;
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
        TileKey key;
        key.z = zoom;
        key.x = wrapped;
        key.y = y;
        key.id = TileId(zoom, wrapped, y);
        if (unique.insert(key.id).second) keys.push_back(key);
      }
    }
  }

  if (keys.size() > maximum) {
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
    keys.resize(maximum);
  }
  return keys;
}

bool OnlineChartsOverlay::HasFreshDiskTile(const TileKey& key,
                                           std::time_t now) const {
  if (!settings_.cache_enabled) return false;
  auto missing = disk_missing_.find(key.id);
  if (missing != disk_missing_.end() && missing->second + kRetryDelay > now) return false;
  const wxString path = CacheFile(key);
  if (!wxFileName::FileExists(path)) return false;
  const wxDateTime modified = wxFileName(path).GetModificationTime();
  return modified.IsValid() && modified.GetTicks() + TileLifetime() > now;
}

OnlineChartsOverlay::CacheEntry* OnlineChartsOverlay::GetTile(const TileKey& key) {
  auto found = cache_.find(key.id);
  if (found != cache_.end()) {
    found->second.last_used = ++use_counter_;
    return &found->second;
  }
  if (!settings_.cache_enabled) return nullptr;

  // Remember tiles that are not on disk, so a missing tile costs one file
  // lookup per minute instead of one per frame while it downloads.
  const std::time_t now = std::time(nullptr);
  auto missing = disk_missing_.find(key.id);
  if (missing != disk_missing_.end() && missing->second + kRetryDelay > now) return nullptr;
  const wxString path = CacheFile(key);
  if (!wxFileName::FileExists(path)) {
    disk_missing_[key.id] = now;
    return nullptr;
  }
  wxFile file(path, wxFile::read);
  if (!file.IsOpened() || file.Length() < 0 ||
      static_cast<uint64_t>(file.Length()) > kMaxTileBytes) {
    return nullptr;
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(file.Length()));
  if (!bytes.empty() && file.Read(bytes.data(), bytes.size()) !=
                            static_cast<ssize_t>(bytes.size())) {
    return nullptr;
  }
  auto tile = std::make_shared<online_charts::MvtTile>();
  if (!online_charts::ParseMvtTile(bytes.data(), bytes.size(), tile.get())) {
    wxRemoveFile(path);
    last_error_ = "A damaged cache tile was removed";
    return nullptr;
  }
  const wxDateTime modified = wxFileName(path).GetModificationTime();
  const std::time_t expires =
      modified.IsValid() ? modified.GetTicks() + TileLifetime() : 0;
  CacheEntry entry;
  entry.tile = tile;
  entry.expires = expires;
  entry.last_used = ++use_counter_;
  cache_[key.id] = entry;
  TrimMemoryCache();
  found = cache_.find(key.id);
  return found == cache_.end() ? nullptr : &found->second;
}


void OnlineChartsOverlay::TrimMemoryCache() {
  while (cache_.size() > kMaxMemoryTiles) {
    auto oldest = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->second.last_used < oldest->second.last_used) oldest = it;
    }
    cache_.erase(oldest);
  }
}

void OnlineChartsOverlay::QueueDownloads(const std::vector<TileKey>& keys) {
  if (shutdown_ || !settings_.overlay_enabled ||
      settings_.cache_mode == OnlineChartsCacheMode::kOfflineOnly) {
    return;
  }
#if wxUSE_WEBREQUEST
  if (!session_.IsOpened()) {
    last_error_ = kHttpsSessionError;
    return;
  }
#endif
  const std::time_t now = std::time(nullptr);
  for (const auto& key : keys) {
    if (download_queue_.size() >= kMaxPrefetchTiles) break;
    if (pending_tiles_.count(key.id) || queued_tiles_.count(key.id)) continue;
    auto cached = cache_.find(key.id);
    if (cached != cache_.end() && cached->second.expires > now) continue;
    if (cached == cache_.end() && HasFreshDiskTile(key, now)) continue;
    auto failed = failed_tiles_.find(key.id);
    if (failed != failed_tiles_.end() && failed->second + kRetryDelay > now) continue;
    download_queue_.push_back(key);
    queued_tiles_.insert(key.id);
  }
  StartQueuedDownloads();
}

void OnlineChartsOverlay::StartQueuedDownloads() {
  if (shutdown_ || settings_.cache_mode == OnlineChartsCacheMode::kOfflineOnly) return;
#if wxUSE_WEBREQUEST
  if (!session_.IsOpened()) {
    last_error_ = kHttpsSessionError;
    return;
  }
#endif
  const std::time_t now = std::time(nullptr);
  while (requests_.size() < kMaxRequests && !download_queue_.empty()) {
    const TileKey key = download_queue_.front();
    download_queue_.pop_front();
    queued_tiles_.erase(key.id);
    if (pending_tiles_.count(key.id)) continue;
    auto cached = cache_.find(key.id);
    if (cached != cache_.end() && cached->second.expires > now) continue;
    StartDownload(key);
  }
}

void OnlineChartsOverlay::StartDownload(const TileKey& key) {
#if wxUSE_WEBREQUEST
  if (!session_.IsOpened()) {
    last_error_ = kHttpsSessionError;
    return;
  }
#endif
  const int id = next_request_id_++;
  if (next_request_id_ < 20000 || next_request_id_ > 1000000) next_request_id_ = 20000;
  const wxString url = wxString::Format(
      "https://tiles.openwaters.io/seascape/%d/%d/%d.pbf", key.z, key.x, key.y);
  PendingRequest pending;
  pending.key = key;
  if (network_log_count_ < kMaxNetworkLogs) {
    ++network_log_count_;
    wxLogMessage("Online Charts: tile request %d started for %s", id,
                 wxString::FromUTF8(key.id.c_str()));
  }
#if wxUSE_WEBREQUEST
  wxWebRequest request = session_.CreateRequest(this, url, id);
  if (!request.IsOk()) {
    wxLogWarning("Online Charts: cannot create HTTPS request for tile %s",
                 wxString::FromUTF8(key.id.c_str()));
    failed_tiles_[key.id] = std::time(nullptr);
    last_error_ = "Could not create the HTTPS tile request";
    ++status_.downloads_failed;
    return;
  }
  request.SetHeader("Accept", "application/x-protobuf, application/octet-stream");
  request.SetHeader("Accept-Encoding", "identity");
  request.SetHeader("User-Agent", "OnlineCharts-OpenCPN/1.5.3");
  pending.request = request;
#else
  if (!curl_multi_) {
    failed_tiles_[key.id] = std::time(nullptr);
    return;
  }
  CURL* easy = curl_easy_init();
  if (!easy) {
    failed_tiles_[key.id] = std::time(nullptr);
    return;
  }
  const std::string url_utf8 = url.ToUTF8().data();
  curl_easy_setopt(easy, CURLOPT_URL, url_utf8.c_str());
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "OnlineCharts-OpenCPN/1.5.3");
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlWrite);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &pending.bytes);
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
  pending.easy = easy;
#endif
  auto inserted = requests_.emplace(id, std::move(pending));
  PendingRequest& stored = inserted.first->second;
#if !wxUSE_WEBREQUEST
  CURL* stored_easy = static_cast<CURL*>(stored.easy);
  curl_easy_setopt(stored_easy, CURLOPT_WRITEDATA, &stored.bytes);
  if (curl_multi_add_handle(static_cast<CURLM*>(curl_multi_), stored_easy) != CURLM_OK) {
    curl_easy_cleanup(stored_easy);
    requests_.erase(inserted.first);
    failed_tiles_[key.id] = std::time(nullptr);
    return;
  }
  if (!curl_timer_.IsRunning()) curl_timer_.Start(40);
#endif
  pending_tiles_[key.id] = id;
#if wxUSE_WEBREQUEST
  stored.request.Start();
#endif
}

void OnlineChartsOverlay::CompleteDownload(int id, bool transport_ok, long http_status,
                                           std::vector<uint8_t> bytes) {
  auto found = requests_.find(id);
  if (found == requests_.end()) return;
  const TileKey key = found->second.key;
#if !wxUSE_WEBREQUEST
  CURL* easy = static_cast<CURL*>(found->second.easy);
  if (curl_multi_ && easy) curl_multi_remove_handle(static_cast<CURLM*>(curl_multi_), easy);
  if (easy) curl_easy_cleanup(easy);
#endif
  bool success = false;
  bool cache_written = false;
  size_t area_count = 0;
  size_t contour_count = 0;
  size_t sounding_count = 0;
  const bool valid_http = http_status == 200 || http_status == 204;
  if (transport_ok && valid_http && !shutdown_ && DecodeTile(&bytes)) {
    auto tile = std::make_shared<online_charts::MvtTile>();
    if (online_charts::ParseMvtTile(bytes.data(), bytes.size(), tile.get())) {
      area_count = tile->areas.size();
      contour_count = tile->contours.size();
      sounding_count = tile->soundings.size();
      if (settings_.cache_enabled) {
        const wxString path = CacheFile(key);
        const wxString temporary = path + wxString::Format(".tmp-%d", id);
        wxFile file(temporary, wxFile::write);
        if (file.IsOpened() &&
            (bytes.empty() || file.Write(bytes.data(), bytes.size()) == bytes.size())) {
          file.Close();
          cache_written = wxRenameFile(temporary, path, true);
          if (cache_written && ++disk_writes_ % kMaintainEveryWrites == 0) MaintainDiskCache();
        }
        if (!cache_written) wxRemoveFile(temporary);
      }
      CacheEntry entry;
      entry.tile = tile;
      entry.expires = std::time(nullptr) + TileLifetime();
      entry.last_used = ++use_counter_;
      cache_[key.id] = entry;
      disk_missing_.erase(key.id);
      TrimMemoryCache();
      failed_tiles_.erase(key.id);
      success = true;
      ++status_.downloads_succeeded;
      if (http_status == 204 || bytes.empty()) {
        last_error_.clear();
      } else {
        last_error_.clear();
      }
    }
  }
  if (!success && !shutdown_) {
    ++status_.downloads_failed;
    if (http_status > 0) {
      last_error_ = wxString::Format("Tile server returned HTTP %ld", http_status);
    } else if (!transport_ok) {
      last_error_ = "Network request failed; cached tiles will be used";
    } else {
      last_error_ = "The tile response was not valid Open Waters data";
    }
  }
  if (network_log_count_ < kMaxNetworkLogs) {
    ++network_log_count_;
    wxLogMessage(
        "Online Charts: tile %s result=%s transport=%d HTTP=%ld bytes=%llu "
        "areas=%u contours=%u soundings=%u cache_written=%d",
        wxString::FromUTF8(key.id.c_str()), success ? "ok" : "failed",
        transport_ok ? 1 : 0, http_status,
        static_cast<unsigned long long>(bytes.size()),
        static_cast<unsigned>(area_count), static_cast<unsigned>(contour_count),
        static_cast<unsigned>(sounding_count), cache_written ? 1 : 0);
  }
  if (!success && !shutdown_) failed_tiles_[key.id] = std::time(nullptr);
  pending_tiles_.erase(key.id);
  requests_.erase(found);
  StartQueuedDownloads();
  if (!shutdown_ && (success || !download_queue_.empty())) {
    RequestRefresh(GetOCPNCanvasWindow());
  }
}

#if wxUSE_WEBREQUEST
void OnlineChartsOverlay::OnWebRequestState(wxWebRequestEvent& event) {
  auto found = requests_.find(event.GetId());
  if (found == requests_.end()) return;
  const wxWebRequest::State state = event.GetState();
  if (state != wxWebRequest::State_Completed && state != wxWebRequest::State_Failed &&
      state != wxWebRequest::State_Cancelled) {
    return;
  }
  std::vector<uint8_t> bytes;
  long status = 0;
  bool success = false;
  wxWebResponse response = event.GetResponse();
  if (response.IsOk()) status = response.GetStatus();
  if (state == wxWebRequest::State_Completed && response.IsOk()) {
    // Open Waters uses 204 for an empty ocean/land tile. NSURLSession may not
    // expose a response stream for that bodyless response; it is still a valid,
    // cacheable empty MVT tile and must not be retried as a network failure.
    success = status == 204 || ReadStream(response.GetStream(), &bytes);
  }
  CompleteDownload(event.GetId(), success, status, std::move(bytes));
}
#else
void OnlineChartsOverlay::OnCurlTimer(wxTimerEvent&) {
  if (shutdown_ || !curl_multi_) return;
  int running = 0;
  curl_multi_perform(static_cast<CURLM*>(curl_multi_), &running);
  CURLMsg* message = nullptr;
  int remaining = 0;
  while ((message = curl_multi_info_read(static_cast<CURLM*>(curl_multi_), &remaining))) {
    if (message->msg != CURLMSG_DONE) continue;
    int id = 0;
    for (const auto& entry : requests_) {
      if (entry.second.easy == message->easy_handle) {
        id = entry.first;
        break;
      }
    }
    auto found = requests_.find(id);
    if (found == requests_.end()) continue;
    long status = 0;
    curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status);
    CompleteDownload(id, message->data.result == CURLE_OK, status,
                     std::move(found->second.bytes));
  }
  if (requests_.empty()) curl_timer_.Stop();
}
#endif

void OnlineChartsOverlay::LogRenderState(
    const char* backend, PlugIn_ViewPort* viewport, size_t visible_tiles,
    size_t ready_tiles, size_t drawn_areas, size_t drawn_contours,
    size_t drawn_soundings) {
  const bool valid = viewport && viewport->bValid;
  status_.viewport_valid = valid;
  status_.visible_tiles = visible_tiles;
  status_.ready_tiles = ready_tiles;
  status_.drawn_areas = drawn_areas;
  status_.drawn_contours = drawn_contours;
  status_.drawn_soundings = drawn_soundings;
  status_.base_error = base_map_ ? base_map_->LastError() : wxString();
  status_.memory_tiles = cache_.size();
  status_.active_requests = requests_.size() + (base_map_ ? base_map_->ActiveRequests() : 0) +
      (seamarks_ ? seamarks_->ActiveRequests() : 0);
  status_.queued_requests = download_queue_.size() + (base_map_ ? base_map_->QueuedRequests() : 0) +
      (seamarks_ ? seamarks_->QueuedRequests() : 0);
  status_.zoom = ZoomForViewport(viewport);
  status_.last_error = last_error_;

  std::string state(backend);
  state += '|' + std::to_string(valid ? 1 : 0);
  state += '|' + std::to_string(viewport ? viewport->pix_width : 0);
  state += '|' + std::to_string(viewport ? viewport->pix_height : 0);
  state += '|' + std::to_string(visible_tiles);
  state += '|' + std::to_string(ready_tiles);
  state += '|' + std::to_string(drawn_areas);
  state += '|' + std::to_string(drawn_contours);
  state += '|' + std::to_string(drawn_soundings);
  state += '|' + std::to_string(status_.base_tiles);
  state += '|' + std::to_string(requests_.size());
  state += '|' + std::to_string(download_queue_.size());
  if (state == last_render_state_ || render_log_count_ >= kMaxRenderLogs) return;
  last_render_state_ = state;
  ++render_log_count_;

  double scale = 0.0;
  double zoom = 0.0;
  if (valid && std::isfinite(viewport->view_scale_ppm) &&
      viewport->view_scale_ppm > 0.0) {
    scale = viewport->view_scale_ppm;
    const double latitude = std::max(
        -kMercatorLatitude, std::min(kMercatorLatitude, viewport->clat));
    zoom = std::log2(kEarthCircumference *
                     std::cos(latitude * kPi / 180.0) * scale / 256.0);
  }
  wxLogMessage(
      "Online Charts: render backend=%s valid=%d size=%dx%d scale=%.6g "
      "zoom=%.2f label_px=%d visible_tiles=%u ready_tiles=%u base_tiles=%u areas=%u "
      "contours=%u soundings=%u pending=%u queued=%u memory_cache=%u",
      backend, valid ? 1 : 0, viewport ? viewport->pix_width : 0,
      viewport ? viewport->pix_height : 0, scale, zoom, status_.depth_label_size,
      static_cast<unsigned>(visible_tiles), static_cast<unsigned>(ready_tiles),
      static_cast<unsigned>(status_.base_tiles), static_cast<unsigned>(drawn_areas),
      static_cast<unsigned>(drawn_contours),
      static_cast<unsigned>(drawn_soundings),
      static_cast<unsigned>(requests_.size()),
      static_cast<unsigned>(download_queue_.size()),
      static_cast<unsigned>(cache_.size()));
}

bool OnlineChartsOverlay::Render(wxDC& wx_dc, PlugIn_ViewPort* viewport) {
  if (shutdown_ || !settings_.overlay_enabled) return false;
  const auto started = std::chrono::steady_clock::now();
  gl_pass_ = false;
  pixel_scale_ = 1.0;
  if (viewport && viewport->bValid) {
    last_viewport_ = std::make_unique<PlugIn_ViewPort>(*viewport);
  }
  const auto keys = VisibleTiles(viewport);
  piDC dc(wx_dc);
  if (viewport && viewport->bValid) dc.SetVP(viewport);
  RenderTiles(&dc, &wx_dc, viewport, keys);
  RecordRenderTime(started);
  return settings_.show_status || !keys.empty();
}

bool OnlineChartsOverlay::Render(wxGLContext* context, PlugIn_ViewPort* viewport) {
  (void)context;
  if (shutdown_ || !settings_.overlay_enabled) return false;
  const auto started = std::chrono::steady_clock::now();
  gl_pass_ = true;
  pixel_scale_ = online_charts::GlPixelScale();
  if (viewport && viewport->bValid) {
    last_viewport_ = std::make_unique<PlugIn_ViewPort>(*viewport);
  }
  const auto keys = VisibleTiles(viewport);

  // Draw with the compatibility fixed-function pipeline. Establish and later
  // restore the host state, including a shader left by the chart engine.
  glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
               GL_TEXTURE_BIT | GL_LINE_BIT | GL_CURRENT_BIT | GL_HINT_BIT);
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  const int previous_program = online_charts::SaveAndClearProgram();
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  RenderTiles(nullptr, nullptr, viewport, keys);

  const GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR && gl_error_log_count_ < 8) {
    ++gl_error_log_count_;
    last_error_ = wxString::Format("OpenGL state error 0x%x", gl_error);
    wxLogWarning("Online Charts: OpenGL state error=0x%x", gl_error);
  }
  online_charts::RestoreProgram(previous_program);
  glPopClientAttrib();
  glPopAttrib();
  RecordRenderTime(started);
  return settings_.show_status || !keys.empty();
}

void OnlineChartsOverlay::RecordRenderTime(std::chrono::steady_clock::time_point started) {
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count();
  status_.render_ms = render_frames_ ? status_.render_ms * 0.9 + ms * 0.1 : ms;
  render_ms_total_ += ms;
  if (++render_frames_ % 100 == 0 && timing_log_count_ < 20) {
    ++timing_log_count_;
    wxLogMessage("Online Charts: %s overlay average %.2f ms per frame over the last 100 frames",
                 gl_pass_ ? "OpenGL" : "wxDC", render_ms_total_ / 100.0);
    render_ms_total_ = 0.0;
  }
}

double OnlineChartsOverlay::LabelHeight(PlugIn_ViewPort* viewport) const {
  const double zoom = online_charts::ViewportZoom(viewport, pixel_scale_);
  const double logical = LabelPixels(zoom < 0.0 ? 12.0 : zoom - 1.0);
  return logical * settings_.label_scale / 100.0 * pixel_scale_;
}

size_t OnlineChartsOverlay::DrawAreasGl(PlugIn_ViewPort* viewport,
                                       std::vector<ReadyTile>& ready) {
  struct AreaRef {
    const AreaMesh* mesh;
    size_t tile;
  };
  std::vector<AreaRef> areas;
  std::vector<online_charts::TileProjector> projectors;
  projectors.reserve(ready.size());
  for (size_t i = 0; i < ready.size(); ++i) {
    CacheEntry* entry = ready[i].entry;
    if (!entry->meshes) {
      auto meshes = std::make_shared<std::vector<AreaMesh>>();
      for (const auto& area : entry->tile->areas) {
        AreaMesh mesh;
        if (!online_charts::Tessellate(area.rings, &mesh.triangles) || mesh.triangles.empty()) {
          continue;
        }
        mesh.colour = AreaColour(area);
        mesh.rank = area.rank;
        mesh.drval1 = area.drval1;
        meshes->push_back(std::move(mesh));
      }
      entry->meshes = meshes;
    }
    projectors.emplace_back(viewport, ready[i].key.z, ready[i].key.x, ready[i].key.y);
    for (const auto& mesh : *entry->meshes) areas.push_back(AreaRef{&mesh, i});
  }
  std::sort(areas.begin(), areas.end(), [](const AreaRef& a, const AreaRef& b) {
    if (a.mesh->rank != b.mesh->rank) return a.mesh->rank < b.mesh->rank;
    return a.mesh->drval1 < b.mesh->drval1;
  });

  std::vector<float> screen;
  glEnableClientState(GL_VERTEX_ARRAY);
  for (const auto& reference : areas) {
    const auto& triangles = reference.mesh->triangles;
    const auto& projector = projectors[reference.tile];
    screen.resize(triangles.size());
    for (size_t i = 0; i + 1 < triangles.size(); i += 2) {
      projector.Project(triangles[i], triangles[i + 1], &screen[i], &screen[i + 1]);
    }
    const wxColour& c = reference.mesh->colour;
    glColor4ub(c.Red(), c.Green(), c.Blue(), c.Alpha());
    glVertexPointer(2, GL_FLOAT, 0, screen.data());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(screen.size() / 2));
  }
  glDisableClientState(GL_VERTEX_ARRAY);
  return areas.size();
}

size_t OnlineChartsOverlay::DrawAreasDc(piDC& dc, wxDC* wx_dc, PlugIn_ViewPort* viewport,
                                       const std::vector<ReadyTile>& ready) {
  struct AreaRef {
    const online_charts::MvtArea* area;
    size_t tile;
  };
  std::vector<AreaRef> areas;
  std::vector<online_charts::TileProjector> projectors;
  projectors.reserve(ready.size());
  for (size_t i = 0; i < ready.size(); ++i) {
    projectors.emplace_back(viewport, ready[i].key.z, ready[i].key.x, ready[i].key.y);
    for (const auto& area : ready[i].entry->tile->areas) areas.push_back(AreaRef{&area, i});
  }
  std::sort(areas.begin(), areas.end(), [](const AreaRef& a, const AreaRef& b) {
    if (a.area->rank != b.area->rank) return a.area->rank < b.area->rank;
    return a.area->drval1 < b.area->drval1;
  });

#if wxUSE_GRAPHICS_CONTEXT
  wxGraphicsContext* graphics_context = nullptr;
  if (wx_dc) {
    if (wxWindowDC* window_dc = wxDynamicCast(wx_dc, wxWindowDC)) {
      graphics_context = wxGraphicsContext::Create(*window_dc);
    } else if (wxMemoryDC* memory_dc = wxDynamicCast(wx_dc, wxMemoryDC)) {
      graphics_context = wxGraphicsContext::Create(*memory_dc);
    }
  }
  std::unique_ptr<wxGraphicsContext> graphics(graphics_context);
#else
  (void)wx_dc;
#endif
  size_t drawn = 0;
  for (const auto& reference : areas) {
    const auto& projector = projectors[reference.tile];
    std::vector<std::vector<wxPoint>> rings;
    for (const auto& ring : reference.area->rings) {
      if (ring.size() < 3) continue;
      std::vector<wxPoint> points;
      points.reserve(ring.size());
      for (const auto& p : ring) {
        float sx = 0, sy = 0;
        projector.Project(p.x, p.y, &sx, &sy);
        points.emplace_back(static_cast<int>(std::lround(sx)), static_cast<int>(std::lround(sy)));
      }
      rings.push_back(std::move(points));
    }
    if (rings.empty()) continue;
    const wxColour colour = AreaColour(*reference.area);
#if wxUSE_GRAPHICS_CONTEXT
    if (graphics) {
      wxGraphicsPath path = graphics->CreatePath();
      for (const auto& ring : rings) {
        path.MoveToPoint(ring[0].x, ring[0].y);
        for (size_t i = 1; i < ring.size(); ++i) path.AddLineToPoint(ring[i].x, ring[i].y);
        path.CloseSubpath();
      }
      graphics->SetPen(*wxTRANSPARENT_PEN);
      graphics->SetBrush(wxBrush(colour));
      graphics->DrawPath(path, wxODDEVEN_RULE);
      ++drawn;
      continue;
    }
#endif
    std::vector<int> counts;
    std::vector<wxPoint> points;
    for (const auto& ring : rings) {
      counts.push_back(static_cast<int>(ring.size()));
      points.insert(points.end(), ring.begin(), ring.end());
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(colour));
    dc.DrawPolygonsTessellated(static_cast<int>(counts.size()), counts.data(), points.data());
    ++drawn;
  }
  return drawn;
}

void OnlineChartsOverlay::RenderTiles(piDC* dc, wxDC* wx_dc, PlugIn_ViewPort* viewport,
                                      const std::vector<TileKey>& keys) {
  if (!viewport || !viewport->bValid) return;
  // Draw the public raster reference map first, below all vector bathymetry.
  status_.base_tiles = base_map_ ? base_map_->Render(dc, viewport, pixel_scale_) : 0;
  status_.base_error = base_map_ ? base_map_->LastError() : wxString();

  std::vector<ReadyTile> ready;
  for (const auto& key : keys) {
    if (CacheEntry* entry = GetTile(key)) ready.push_back(ReadyTile{key, entry});
  }
  QueueDownloads(keys);

  size_t drawn_areas = 0;
  size_t drawn_contours = 0;
  size_t drawn_soundings = 0;
  if (settings_.show_shading && !ready.empty()) {
    drawn_areas = gl_pass_ ? DrawAreasGl(viewport, ready) : DrawAreasDc(*dc, wx_dc, viewport, ready);
  }

  const double map_zoom = online_charts::ViewportZoom(viewport, pixel_scale_) - 1.0;
  const float label_height = static_cast<float>(LabelHeight(viewport));
  status_.depth_label_size = static_cast<int>(std::lround(label_height / pixel_scale_));
  const float width = static_cast<float>(viewport->pix_width);
  const float height = static_cast<float>(viewport->pix_height);
  auto on_screen = [&](float x, float y) {
    return x >= 0 && y >= 0 && x < width && y < height;
  };
  std::vector<LabelCandidate> labels;

  if (settings_.show_contours && !ready.empty()) {
    if (gl_pass_) {
      glEnableClientState(GL_VERTEX_ARRAY);
      glEnable(GL_LINE_SMOOTH);
      glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    } else {
      dc->SetBrush(*wxTRANSPARENT_BRUSH);
    }
    std::vector<float> points;
    std::vector<wxPoint> dc_points;
    for (const auto& item : ready) {
      const online_charts::TileProjector projector(viewport, item.key.z, item.key.x, item.key.y);
      for (const auto& contour : item.entry->tile->contours) {
        if (contour.feet_system != settings_.depth_feet) continue;
        const bool has_depth = settings_.depth_feet ? contour.has_depth_ft : contour.has_depth_m;
        if (!has_depth) continue;
        const double depth = settings_.depth_feet ? contour.depth_ft : contour.depth_m;
        const bool major = std::fabs(std::fabs(depth) - (settings_.depth_feet ? 30.0 : 10.0)) < 0.01;
        const wxColour colour = major ? wxColour(76, 91, 99, 255) : wxColour(101, 127, 140, 255);
        if (gl_pass_) {
          glColor4ub(colour.Red(), colour.Green(), colour.Blue(), 230);
          glLineWidth(static_cast<GLfloat>(std::max(1.0, (major ? 1.6 : 0.9) * pixel_scale_)));
        } else {
          dc->SetPen(wxPen(colour, major ? 2 : 1));
        }
        for (const auto& line : contour.lines) {
          if (line.size() < 2) continue;
          points.resize(line.size() * 2);
          float min_x = std::numeric_limits<float>::max();
          float min_y = min_x;
          float max_x = -min_x;
          float max_y = -min_x;
          for (size_t i = 0; i < line.size(); ++i) {
            projector.Project(line[i].x, line[i].y, &points[i * 2], &points[i * 2 + 1]);
            min_x = std::min(min_x, points[i * 2]);
            max_x = std::max(max_x, points[i * 2]);
            min_y = std::min(min_y, points[i * 2 + 1]);
            max_y = std::max(max_y, points[i * 2 + 1]);
          }
          if (max_x < -20 || min_x > width + 20 || max_y < -20 || min_y > height + 20) continue;
          if (gl_pass_) {
            glVertexPointer(2, GL_FLOAT, 0, points.data());
            glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(line.size()));
          } else {
            dc_points.resize(line.size());
            for (size_t i = 0; i < line.size(); ++i) {
              dc_points[i] = wxPoint(static_cast<int>(std::lround(points[i * 2])),
                                     static_cast<int>(std::lround(points[i * 2 + 1])));
            }
            dc->DrawLines(static_cast<int>(dc_points.size()), dc_points.data());
          }
          ++drawn_contours;
          if (map_zoom >= 8.0) {
            const size_t middle = line.size() / 2;
            const float x = points[middle * 2];
            const float y = points[middle * 2 + 1];
            if (on_screen(x, y)) labels.push_back(LabelCandidate{DepthText(depth), wxString(), x, y, false});
          }
        }
      }
    }
    if (gl_pass_) {
      glDisable(GL_LINE_SMOOTH);
      glDisableClientState(GL_VERTEX_ARRAY);
    }
  }

  if (settings_.show_soundings && map_zoom >= 7.0 && !ready.empty()) {
    struct SoundingRef {
      size_t tile;
      const online_charts::MvtSounding* sounding;
      double depth;
    };
    std::vector<SoundingRef> soundings;
    for (size_t t = 0; t < ready.size(); ++t) {
      for (const auto& sounding : ready[t].entry->tile->soundings) {
        const bool has_depth = settings_.depth_feet ? sounding.has_depth_ft : sounding.has_depth_m;
        if (has_depth) {
          soundings.push_back(SoundingRef{
              t, &sounding, settings_.depth_feet ? sounding.depth_ft : sounding.depth_m});
        }
      }
    }
    // Shallow soundings first: they matter most and win label collisions.
    std::sort(soundings.begin(), soundings.end(),
              [](const SoundingRef& a, const SoundingRef& b) { return std::fabs(a.depth) < std::fabs(b.depth); });
    std::vector<std::unique_ptr<online_charts::TileProjector>> projectors(ready.size());
    for (const auto& reference : soundings) {
      auto& projector = projectors[reference.tile];
      if (!projector) {
        const TileKey& key = ready[reference.tile].key;
        projector = std::make_unique<online_charts::TileProjector>(viewport, key.z, key.x, key.y);
      }
      float x = 0, y = 0;
      projector->Project(reference.sounding->point.x, reference.sounding->point.y, &x, &y);
      if (!on_screen(x, y)) continue;
      LabelCandidate candidate;
      SoundingText(reference.depth, settings_.depth_feet, &candidate.text, &candidate.decimal);
      candidate.x = x;
      candidate.y = y;
      candidate.sounding = true;
      labels.push_back(candidate);
    }
  }
  if (!labels.empty()) DrawLabels(dc, viewport, labels, label_height, &drawn_soundings);
  // OpenSeaMap seamarks on top, as in the browser dashboard.
  status_.seamark_tiles = seamarks_ ? seamarks_->Render(dc, viewport, pixel_scale_) : 0;

  LogRenderState(gl_pass_ ? "OpenGL" : "wxDC", viewport, keys.size(), ready.size(),
                 drawn_areas, drawn_contours, drawn_soundings);
  DrawStatus(dc, viewport,
             StatusDetail(viewport, keys.size(), ready.size(), drawn_areas, drawn_contours,
                          drawn_soundings));
}

void OnlineChartsOverlay::DrawLabels(piDC* dc, PlugIn_ViewPort* viewport,
                                     const std::vector<LabelCandidate>& candidates,
                                     float height, size_t* drawn_soundings) {
  (void)viewport;
  const float decimal_height = height * 0.72f;
  const int margin = static_cast<int>(std::lround(4 * pixel_scale_));
  wxFont regular;
  wxFont italic;
  wxFont italic_small;
  if (gl_pass_) {
    text_regular_.Ensure(OpenCPNFont("ChartTexts", 12));
    text_italic_.Ensure(OpenCPNFont("ChartTexts", 12, wxFONTSTYLE_ITALIC));
    if (!text_regular_.Ready() || !text_italic_.Ready()) return;
  } else {
    const int pixels = std::max(6, static_cast<int>(std::lround(height)));
    regular = OpenCPNFont("ChartTexts", 12, wxFONTSTYLE_NORMAL, pixels);
    italic = OpenCPNFont("ChartTexts", 12, wxFONTSTYLE_ITALIC, pixels);
    italic_small = OpenCPNFont("ChartTexts", 12, wxFONTSTYLE_ITALIC,
                               std::max(5, static_cast<int>(std::lround(decimal_height))));
  }

  LabelGrid grid;
  size_t soundings = 0;
  for (const auto& label : candidates) {
    if (label.sounding && soundings >= 800) break;
    float whole_w = 0, line_h = 0, decimal_w = 0;
    if (gl_pass_) {
      const auto& atlas = label.sounding ? text_italic_ : text_regular_;
      atlas.Measure(label.text, height, &whole_w, &line_h);
      if (!label.decimal.empty()) text_italic_.Measure(label.decimal, decimal_height, &decimal_w, nullptr);
    } else {
      wxCoord w = 0, h = 0;
      dc->SetFont(label.sounding ? italic : regular);
      dc->GetTextExtent(label.text, &w, &h);
      whole_w = static_cast<float>(w);
      line_h = static_cast<float>(h);
      if (!label.decimal.empty()) {
        dc->SetFont(italic_small);
        dc->GetTextExtent(label.decimal, &w, &h);
        decimal_w = static_cast<float>(w);
      }
    }
    const float total_w = whole_w + decimal_w;
    const float left = label.x - total_w / 2;
    const float top = label.y - line_h / 2;
    const wxRect box(static_cast<int>(left) - margin, static_cast<int>(top) - margin,
                     static_cast<int>(total_w) + 2 * margin, static_cast<int>(line_h) + 2 * margin);
    if (!grid.Free(box)) continue;
    grid.Add(box);
    if (gl_pass_) {
      const auto& atlas = label.sounding ? text_italic_ : text_regular_;
      atlas.Draw(label.text, left, top, height, LabelColour(), HaloColour());
      if (!label.decimal.empty()) {
        text_italic_.Draw(label.decimal, left + whole_w, top + line_h * 0.42f, decimal_height,
                          LabelColour(), HaloColour());
      }
    } else {
      dc->SetFont(label.sounding ? italic : regular);
      DrawLabel(*dc, label.text, static_cast<int>(left), static_cast<int>(top), LabelColour());
      if (!label.decimal.empty()) {
        dc->SetFont(italic_small);
        DrawLabel(*dc, label.decimal, static_cast<int>(left + whole_w),
                  static_cast<int>(top + line_h * 0.42f), LabelColour());
      }
    }
    if (label.sounding) ++soundings;
  }
  *drawn_soundings = soundings;
}

wxString OnlineChartsOverlay::StatusDetail(PlugIn_ViewPort* viewport, size_t visible_tiles,
                                           size_t ready_tiles, size_t drawn_areas,
                                           size_t drawn_contours, size_t drawn_soundings) {
  wxString detail;
  if (visible_tiles == 0) {
    const int zoom = ZoomForViewport(viewport);
    if (zoom >= 0 && zoom < kMinZoom) {
      detail = "Online Charts: zoom in for bathymetry";
    } else {
      detail = "Online Charts: no valid tiles for this viewport";
    }
  } else if (ready_tiles == 0) {
    if (settings_.cache_mode == OnlineChartsCacheMode::kOfflineOnly) {
      detail = "Online Charts: offline mode; no cached tiles for this view";
    } else if (!requests_.empty() || !download_queue_.empty()) {
      detail = wxString::Format(
          "Online Charts: loading bathymetry (%u active, %u queued)",
          static_cast<unsigned>(requests_.size()),
          static_cast<unsigned>(download_queue_.size()));
    } else if (!last_error_.empty()) {
      detail = "Online Charts: " + last_error_;
    } else {
      detail = "Online Charts: no cached bathymetry for this view";
    }
  } else if (drawn_areas + drawn_contours + drawn_soundings == 0) {
    detail = "Online Charts: tiles ready; no enabled objects in this view";
  } else {
    detail = wxString::Format(
        "Online Charts: %u base tiles, %u areas, %u contours, %u soundings",
        static_cast<unsigned>(status_.base_tiles), static_cast<unsigned>(drawn_areas),
        static_cast<unsigned>(drawn_contours), static_cast<unsigned>(drawn_soundings));
  }
  status_.detail = detail;
  status_.active_requests = requests_.size() + (base_map_ ? base_map_->ActiveRequests() : 0) +
      (seamarks_ ? seamarks_->ActiveRequests() : 0);
  status_.queued_requests = download_queue_.size() + (base_map_ ? base_map_->QueuedRequests() : 0) +
      (seamarks_ ? seamarks_->QueuedRequests() : 0);
  return detail;
}

void OnlineChartsOverlay::DrawStatus(piDC* dc, PlugIn_ViewPort* viewport, const wxString& detail) {
  if (!viewport || !viewport->bValid) return;
  std::vector<std::pair<wxString, wxColour>> lines;
  if (settings_.show_status) lines.emplace_back(detail, wxColour(65, 85, 96));
  lines.emplace_back("Bathymetry: Open Waters / EMODnet / GEBCO", wxColour(80, 95, 100));
  if (settings_.show_seamarks) lines.emplace_back(wxString(L"Seamarks: \u00a9 OpenSeaMap contributors"), wxColour(80, 95, 100));
  if (settings_.show_base_map) {
    lines.emplace_back(settings_.base_map_provider == 0
                           ? wxString(L"Base map: \u00a9 OpenStreetMap contributors")
                           : wxString("Base map: Esri, Garmin, GEBCO, NOAA NGDC, and other contributors"),
                       wxColour(80, 95, 100));
  }
  // Keep clear of OpenCPN's own scale bar in the bottom-left corner.
  const float x = static_cast<float>(8 * pixel_scale_);
  float y = static_cast<float>(viewport->pix_height) - static_cast<float>(40 * pixel_scale_);
  if (gl_pass_) {
    text_regular_.Ensure(OpenCPNFont("StatusBar", 9));
    if (!text_regular_.Ready()) return;
    const float height = static_cast<float>(13 * pixel_scale_);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      y -= height * 1.15f;
      text_regular_.Draw(it->first, x, y, height, it->second, HaloColour());
    }
    return;
  }
  dc->SetBrush(*wxTRANSPARENT_BRUSH);
  dc->SetFont(OpenCPNFont("StatusBar", 9));
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    y -= 15;
    DrawLabel(*dc, it->first, static_cast<int>(x), static_cast<int>(y), it->second);
  }
}


OnlineChartsStatus OnlineChartsOverlay::Status() {
  status_.memory_tiles = cache_.size();
  status_.active_requests = requests_.size() + (base_map_ ? base_map_->ActiveRequests() : 0) +
      (seamarks_ ? seamarks_->ActiveRequests() : 0);
  status_.queued_requests = download_queue_.size() + (base_map_ ? base_map_->QueuedRequests() : 0) +
      (seamarks_ ? seamarks_->QueuedRequests() : 0);
  status_.last_error = last_error_;
  status_.base_error = base_map_ ? base_map_->LastError() : wxString();
  size_t disk_tiles = 0;
  DiskUsage(&status_.disk_bytes, &disk_tiles);
  status_.disk_tiles = disk_tiles;
  return status_;
}

wxString OnlineChartsOverlay::ClearCache() {
  CancelRequests();
  download_queue_.clear();
  queued_tiles_.clear();
  cache_.clear();
  failed_tiles_.clear();
  disk_missing_.clear();
  saved_area_.clear();
  size_t removed = 0;
  if (wxDirExists(cache_dir_)) {
    wxArrayString files;
    wxDir::GetAllFiles(cache_dir_, &files, "*.pbf*", wxDIR_FILES);
    for (const auto& path : files) {
      if (wxRemoveFile(path)) ++removed;
    }
  }
  status_.disk_tiles = 0;
  status_.disk_bytes = 0;
  last_error_.clear();
  RequestRefresh(GetOCPNCanvasWindow());
  return wxString::Format("Removed %u cached tiles. Downloads restart as needed.",
                          static_cast<unsigned>(removed));
}

wxString OnlineChartsOverlay::SaveVisibleArea() {
  if (!settings_.cache_enabled) return "Enable the offline cache first.";
  if (settings_.cache_mode == OnlineChartsCacheMode::kOfflineOnly) {
    return "Offline-only mode prevents downloads. Select another cache mode first.";
  }
  if (!last_viewport_ || !last_viewport_->bValid) {
    return "Open or move the chart first, then retry.";
  }
  int first_zoom = ZoomForViewport(last_viewport_.get());
  if (first_zoom < 0) return "The current chart viewport is not valid.";
  first_zoom = std::max(kMinZoom, first_zoom);
  const int last_zoom = std::min(kMaxZoom, first_zoom + settings_.prefetch_levels);
  std::vector<TileKey> planned;
  for (int zoom = first_zoom; zoom <= last_zoom; ++zoom) {
    const auto keys = VisibleTiles(last_viewport_.get(), zoom, kMaxPrefetchTiles);
    if (planned.size() + keys.size() > static_cast<size_t>(kMaxPrefetchTiles)) {
      return wxString::Format("More than %d tiles. Zoom in or reduce Extra zoom levels.",
                              kMaxPrefetchTiles);
    }
    planned.insert(planned.end(), keys.begin(), keys.end());
  }

  // The saved area must fit below the trimming target, or the cache would
  // delete part of it while the rest downloads.
  uint64_t disk_bytes = 0;
  size_t disk_files = 0;
  DiskUsage(&disk_bytes, &disk_files);
  const uint64_t tile_bytes = disk_files >= 20 ? disk_bytes / disk_files : kAssumedTileBytes;
  const uint64_t needed = tile_bytes * planned.size();
  const uint64_t target = static_cast<uint64_t>(settings_.cache_max_mb) * 1024 * 1024 * 9 / 10;
  if (needed > target) {
    return wxString::Format(
        "This area needs about %u MB, more than the %d MB cache allows. "
        "Raise Maximum cache size, zoom in or reduce Extra zoom levels.",
        static_cast<unsigned>(needed / (1024 * 1024) + 1), settings_.cache_max_mb);
  }

  // Tiles of the latest saved area are never trimmed, including fresh ones
  // that are skipped below and keep their older file times.
  saved_area_.clear();
  for (const auto& key : planned) saved_area_.insert(CacheFile(key).ToStdString());

  const size_t before = download_queue_.size() + requests_.size();
  QueueDownloads(planned);
  const size_t added = download_queue_.size() + requests_.size() >= before
                           ? download_queue_.size() + requests_.size() - before
                           : 0;
  return wxString::Format(
      "Save visible area: %u tiles planned (about %u MB); %u active or queued. "
      "Fresh cached tiles are skipped.",
      static_cast<unsigned>(planned.size()),
      static_cast<unsigned>(needed / (1024 * 1024) + 1), static_cast<unsigned>(added));
}

void OnlineChartsOverlay::DiskUsage(uint64_t* bytes, size_t* files) const {
  *bytes = 0;
  *files = 0;
  if (cache_dir_.empty() || !wxDirExists(cache_dir_)) return;
  wxArrayString paths;
  wxDir::GetAllFiles(cache_dir_, &paths, "*.pbf", wxDIR_FILES);
  *files = paths.size();
  for (const auto& path : paths) {
    const wxULongLong size = wxFileName(path).GetSize();
    if (size != wxInvalidSize) *bytes += size.GetValue();
  }
}

void OnlineChartsOverlay::MaintainDiskCache() {
  if (!settings_.cache_enabled || cache_dir_.empty() || !wxDirExists(cache_dir_)) return;
  wxArrayString files;
  wxDir::GetAllFiles(cache_dir_, &files, "*.pbf", wxDIR_FILES);
  struct DiskFile {
    wxString path;
    wxDateTime modified;
    wxULongLong size;
  };
  std::vector<DiskFile> disk_files;
  wxULongLong total = 0;
  for (const auto& path : files) {
    wxFileName file(path);
    DiskFile item{path, file.GetModificationTime(), file.GetSize()};
    total += item.size;
    disk_files.push_back(item);
  }
  std::sort(disk_files.begin(), disk_files.end(), [](const DiskFile& a, const DiskFile& b) {
    return a.modified < b.modified;
  });
  const uint64_t configured = static_cast<uint64_t>(settings_.cache_max_mb) * 1024 * 1024;
  const wxULongLong limit(configured);
  const wxULongLong target(configured * 9 / 10);
  size_t remaining = disk_files.size();
  if (total <= limit && remaining <= kMaxDiskTiles) return;
  // Once over a limit, trim the oldest downloads to 90% so the next writes do
  // not trigger another pass straight away.
  const size_t target_files = kMaxDiskTiles * 9 / 10;
  for (const auto& file : disk_files) {
    if (total <= target && remaining <= target_files) break;
    if (saved_area_.count(file.path.ToStdString())) continue;
    if (wxRemoveFile(file.path)) {
      total -= file.size;
      --remaining;
    }
  }
}
