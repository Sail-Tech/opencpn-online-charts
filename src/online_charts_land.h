#ifndef ONLINE_CHARTS_LAND_H_
#define ONLINE_CHARTS_LAND_H_

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

#include <wx/bitmap.h>
#include <wx/event.h>
#include <wx/image.h>
#include <wx/string.h>

class OnlineChartsHttp;
class PlugIn_ViewPort;
class piDC;

// Network-only public raster layers: the base map (0 OpenStreetMap, 1 Esri
// World Ocean Base), OpenSeaMap seamarks (2) and the depth colours (3), from
// the Open Waters elevation raster. They deliberately have no disk cache or
// prefetcher; only visible tiles are requested and retained in bounded memory.
class OnlineChartsLandLayer : public wxEvtHandler {
 public:
  OnlineChartsLandLayer();
  ~OnlineChartsLandLayer() override;

  // opacity is a percentage; the base map below 100 lets the OpenCPN chart show through.
  void Configure(bool enabled, int provider, bool allow_network, int opacity);
  // Depth colours only: Chart (0) or Topo (1). A change reloads the tiles.
  void SetDepthColours(int scheme);
  void Shutdown();
  // Draws the visible tiles. dc is null for the OpenGL canvas, where tiles are
  // uploaded once as textures; pixel_scale is canvas pixels per logical pixel.
  size_t Render(piDC* dc, PlugIn_ViewPort* viewport, double pixel_scale);

  wxString LastError() const { return last_error_; }
  size_t ReadyTiles() const { return images_.size(); }
  size_t ActiveRequests() const;
  size_t QueuedRequests() const;

 private:
  struct TileKey {
    int z = 0;
    int x = 0;
    int y = 0;
    std::string id;
  };
  struct CachedImage {
    wxImage image;
    uint64_t last_used = 0;
    unsigned texture = 0;  // OpenGL texture, created on first OpenGL draw.
    wxBitmap scaled;       // wxDC canvas: image scaled to its last drawn size.
  };

  int ZoomForViewport(PlugIn_ViewPort* viewport) const;
  std::vector<TileKey> VisibleTiles(PlugIn_ViewPort* viewport) const;
  // Replaces tiles known to be missing by their nearest ancestor that may exist.
  std::vector<TileKey> AvailableTiles(const std::vector<TileKey>& keys) const;
  void QueueDownloads(const std::vector<TileKey>& keys);
  void StartQueuedDownloads();
  void StartDownload(const TileKey& key);
  void CancelRequests();
  void TrimMemoryCache();
  void ClearImages();
  size_t RenderGl(PlugIn_ViewPort* viewport, const std::vector<TileKey>& keys);
  size_t RenderDc(piDC& dc, PlugIn_ViewPort* viewport, const std::vector<TileKey>& keys);
  void CompleteDownload(const TileKey& key, bool transport_ok, long http_status,
                        const std::string& bytes);

  std::unordered_map<std::string, CachedImage> images_;
  // Textures of evicted tiles, deleted on the next OpenGL pass when the
  // context is current.
  std::vector<unsigned> textures_to_delete_;
  double pixel_scale_ = 1.0;
  std::unordered_map<std::string, std::time_t> failed_tiles_;
  wxString last_error_;
  uint64_t use_counter_ = 0;
  int provider_ = 0;
  unsigned char alpha_ = 255;
  int depth_colours_ = 0;
  // Esri World Ocean Base tiles that do not exist (HTTP 404): detailed tiles
  // are regional, so their parent is drawn magnified instead.
  std::unordered_set<std::string> missing_tiles_;
  bool enabled_ = true;
  bool allow_network_ = true;
  bool shutdown_ = false;

  // Shared HTTPS client: wxWebRequest on macOS and Windows, libcurl on Linux.
  std::unique_ptr<OnlineChartsHttp> http_;
  std::unordered_set<std::string> pending_tiles_;
  std::deque<TileKey> download_queue_;
  std::unordered_set<std::string> queued_tiles_;
};

#endif  // ONLINE_CHARTS_LAND_H_
