#include "online_charts_pi.h"

#include "online_charts_geohash.h"
#include "online_charts_overlay.h"
#include "online_charts_preferences.h"
#include "pidc.h"

#include <wx/fileconf.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

extern "C" DECL_EXP opencpn_plugin* create_pi(void* plugin_manager) {
  wxLogMessage("Online Charts: create_pi");
  return new OnlineChartsPi(plugin_manager);
}

extern "C" DECL_EXP void destroy_pi(opencpn_plugin* plugin) {
  wxLogMessage("Online Charts: destroy_pi");
  delete plugin;
}

namespace {
// Chart-level overlay pass of newer OpenCPN versions (OVERLAY_CHARTS in plugin
// API 1.21), called by OpenCPN 5.14 for API 1.18 plugins as well.
constexpr int kOverlayCharts = 256;

int MinorVersion() {
  const std::string version(API_VERSION);
  const auto dot = version.find('.');
  return dot == std::string::npos ? 0 : std::atoi(version.substr(dot + 1).c_str());
}

// Runs a program with separate arguments (no shell quoting). With `wait`, the
// result is the program's exit code; otherwise 0 when it started.
long Run(const std::vector<wxString>& args, bool wait) {
  std::vector<const wchar_t*> argv;
  for (const auto& arg : args) argv.push_back(arg.wc_str());
  argv.push_back(nullptr);
  if (wait) return wxExecute(argv.data(), wxEXEC_SYNC | wxEXEC_NOEVENTS);
  return wxExecute(argv.data(), wxEXEC_ASYNC) > 0 ? 0 : -1;
}

// Opens a page in Google Chrome or Chromium, where the GPS Charts extension can
// act on it (it places Garmin's marker). False when neither is installed.
bool OpenInChrome(const wxString& url) {
#if defined(__WXOSX__)
  // `open -b` exits with an error when no application has that bundle id.
  for (const char* bundle : {"com.google.Chrome", "org.chromium.Chromium"}) {
    if (Run({"open", "-b", bundle, url}, true) == 0) return true;
  }
  return false;
#elif defined(__WXMSW__)
  for (const char* base : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
    wxString root;
    if (!wxGetEnv(base, &root) || root.empty()) continue;
    const wxString chrome = root + "\\Google\\Chrome\\Application\\chrome.exe";
    if (wxFileName::FileExists(chrome) && Run({chrome, url}, false) == 0) return true;
  }
  return false;
#else
  wxPathList path;
  path.AddEnvList("PATH");
  for (const char* name : {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser"}) {
    const wxString program = path.FindAbsoluteValidPath(name);
    if (!program.empty() && Run({program, url}, false) == 0) return true;
  }
  return false;
#endif
}

wxString DataFile(const wxString& name) {
  wxString path = GetPluginDataDir(PKG_NAME);
  path += wxFileName::GetPathSeparator();
  path += "data";
  path += wxFileName::GetPathSeparator();
  path += name;
  return path;
}

std::string Decimal(double value, int precision) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", precision, value);
  std::string result(text);
  for (char& c : result) {
    if (c == ',') c = '.';
  }
  return result;
}

void JsonNumber(std::string* out, const char* name, double value,
                int precision, bool* first) {
  if (!std::isfinite(value)) return;
  if (!*first) *out += ',';
  *first = false;
  *out += '\"';
  *out += name;
  *out += "\":";
  *out += Decimal(value, precision);
}

std::string IsoTime(std::time_t value) {
  if (value <= 0) return {};
  std::tm tm{};
#ifdef _WIN32
  if (gmtime_s(&tm, &value) != 0) return {};
#else
  if (!gmtime_r(&value, &tm)) return {};
#endif
  char text[32];
  const int length = std::snprintf(
      text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return length > 0 ? std::string(text, static_cast<size_t>(length)) : std::string();
}

OnlineChartsSettings LoadSettings() {
  OnlineChartsSettings settings;
  wxFileConfig* config = GetOCPNConfigObject();
  if (!config) return settings;
  const wxString previous_path = config->GetPath();
  config->SetPath("/PlugIns/OnlineCharts");
  config->Read("OverlayEnabled", &settings.overlay_enabled, true);
  config->Read("ShowContours", &settings.show_contours, true);
  config->Read("ShowSoundings", &settings.show_soundings, true);
  config->Read("ShowShading", &settings.show_shading, true);
  config->Read("ShowStatus", &settings.show_status, true);
  config->Read("ShowBaseMap", &settings.show_base_map, true);
  config->Read("BaseMapProvider", &settings.base_map_provider, 1);
  config->Read("ShowSeamarks", &settings.show_seamarks, true);
  settings.base_map_provider = std::max(0, std::min(1, settings.base_map_provider));
  config->Read("DepthFeet", &settings.depth_feet, false);
  // 1.4.0 replaced the fixed point size with a percentage of the zoom-dependent
  // size used by the browser dashboard; the old DepthLabelSize value is ignored.
  config->Read("DepthLabelScale", &settings.label_scale, 100);
  settings.label_scale = std::max(50, std::min(300, settings.label_scale));
  config->Read("CacheEnabled", &settings.cache_enabled, true);
  int mode = 0;
  config->Read("CacheMode", &mode, 0);
  settings.cache_mode = mode >= 0 && mode <= 2
                            ? static_cast<OnlineChartsCacheMode>(mode)
                            : OnlineChartsCacheMode::kNetworkFirst;
  config->Read("CacheMaxMB", &settings.cache_max_mb, 256);
  config->Read("CacheMaxAgeDays", &settings.cache_max_age_days, 1);
  config->Read("PrefetchLevels", &settings.prefetch_levels, 2);
  config->SetPath(previous_path);
  return settings;
}

OnlineChartsWeatherSettings LoadWeatherSettings() {
  OnlineChartsWeatherSettings settings;
  wxFileConfig* config = GetOCPNConfigObject();
  if (!config) return settings;
  const wxString previous_path = config->GetPath();
  config->SetPath("/PlugIns/OnlineCharts");
  config->Read("WeatherEnabled", &settings.enabled, true);
  config->Read("WeatherPanel", &settings.show_panel, false);
  config->Read("WeatherTimeline", &settings.show_timeline, true);
  config->Read("WeatherWindBarb", &settings.wind_barb, true);
  config->Read("WeatherWindField", &settings.wind_field, true);
  config->Read("WeatherWaveField", &settings.wave_field, true);
  config->Read("WeatherCurrentField", &settings.current_field, true);
  config->Read("WeatherModel", &settings.model, 0);
  config->Read("WeatherFahrenheit", &settings.temp_fahrenheit, false);
  config->Read("WeatherWaveFeet", &settings.wave_feet, false);
  config->Read("WeatherFreshMinutes", &settings.fresh_minutes, 60);
  config->Read("WeatherKeepHours", &settings.keep_hours, 72);
  // The forecast hour is a viewing choice, not a setting: it starts at "now".
  config->SetPath(previous_path);
  return settings;
}

void SaveWeatherSettings(const OnlineChartsWeatherSettings& settings) {
  wxFileConfig* config = GetOCPNConfigObject();
  if (!config) return;
  const wxString previous_path = config->GetPath();
  config->SetPath("/PlugIns/OnlineCharts");
  config->Write("WeatherEnabled", settings.enabled);
  config->Write("WeatherPanel", settings.show_panel);
  config->Write("WeatherTimeline", settings.show_timeline);
  config->Write("WeatherWindBarb", settings.wind_barb);
  config->Write("WeatherWindField", settings.wind_field);
  config->Write("WeatherWaveField", settings.wave_field);
  config->Write("WeatherCurrentField", settings.current_field);
  config->Write("WeatherModel", settings.model);
  config->Write("WeatherFahrenheit", settings.temp_fahrenheit);
  config->Write("WeatherWaveFeet", settings.wave_feet);
  config->Write("WeatherFreshMinutes", settings.fresh_minutes);
  config->Write("WeatherKeepHours", settings.keep_hours);
  config->Flush();
  config->SetPath(previous_path);
}

void SaveSettings(const OnlineChartsSettings& settings) {
  wxFileConfig* config = GetOCPNConfigObject();
  if (!config) {
    wxLogWarning("Online Charts: OpenCPN configuration is unavailable");
    return;
  }
  const wxString previous_path = config->GetPath();
  config->SetPath("/PlugIns/OnlineCharts");
  config->Write("OverlayEnabled", settings.overlay_enabled);
  config->Write("ShowContours", settings.show_contours);
  config->Write("ShowSoundings", settings.show_soundings);
  config->Write("ShowShading", settings.show_shading);
  config->Write("ShowStatus", settings.show_status);
  config->Write("ShowBaseMap", settings.show_base_map);
  config->Write("BaseMapProvider", settings.base_map_provider);
  config->Write("ShowSeamarks", settings.show_seamarks);
  config->Write("DepthFeet", settings.depth_feet);
  config->Write("DepthLabelScale", settings.label_scale);
  config->Write("CacheEnabled", settings.cache_enabled);
  config->Write("CacheMode", static_cast<int>(settings.cache_mode));
  config->Write("CacheMaxMB", settings.cache_max_mb);
  config->Write("CacheMaxAgeDays", settings.cache_max_age_days);
  config->Write("PrefetchLevels", settings.prefetch_levels);
  config->Flush();
  config->SetPath(previous_path);
}
}  // namespace

OnlineChartsPi::OnlineChartsPi(void* plugin_manager)
    : opencpn_plugin_118(plugin_manager) {}

OnlineChartsPi::~OnlineChartsPi() = default;

int OnlineChartsPi::Init() {
  wxLogMessage("Online Charts: Init started");
  const wxString root = DataFile("online_charts");
  server_ = std::make_unique<OnlineChartsServer>(root.ToStdString());
  settings_ = LoadSettings();
  overlay_enabled_ = settings_.overlay_enabled;
  overlay_ = std::make_unique<OnlineChartsOverlay>();
  overlay_->Configure(settings_);
  weather_settings_ = LoadWeatherSettings();
  weather_ = std::make_unique<OnlineChartsWeather>();
  weather_->Configure(weather_settings_);

  const wxString icon = DataFile("online_charts_pi.svg");
  const wxString toggled = DataFile("online_charts_pi_toggled.svg");
  if (wxFileName::FileExists(icon)) {
    toolbar_tool_id_ = InsertPlugInToolSVG(
        "Online Charts", icon, icon, toggled, wxITEM_CHECK, "Online Charts",
        "Toggle online vector bathymetry over the active OpenCPN chart",
        nullptr, -1, 0, this);
    SetToolbarItemState(toolbar_tool_id_, overlay_enabled_);
    panel_bitmap_ = GetBitmapFromSVGFile(icon, 48, 48);
  } else {
    wxLogWarning("Online Charts: toolbar icon not found: %s", icon);
  }
#ifdef ONLINE_CHARTS_BENCHMARK
  // Local test builds only: redraw the chart repeatedly so the overlay's
  // per-frame drawing time is logged without user interaction.
  benchmark_timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
    if (++benchmark_ticks_ > 400) {
      benchmark_timer_.Stop();
      return;
    }
    if (benchmark_ticks_ > 100) RequestRefresh(GetOCPNCanvasWindow());
    if (benchmark_ticks_ == 380 && weather_) {
      // Press the left button where the cursor is, to test the time bar.
      wxMouseEvent press(wxEVT_LEFT_DOWN);
      press.SetEventObject(GetOCPNCanvasWindow());
      press.SetLeftDown(true);
      wxLogMessage("Online Charts: test press used=%d playing=%d hour=%d", weather_->OnMouse(press) ? 1 : 0,
                   weather_->Playing() ? 1 : 0, weather_->ForecastHour());
      wxMouseEvent release(wxEVT_LEFT_UP);
      release.SetEventObject(GetOCPNCanvasWindow());
      weather_->OnMouse(release);
    }
  });
  benchmark_timer_.Start(30);
#endif
  wxLogMessage("Online Charts: Init complete");
  const wxString weather_icon = DataFile("online_charts_weather.svg");
  const wxString weather_toggled = DataFile("online_charts_weather_toggled.svg");
  if (wxFileName::FileExists(weather_icon)) {
    weather_tool_id_ = InsertPlugInToolSVG(
        "Weather", weather_icon, weather_icon, weather_toggled, wxITEM_CHECK, "Weather",
        "Show or hide the Weather window (conditions, wind, waves and forecast model)",
        nullptr, -1, 0, this);
  }
  const wxString provider_icon = DataFile("online_charts_provider.svg");
  if (wxFileName::FileExists(provider_icon)) {
    provider_tool_id_ = InsertPlugInToolSVG(
        "Open provider", provider_icon, provider_icon, provider_icon, wxITEM_NORMAL,
        "Open provider at the boat position",
        "Open i-Boating, C-MAP or Garmin Marine in the web browser at the boat position (or the chart centre)",
        nullptr, -1, 0, this);
  }
  if (wxFileConfig* config = GetOCPNConfigObject()) {
    const wxString previous_path = config->GetPath();
    config->SetPath("/PlugIns/OnlineCharts");
    config->Read("WeatherWindowShown", &weather_window_shown_, true);
    config->SetPath(previous_path);
  }
  // Open the window once OpenCPN has finished building its main frame.
  if (weather_window_shown_) {
    GetOCPNCanvasWindow()->CallAfter([this]() { ShowWeatherWindow(true); });
  } else if (weather_tool_id_ >= 0) {
    SetToolbarItemState(weather_tool_id_, false);
  }
  return WANTS_TOOLBAR_CALLBACK | INSTALLS_TOOLBAR_TOOL | WANTS_OVERLAY_CALLBACK |
         WANTS_OPENGL_OVERLAY_CALLBACK | WANTS_PREFERENCES | WANTS_NMEA_EVENTS |
         WANTS_MOUSE_EVENTS;
}

bool OnlineChartsPi::DeInit() {
  wxLogMessage("Online Charts: DeInit started");
  if (weather_window_) {
    // Delete now rather than with Destroy(): Destroy() defers the deletion of
    // a top-level window until OpenCPN exits, after this plugin library has
    // been unloaded, and the destructor call then crashes OpenCPN.
    delete weather_window_;
    weather_window_ = nullptr;
  }
  if (weather_tool_id_ >= 0) {
    RemovePlugInTool(weather_tool_id_);
    weather_tool_id_ = -1;
  }
  if (provider_tool_id_ >= 0) {
    RemovePlugInTool(provider_tool_id_);
    provider_tool_id_ = -1;
  }
  if (weather_) {
    weather_->Shutdown();
    weather_.reset();
  }
  if (overlay_) {
    overlay_->Shutdown();
    wxLogMessage("Online Charts: overlay stopped");
    overlay_.reset();
  }
  if (server_) server_->Stop();
  if (toolbar_tool_id_ >= 0) {
    RemovePlugInTool(toolbar_tool_id_);
    toolbar_tool_id_ = -1;
  }
  wxLogMessage("Online Charts: DeInit complete");
  return true;
}

int OnlineChartsPi::GetAPIVersionMajor() { return std::atoi(API_VERSION); }
int OnlineChartsPi::GetAPIVersionMinor() { return MinorVersion(); }
int OnlineChartsPi::GetPlugInVersionMajor() { return PLUGIN_VERSION_MAJOR; }
int OnlineChartsPi::GetPlugInVersionMinor() { return PLUGIN_VERSION_MINOR; }
int OnlineChartsPi::GetPlugInVersionPatch() { return PLUGIN_VERSION_PATCH; }
int OnlineChartsPi::GetPlugInVersionPost() { return PLUGIN_VERSION_TWEAK; }
const char* OnlineChartsPi::GetPlugInVersionPre() { return PKG_PRERELEASE; }
const char* OnlineChartsPi::GetPlugInVersionBuild() { return PKG_BUILD_INFO; }
wxBitmap* OnlineChartsPi::GetPlugInBitmap() { return &panel_bitmap_; }
wxString OnlineChartsPi::GetCommonName() { return PLUGIN_API_NAME; }
wxString OnlineChartsPi::GetShortDescription() { return PKG_SUMMARY; }
wxString OnlineChartsPi::GetLongDescription() { return PKG_DESCRIPTION; }
int OnlineChartsPi::GetToolbarToolCount() { return 3; }

void OnlineChartsPi::ShowWeatherWindow(bool show) {
  if (show && !weather_window_ && weather_) {
    wxWindow* canvas = GetOCPNCanvasWindow();
    weather_window_ = new OnlineChartsWeatherWindow(
        wxGetTopLevelParent(canvas), weather_.get(),
        [this](const OnlineChartsWeatherSettings& settings) { ApplyWeather(settings); },
        [this]() {
          weather_window_shown_ = false;
          if (weather_tool_id_ >= 0) SetToolbarItemState(weather_tool_id_, false);
          if (wxFileConfig* config = GetOCPNConfigObject()) {
            config->Write("/PlugIns/OnlineCharts/WeatherWindowShown", false);
          }
        });
    // Top right of the chart, clear of OpenCPN's compass and GPS icons.
    const wxPoint origin = canvas->GetScreenPosition();
    const wxSize size = weather_window_->GetSize();
    weather_window_->Move(origin.x + canvas->GetSize().x - size.x - 16, origin.y + 64);
  }
  if (weather_window_) {
    weather_window_->Sync();
    weather_window_->Show(show);
  }
  weather_window_shown_ = show;
  if (weather_tool_id_ >= 0) SetToolbarItemState(weather_tool_id_, show);
  if (wxFileConfig* config = GetOCPNConfigObject()) {
    config->Write("/PlugIns/OnlineCharts/WeatherWindowShown", show);
  }
}

void OnlineChartsPi::ApplyWeather(const OnlineChartsWeatherSettings& settings) {
  if (!weather_) return;
  weather_settings_ = settings;
  // Keep the forecast time being viewed; it is not a saved setting.
  weather_settings_.forecast_hour = weather_->ForecastHour();
  weather_->Configure(weather_settings_);
  SaveWeatherSettings(weather_settings_);
  RequestRefresh(GetOCPNCanvasWindow());
}

bool OnlineChartsPi::MouseEventHook(wxMouseEvent& event) {
  if (!overlay_enabled_ || !weather_) return false;
  return weather_->OnMouse(event);
}

// Opens a provider web page at the boat (or the chart centre without a GPS
// fix), like the browser dashboard's "Open provider at the boat position".
void OnlineChartsPi::OpenProviderMenu() {
  double lat = 0, lon = 0, zoom = 13.76;
  bool at_boat = false;
  if (!weather_ || !weather_->ProviderPosition(&lat, &lon, &at_boat, &zoom)) {
    wxMessageBox("Move the chart once, then try again.", "Open provider", wxOK | wxICON_INFORMATION);
    return;
  }
  enum { kIBoating = wxID_HIGHEST + 1, kCMap, kGarmin, kInfo };
  wxMenu menu;
  menu.Append(kInfo, at_boat ? "At the boat position (GPS)" : "At the chart centre (no live GPS fix)");
  menu.Enable(kInfo, false);
  menu.AppendSeparator();
  menu.Append(kIBoating, "Open i-Boating");
  menu.Append(kCMap, "Open C-MAP (Pin location)");
  menu.Append(kGarmin, "Open Garmin Marine");
  wxWindow* canvas = GetOCPNCanvasWindow();
  const int choice = canvas->GetPopupMenuSelectionFromUser(menu, canvas->ScreenToClient(wxGetMousePosition()));
  wxString url;
  if (choice == kIBoating) {
    url = wxString::FromUTF8(("https://fishing-app.gpsnauticalcharts.com/i-boating-fishing-web-app/"
                              "fishing-marine-charts-navigation.html?depth=" +
                              std::string(settings_.depth_feet ? "ft" : "m") + "#" + Decimal(zoom, 2) + "/" +
                              Decimal(lat, 6) + "/" + Decimal(lon, 6)).c_str());
  } else if (choice == kCMap) {
    // C-MAP's Pin location route takes longitude first.
    url = wxString::FromUTF8(("https://appchart.c-map.com/core/select/new_place/@" + Decimal(lon, 6) + "," +
                              Decimal(lat, 6)).c_str());
  } else if (choice == kGarmin) {
    // Garmin centres its map on the geohash in `key`; only a click on the map
    // places its marker. In Chrome, the GPS Charts extension (0.7.1 or later)
    // makes that click for pages carrying gpscharts=pin. Elsewhere, click the
    // map centre.
    const std::string garmin = online_charts::GarminMarineUrl(lat, lon);
    if (garmin.empty()) return;
    url = wxString::FromUTF8((garmin + "&gpscharts=pin").c_str());
    if (OpenInChrome(url)) return;
  }
  if (!url.empty() && !wxLaunchDefaultBrowser(url)) {
    wxMessageBox("The web browser could not be opened.", "Open provider", wxOK | wxICON_WARNING);
  }
}

void OnlineChartsPi::OnToolbarToolCallback(int id) {
  if (id == provider_tool_id_) {
    OpenProviderMenu();
    return;
  }
  if (id == weather_tool_id_) {
    ShowWeatherWindow(!weather_window_shown_);
    return;
  }
  if (id != toolbar_tool_id_) return;
  overlay_enabled_ = !overlay_enabled_;
  settings_.overlay_enabled = overlay_enabled_;
  if (overlay_) overlay_->Configure(settings_);
  SetToolbarItemState(toolbar_tool_id_, overlay_enabled_);
  SaveSettings(settings_);
  wxLogMessage("Online Charts: overlay %s", overlay_enabled_ ? "On" : "Off");
  RequestRefresh(GetOCPNCanvasWindow());
}

void OnlineChartsPi::ShowPreferencesDialog(wxWindow* parent) {
  if (!overlay_) return;
  OnlineChartsSettings updated = settings_;
  OnlineChartsWeatherSettings weather_updated = weather_settings_;
  if (!ShowOnlineChartsPreferences(parent, &updated, overlay_.get(), &weather_updated,
                                   weather_.get())) {
    return;
  }
  settings_ = updated;
  weather_settings_ = weather_updated;
  if (weather_) weather_->Configure(weather_settings_);
  SaveWeatherSettings(weather_settings_);
  if (weather_window_) weather_window_->Sync();
  overlay_enabled_ = settings_.overlay_enabled;
  overlay_->Configure(settings_);
  if (toolbar_tool_id_ >= 0) SetToolbarItemState(toolbar_tool_id_, overlay_enabled_);
  SaveSettings(settings_);
  wxLogMessage(
      "Online Charts: preferences saved; overlay=%s cache_mode=%d label_scale=%d%%",
      overlay_enabled_ ? "On" : "Off", static_cast<int>(settings_.cache_mode),
      settings_.label_scale);
  RequestRefresh(GetOCPNCanvasWindow());
}

// OpenCPN calls an API 1.18 plugin both through RenderOverlay/RenderGLOverlay
// (legacy priority) and through the multi-canvas callbacks for every
// priority, so all drawing happens in the multi-canvas callbacks only;
// drawing in both would draw everything twice.
bool OnlineChartsPi::RenderOverlay(wxDC&, PlugIn_ViewPort*) { return false; }

bool OnlineChartsPi::RenderGLOverlay(wxGLContext*, PlugIn_ViewPort*) { return false; }

bool OnlineChartsPi::RenderOverlayMultiCanvas(wxDC& dc, PlugIn_ViewPort* viewport,
                                              int canvasIndex, int priority) {
  (void)canvasIndex;
  // The wxDC canvas has no chart-level pass; OpenCPN draws tide and current
  // stations after the legacy pass there, so they stay visible.
  if (priority != OVERLAY_LEGACY || !overlay_enabled_ || !overlay_) return false;
  bool drawn = overlay_->Render(dc, viewport);
  if (weather_ && viewport && viewport->bValid) {
    piDC weather_dc(dc);
    weather_dc.SetVP(viewport);
    weather_->Render(&weather_dc, viewport, 1.0);
    drawn = drawn || weather_settings_.enabled;
  }
  return drawn;
}

bool OnlineChartsPi::RenderGLOverlayMultiCanvas(wxGLContext* context,
                                                PlugIn_ViewPort* viewport,
                                                int canvasIndex, int priority) {
  (void)canvasIndex;
  if (!overlay_enabled_ || !overlay_) return false;
  // Base map, bathymetry and seamarks behave like a chart: they are drawn in
  // the chart pass, below OpenCPN's routes, waypoints and tide and current
  // stations. Hosts without that pass get them in the legacy pass instead.
  if (priority == kOverlayCharts) {
    charts_pass_seen_ = true;
    return overlay_->Render(context, viewport);
  }
  if (priority != OVERLAY_LEGACY) return false;
  bool drawn = charts_pass_seen_ ? false : overlay_->Render(context, viewport);
  if (weather_ && viewport && viewport->bValid) {
    weather_->Render(nullptr, viewport, online_charts::GlPixelScale());
    drawn = drawn || weather_settings_.enabled;
  }
  return drawn;
}

void OnlineChartsPi::SetPositionFix(PlugIn_Position_Fix& fix) {
  if (!std::isfinite(fix.Lat) || !std::isfinite(fix.Lon) ||
      fix.Lat < -90.0 || fix.Lat > 90.0 || fix.Lon < -180.0 || fix.Lon > 180.0) {
    return;
  }
  // OpenCPN keeps reporting the last own-ship position, with a current time
  // and a course of 0, when no GPS is connected; the speed is then unknown
  // (NaN). Only a live fix (speed or satellites) places the weather at the boat.
  const std::time_t now = std::time(nullptr);
  const bool live = std::isfinite(fix.Sog) || fix.nSats > 0;
  if (weather_ && live && fix.FixTime > 0 &&
      std::llabs(static_cast<long long>(now - fix.FixTime)) < 600) {
    weather_->SetBoat(fix.Lat, fix.Lon);
  }
  if (!server_) return;

  std::string out = "{";
  bool first = true;
  JsonNumber(&out, "lat", fix.Lat, 7, &first);
  JsonNumber(&out, "lon", fix.Lon, 7, &first);
  JsonNumber(&out, "sog", fix.Sog, 3, &first);
  JsonNumber(&out, "cog", fix.Cog, 2, &first);
  JsonNumber(&out, "variation", fix.Var, 2, &first);
  const std::string time = IsoTime(fix.FixTime);
  if (!time.empty()) out += ",\"time\":\"" + time + '\"';
  if (fix.nSats >= 0) out += ",\"satellites\":" + std::to_string(fix.nSats);
  out += ",\"source\":\"OpenCPN\",\"quality\":\"GPS\"}";
  server_->Broadcast(out);
}
