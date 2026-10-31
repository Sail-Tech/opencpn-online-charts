#ifndef ONLINE_CHARTS_PI_H_
#define ONLINE_CHARTS_PI_H_

#include "wx/wxprec.h"
#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include <memory>

#include <wx/timer.h>

#include "config.h"
#include "ocpn_plugin.h"
#include "online_charts_server.h"
#include "online_charts_overlay.h"
#include "online_charts_weather.h"
#include "online_charts_weather_window.h"

class OnlineChartsPi : public opencpn_plugin_118 {
 public:
  explicit OnlineChartsPi(void* plugin_manager);
  ~OnlineChartsPi() override;

  int Init() override;
  bool DeInit() override;

  int GetAPIVersionMajor() override;
  int GetAPIVersionMinor() override;
  int GetPlugInVersionMajor() override;
  int GetPlugInVersionMinor() override;
  int GetPlugInVersionPatch() override;
  int GetPlugInVersionPost() override;
  const char* GetPlugInVersionPre() override;
  const char* GetPlugInVersionBuild() override;

  wxBitmap* GetPlugInBitmap() override;
  wxString GetCommonName() override;
  wxString GetShortDescription() override;
  wxString GetLongDescription() override;

  int GetToolbarToolCount() override;
  void OnToolbarToolCallback(int id) override;
  void ShowPreferencesDialog(wxWindow* parent) override;
  void SetPositionFix(PlugIn_Position_Fix& fix) override;
  bool RenderOverlay(wxDC& dc, PlugIn_ViewPort* viewport) override;
  bool RenderGLOverlay(wxGLContext* context, PlugIn_ViewPort* viewport) override;
  bool RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* viewport,
                                int canvasIndex, int priority) override;
  bool RenderGLOverlayMultiCanvas(wxGLContext* context,
                                  PlugIn_ViewPort* viewport, int canvasIndex,
                                  int priority) override;
  bool MouseEventHook(wxMouseEvent& event) override;

 private:
  void ApplyWeather(const OnlineChartsWeatherSettings& settings);
  void ShowWeatherWindow(bool show);
  void OpenProviderMenu();

  int toolbar_tool_id_ = -1;
  int weather_tool_id_ = -1;
  int provider_tool_id_ = -1;
  OnlineChartsWeatherWindow* weather_window_ = nullptr;  // Owned by wxWidgets.
  bool weather_window_shown_ = true;
  bool charts_pass_seen_ = false;  // OpenCPN calls the chart-level overlay pass.
  wxBitmap panel_bitmap_;
  std::unique_ptr<OnlineChartsServer> server_;
  std::unique_ptr<OnlineChartsOverlay> overlay_;
  std::unique_ptr<OnlineChartsWeather> weather_;
  OnlineChartsSettings settings_;
  OnlineChartsWeatherSettings weather_settings_;
  bool overlay_enabled_ = true;
#ifdef ONLINE_CHARTS_BENCHMARK
  wxTimer benchmark_timer_;
  int benchmark_ticks_ = 0;
#endif
};

#endif  // ONLINE_CHARTS_PI_H_
