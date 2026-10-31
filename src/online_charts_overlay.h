#ifndef ONLINE_CHARTS_OVERLAY_H_
#define ONLINE_CHARTS_OVERLAY_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/webrequest.h>
#if !wxUSE_WEBREQUEST
#include <wx/timer.h>
#endif

#include "online_charts_mvt.h"
#include "online_charts_render.h"

class OnlineChartsLandLayer;
class PlugIn_ViewPort;
class piDC;
class wxDC;
class wxGLContext;

enum class OnlineChartsCacheMode {
  kNetworkFirst = 0,
  kCacheFirst = 1,
  kOfflineOnly = 2,
};

struct OnlineChartsSettings {
  bool overlay_enabled = true;
  bool show_contours = true;
  bool show_soundings = true;
  bool show_shading = true;
  bool show_status = true;
  bool show_base_map = true;
  int base_map_provider = 1;  // 0 = OpenStreetMap, 1 = Esri World Ocean Base.
  bool show_seamarks = true;  // OpenSeaMap buoys, lights, beacons and areas.
  bool depth_feet = false;
  int label_scale = 100;  // Percent of the zoom-dependent depth label size.
  bool cache_enabled = true;
  OnlineChartsCacheMode cache_mode = OnlineChartsCacheMode::kNetworkFirst;
  int cache_max_mb = 256;
  int cache_max_age_days = 1;
  int prefetch_levels = 2;
};

struct OnlineChartsStatus {
  size_t visible_tiles = 0;
  size_t ready_tiles = 0;
  size_t drawn_areas = 0;
  size_t drawn_contours = 0;
  size_t drawn_soundings = 0;
  size_t base_tiles = 0;
  size_t seamark_tiles = 0;
  size_t memory_tiles = 0;
  size_t disk_tiles = 0;
  size_t active_requests = 0;
  size_t queued_requests = 0;
  uint64_t disk_bytes = 0;
  uint64_t downloads_succeeded = 0;
  uint64_t downloads_failed = 0;
  int zoom = 0;
  int depth_label_size = 0;  // Current label height in logical pixels.
  double render_ms = 0.0;    // Average overlay drawing time per frame.
  bool viewport_valid = false;
  wxString detail;
  wxString last_error;
  wxString base_error;
};

class OnlineChartsOverlay : public wxEvtHandler {
 public:
  OnlineChartsOverlay();
  ~OnlineChartsOverlay() override;

  void Configure(const OnlineChartsSettings& settings);
  const OnlineChartsSettings& Settings() const { return settings_; }
  void Shutdown();
  bool Render(wxDC& dc, PlugIn_ViewPort* viewport);
  bool Render(wxGLContext* context, PlugIn_ViewPort* viewport);

  OnlineChartsStatus Status();
  wxString ClearCache();
  wxString SaveVisibleArea();

 private:
  struct TileKey {
    int z = 0;
    int x = 0;
    int y = 0;
    std::string id;
  };

  // Triangles of each depth area in tile coordinates, built once per tile on
  // first OpenGL use instead of tessellating every polygon on every frame.
  struct AreaMesh {
    std::vector<float> triangles;
    wxColour colour;
    double rank = 0.0;
    double drval1 = 0.0;
  };

  struct CacheEntry {
    std::shared_ptr<const online_charts::MvtTile> tile;
    std::time_t expires = 0;
    uint64_t last_used = 0;
    std::shared_ptr<std::vector<AreaMesh>> meshes;
  };

  struct ReadyTile {
    TileKey key;
    CacheEntry* entry = nullptr;
  };

  struct LabelCandidate {
    wxString text;
    wxString decimal;  // Sounding decimal, drawn as a subscript.
    float x = 0;
    float y = 0;
    bool sounding = false;
  };

  struct PendingRequest {
    TileKey key;
    std::vector<uint8_t> bytes;
#if wxUSE_WEBREQUEST
    wxWebRequest request;
#else
    void* easy = nullptr;
#endif
  };

  int ZoomForViewport(PlugIn_ViewPort* viewport) const;
  std::vector<TileKey> VisibleTiles(PlugIn_ViewPort* viewport,
                                    int forced_zoom = -1,
                                    size_t maximum = 96) const;
  CacheEntry* GetTile(const TileKey& key);
  bool HasFreshDiskTile(const TileKey& key, std::time_t now) const;
  void QueueDownloads(const std::vector<TileKey>& keys);
  void StartQueuedDownloads();
  void StartDownload(const TileKey& key);
  void CancelRequests();
  void CompleteDownload(int id, bool transport_ok, long http_status,
                        std::vector<uint8_t> bytes);
#if wxUSE_WEBREQUEST
  void OnWebRequestState(wxWebRequestEvent& event);
#else
  void OnCurlTimer(wxTimerEvent& event);
#endif
  void RenderTiles(piDC* dc, wxDC* wx_dc, PlugIn_ViewPort* viewport,
                   const std::vector<TileKey>& keys);
  void RecordRenderTime(std::chrono::steady_clock::time_point started);
  size_t DrawAreasGl(PlugIn_ViewPort* viewport, std::vector<ReadyTile>& ready);
  size_t DrawAreasDc(piDC& dc, wxDC* wx_dc, PlugIn_ViewPort* viewport,
                     const std::vector<ReadyTile>& ready);
  void DrawLabels(piDC* dc, PlugIn_ViewPort* viewport,
                  const std::vector<LabelCandidate>& candidates, float height,
                  size_t* drawn_soundings);
  wxString StatusDetail(PlugIn_ViewPort* viewport, size_t visible_tiles, size_t ready_tiles,
                        size_t drawn_areas, size_t drawn_contours, size_t drawn_soundings);
  void DrawStatus(piDC* dc, PlugIn_ViewPort* viewport, const wxString& detail);
  double LabelHeight(PlugIn_ViewPort* viewport) const;
  void LogRenderState(const char* backend, PlugIn_ViewPort* viewport,
                      size_t visible_tiles, size_t ready_tiles,
                      size_t drawn_areas, size_t drawn_contours,
                      size_t drawn_soundings);
  void TrimMemoryCache();
  void MaintainDiskCache();
  wxString CacheFile(const TileKey& key) const;
  std::time_t TileLifetime() const;

#if wxUSE_WEBREQUEST
  wxWebSession session_;
#else
  void* curl_multi_ = nullptr;
  wxTimer curl_timer_;
#endif
  std::unique_ptr<OnlineChartsLandLayer> base_map_;
  std::unique_ptr<OnlineChartsLandLayer> seamarks_;  // OpenSeaMap, drawn above the bathymetry.
  OnlineChartsSettings settings_;
  wxString cache_dir_;
  std::unordered_map<std::string, CacheEntry> cache_;
  std::map<int, PendingRequest> requests_;  // Stable storage for libcurl callbacks.
  std::unordered_map<std::string, int> pending_tiles_;
  std::unordered_map<std::string, std::time_t> failed_tiles_;
  std::unordered_map<std::string, std::time_t> disk_missing_;  // Avoids a disk lookup per frame.
  std::deque<TileKey> download_queue_;
  std::unordered_set<std::string> queued_tiles_;
  std::unique_ptr<PlugIn_ViewPort> last_viewport_;
  OnlineChartsStatus status_;
  uint64_t use_counter_ = 0;
  int next_request_id_ = 20000;
  unsigned disk_writes_ = 0;
  unsigned network_log_count_ = 0;
  unsigned render_log_count_ = 0;
  unsigned gl_error_log_count_ = 0;
  std::string last_render_state_;
  online_charts::GlTextAtlas text_regular_;
  online_charts::GlTextAtlas text_italic_;
  double pixel_scale_ = 1.0;  // Canvas pixels per logical pixel for the current pass.
  bool gl_pass_ = false;
  double render_ms_total_ = 0.0;
  unsigned render_frames_ = 0;
  unsigned timing_log_count_ = 0;
  wxString last_error_;
  bool shutdown_ = false;
};

#endif  // ONLINE_CHARTS_OVERLAY_H_
