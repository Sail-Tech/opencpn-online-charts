#ifndef ONLINE_CHARTS_WEATHER_H_
#define ONLINE_CHARTS_WEATHER_H_

#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/string.h>
#include <wx/timer.h>

#include "online_charts_render.h"

class OnlineChartsHttp;
class PlugIn_ViewPort;
class piDC;
class wxDC;
class wxMouseEvent;

// Marine forecast from Open-Meteo (no API key), matching the browser
// dashboard: wind, gusts, air temperature, pressure and waves at the boat (or
// the chart centre without a GPS fix), a wind barb at the boat, an optional
// wind and wave field across the chart, a choice of model and units, and a
// forecast hour up to 72 hours ahead.
struct OnlineChartsWeatherSettings {
  bool enabled = true;
  bool show_panel = false;    // Conditions drawn on the chart; the Weather window shows them too.
  bool show_timeline = true;  // Forecast time bar at the bottom of the chart.
  bool wind_barb = true;
  bool wind_field = true;
  bool wave_field = true;
  bool current_field = true;  // Red ocean current arrows across the chart and at the boat.
  int model = 0;  // Index into the model list, 0 = best match.
  bool temp_fahrenheit = false;
  bool wave_feet = false;
  int forecast_hour = 0;    // Hours ahead of now, 0-72.
  int fresh_minutes = 60;   // Cached readings younger than this are reused without a request.
  int keep_hours = 72;      // Cached readings are kept up to this age as an offline fallback.
};

// The conditions at the forecast time, formatted for the Weather window and
// the chart panel. Empty values are shown as a dash.
struct OnlineChartsConditions {
  wxString place;   // "At the boat" or "At the chart centre".
  wxString wind;
  wxString gusts;
  wxString air_temp;
  wxString pressure;
  wxString waves;
  wxString current;
  wxString when;    // Forecast time and model.
  wxString status;  // Source and age of the reading, or why there is none.
};

class OnlineChartsWeather : public wxEvtHandler {
 public:
  static int ModelCount();
  static const char* ModelName(int index);

  OnlineChartsWeather();
  ~OnlineChartsWeather() override;

  void Configure(const OnlineChartsWeatherSettings& settings);
  const OnlineChartsWeatherSettings& Settings() const { return settings_; }
  void SetForecastHour(int hours);
  int ForecastHour() const { return settings_.forecast_hour; }
  void SetPlaying(bool playing);
  bool Playing() const { return play_timer_.IsRunning(); }
  OnlineChartsConditions Current() const;
  // Chart mouse events for the forecast time bar and the field read-out.
  // Returns true when the event was used and must not reach the chart.
  bool OnMouse(const wxMouseEvent& event);
  // Position for the provider pages: the boat with a live GPS fix, otherwise
  // the chart centre. Returns false before the first chart render.
  bool ProviderPosition(double* lat, double* lon, bool* at_boat, double* zoom) const;
  void SetBoat(double lat, double lon);
  void Shutdown();

  // dc is null for the OpenGL canvas (context current); pixel_scale is canvas
  // pixels per logical pixel.
  void Render(piDC* dc, PlugIn_ViewPort* viewport, double pixel_scale);

  void RefreshNow();
  wxString ClearCache();
  wxString StatusText() const;

  struct Hour {
    double wind_speed = NAN;  // knots
    double wind_dir = NAN;    // degrees, direction the wind comes from
    double wind_gust = NAN;   // knots
    double air_temp = NAN;    // Celsius
    double pressure = NAN;    // hPa
    double wave_height = NAN; // metres
    double wave_dir = NAN;    // degrees, direction the waves come from
    double wave_period = NAN; // seconds
    double current_speed = NAN;  // knots
    double current_dir = NAN;    // degrees, direction the current flows towards
  };
  struct Series {
    std::vector<std::time_t> times;
    std::vector<Hour> hours;
  };
  struct FieldPoint {
    double lat = 0;
    double lon = 0;
    Series series;
  };

 private:
  struct Reading {
    std::string key;
    std::time_t fetched = 0;  // When the data was downloaded.
    bool complete = false;    // Both the forecast and the marine answer arrived.
    bool met = false;         // Wind and air from MET Norway (Open-Meteo refused).
    std::vector<FieldPoint> points;
  };
  // A point of the wind and wave field. Points sit on a fixed latitude and
  // longitude lattice and are cached one by one, so moving the chart only
  // requests the points not yet known: Open-Meteo counts every location of a
  // request against its daily limit.
  struct Node {
    double lat = 0;
    double lon = 0;
    Series series;
    std::time_t wind_time = 0;  // When the wind was downloaded (0 = not yet).
    std::time_t wave_time = 0;  // When the waves were downloaded (0 = not yet).
    bool wind_met = false;      // The wind came from MET Norway.
    bool has_current = false;   // The marine answer included ocean currents.
    double elevation = NAN;     // Terrain height (m) from the wind answer; above 2 m is land.
  };
  struct Job;

  void OnTimer(wxTimerEvent& event);
  void OnPlayTimer(wxTimerEvent& event);
  struct TimelineLayout {
    bool valid = false;
    double x = 0, y = 0, w = 0, h = 0;          // Bar, in logical pixels.
    double play_x = 0, play_y = 0, play_r = 0;  // Play button.
    double track_x0 = 0, track_x1 = 0, track_y = 0;
    double now_x = 0, now_y = 0, now_w = 0, now_h = 0;
    double label_x = 0;
  };
  TimelineLayout Layout() const;
  int HourAtX(double x) const;
  void DrawTimeline(piDC* dc);
  void DrawReadout(piDC* dc);
  void FillRoundRect(piDC* dc, double x, double y, double w, double h, double r, const wxColour& colour);
  void FillCircle(piDC* dc, double x, double y, double r, const wxColour& colour);
  void Text(piDC* dc, const wxString& text, double x, double y, double height, const wxColour& colour,
            bool bold = false);
  double TextWidth(piDC* dc, const wxString& text, double height, bool bold = false);
  void Schedule();
  void Update(bool force);
  void Fetch(const std::string& key, const std::vector<std::pair<double, double>>& points,
             bool field, bool force);
  void Finish(const std::shared_ptr<Job>& job);
  bool LoadCached(const std::string& key, Reading* reading) const;
  void SaveCached(const std::string& key, std::time_t fetched,
                  const std::vector<std::pair<double, double>>& points,
                  const std::string& forecast, const std::string& marine);
  void PruneCache();
  wxString CacheFile(const std::string& key) const;
  std::string PointKey(double lat, double lon) const;
  std::string NodeKey(double lat, double lon) const;
  void UpdateField(std::time_t now, std::time_t fresh);
  void FetchNodes(const std::vector<std::string>& keys, bool marine);
  bool LoadNode(const std::string& key, Node* node) const;
  void SaveNode(const std::string& key, const Node& node);
  void NoteFailure(bool marine, long status, const std::string& body);
  void FetchMetNodes(const std::vector<std::string>& keys);
  void PumpMet();
  std::time_t TargetTime() const;
  const Hour* HourAt(const Series& series) const;
  bool BoatKnown() const;

  void DrawPanel(piDC* dc, PlugIn_ViewPort* viewport);
  void DrawBarb(piDC* dc, PlugIn_ViewPort* viewport, double lat, double lon, double speed,
                double dir, double size, const wxColour& colour);
  void DrawCurrentArrow(piDC* dc, PlugIn_ViewPort* viewport, double lat, double lon,
                        double speed, double dir, double size, double offset_y);
  void DrawWaveArrow(piDC* dc, PlugIn_ViewPort* viewport, double lat, double lon,
                     double height, double dir, double size, double offset_x);
  void DrawSegments(piDC* dc, const std::vector<float>& lines, const std::vector<float>& triangles,
                    const wxColour& colour, double width);

  OnlineChartsWeatherSettings settings_;
  std::unique_ptr<OnlineChartsHttp> http_;
  wxTimer timer_;
  wxTimer play_timer_;
  wxString cache_dir_;
  bool shutdown_ = false;

  double boat_lat_ = NAN;
  double boat_lon_ = NAN;
  std::time_t boat_time_ = 0;
  double centre_lat_ = NAN;
  double centre_lon_ = NAN;
  double view_south_ = NAN, view_north_ = NAN, view_west_ = NAN, view_east_ = NAN;
  bool view_valid_ = false;
  std::time_t view_changed_ = 0;

  Reading point_;
  std::map<std::string, Node> nodes_;
  std::vector<std::string> visible_nodes_;  // Lattice points of the current chart view.
  double node_step_ = 0;                    // Lattice spacing in degrees.
  std::set<std::string> node_requests_;     // Points with a request in flight.
  std::time_t forecast_blocked_until_ = 0;  // Back-off after errors or Open-Meteo limits.
  std::time_t marine_blocked_until_ = 0;
  int refusals_ = 0;  // Consecutive HTTP 429 answers.
  // MET Norway fallback for the wind when Open-Meteo refuses: one request per
  // point, a few at a time as MET Norway's terms ask.
  std::time_t met_blocked_until_ = 0;
  std::vector<std::string> met_queue_;
  int met_active_ = 0;
  double view_zoom_ = 13.76;  // Map zoom (512-pixel tiles) of the last render.
  std::vector<std::string> fetching_;
  std::time_t last_failure_ = 0;
  bool offline_ = false;
  wxString last_error_;

  online_charts::GlTextAtlas text_;
  online_charts::GlTextAtlas text_bold_;
  double canvas_w_ = 0;  // Logical size of the chart canvas at the last render.
  double canvas_h_ = 0;
  bool dragging_ = false;
  // Field points drawn in the last frame, in logical pixels, for the read-out.
  struct Mark {
    double x = 0, y = 0;
    size_t point = 0;
  };
  std::vector<Mark> marks_;
  int hover_ = -1;
  double hover_x_ = 0, hover_y_ = 0;
  double pixel_scale_ = 1.0;
  bool gl_pass_ = false;
};

#endif  // ONLINE_CHARTS_WEATHER_H_
