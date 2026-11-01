#include "online_charts_weather.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

#include <wx/datetime.h>
#include <wx/dir.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/arrstr.h>
#include <wx/textfile.h>
#include <wx/utils.h>
#include <wx/window.h>

#include "ocpn_plugin.h"
#include "online_charts_http.h"
#include "online_charts_json.h"
#include "pidc.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxCacheFiles = 3000;
constexpr size_t kMaxFieldPoints = 40;
constexpr size_t kMaxLocationsPerRequest = 50;
constexpr std::time_t kRetryAfterFailure = 5 * 60;
constexpr std::time_t kBoatFixLifetime = 10 * 60;
constexpr std::time_t kSettleSeconds = 2;  // Wait for the chart to stop moving before fetching.
constexpr int kForecastDays = 4;           // Covers the 72-hour window from any time of day.
constexpr int kUpdateTimer = 1;
// Readings fetched before this time (1.4.0 adding currents) lack them.
constexpr std::time_t kCurrentsSince = 1793476000;  // 2026-10-31 19:46 UTC
constexpr int kPlayTimer = 2;

struct Model {
  const char* id;
  const char* name;
};
// Same choices as the browser dashboard.
const Model kModels[] = {
    {"best_match", "Best match (automatic)"},
    {"ecmwf_ifs025", "ECMWF IFS"},
    {"gfs_seamless", "GFS (NOAA)"},
    {"icon_seamless", "ICON (DWD)"},
    {"meteofrance_seamless", "M\u00e9t\u00e9o-France"},
    {"ukmo_seamless", "UK Met Office"},
};
constexpr int kModelCount = sizeof(kModels) / sizeof(kModels[0]);

// Functions rather than global wxColour objects: a global with a destructor
// registers an exit handler that can run after OpenCPN unloads the plugin.
wxColour BarbColour() { return wxColour(7, 59, 85); }
wxColour Outline() { return wxColour(255, 255, 255, 230); }

// Fixed-point text with '.' decimals whatever the process locale.
std::string Fixed(double value, int precision) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", precision, value);
  std::string result(text);
  for (char& c : result) {
    if (c == ',') c = '.';
  }
  return result;
}

double Round1(double value) { return std::round(value * 10.0) / 10.0; }

const char* Compass(double degrees) {
  static const char* kPoints[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  const double d = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
  return kPoints[static_cast<int>(std::lround(d / 22.5)) % 16];
}

wxString Age(std::time_t seconds) {
  const long minutes = static_cast<long>(seconds / 60);
  if (minutes < 1) return "just now";
  if (minutes < 60) return wxString::Format("%ld min ago", minutes);
  const long hours = std::lround(minutes / 60.0);
  if (hours < 48) return wxString::Format("%ld h ago", hours);
  return wxString::Format("%ld d ago", std::lround(hours / 24.0));
}

// Colour of a wave arrow by significant wave height, as in the dashboard.
wxColour WaveColour(double height) {
  if (height < 0.5) return wxColour(86, 180, 233);
  if (height < 1.25) return wxColour(31, 143, 195);
  if (height < 2.5) return wxColour(240, 162, 2);
  if (height < 4.0) return wxColour(232, 89, 12);
  return wxColour(193, 18, 31);
}

wxString PointList(const std::vector<std::pair<double, double>>& points, bool latitude) {
  std::string list;
  for (const auto& point : points) {
    if (!list.empty()) list += ',';
    list += Fixed(latitude ? point.first : point.second, 4);
  }
  return wxString::FromUTF8(list.c_str());
}

double NumberAt(const online_charts::JsonValue* array, size_t index) {
  if (!array || !array->IsArray() || index >= array->items.size()) return NAN;
  const auto& item = array->items[index];
  return item.IsNumber() ? item.number : NAN;
}

// Open-Meteo answers a multi-location request with an array and a single
// location with an object.
std::vector<const online_charts::JsonValue*> Locations(const online_charts::JsonValue& root,
                                                      size_t count) {
  std::vector<const online_charts::JsonValue*> list(count, nullptr);
  if (root.IsArray()) {
    for (size_t i = 0; i < count && i < root.items.size(); ++i) list[i] = &root.items[i];
  } else if (root.IsObject() && count > 0) {
    list[0] = &root;
  }
  return list;
}

// Builds each point's hourly series from the forecast and marine responses
// (either may be missing: the marine model has no data inland). Returns true
// when at least one value was found.
// "2026-10-31T19:00:00Z" as UTC seconds, without the C locale or time zone.
std::time_t ParseUtc(const std::string& text) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 5) {
    return 0;
  }
  // Days from the civil date (Howard Hinnant's algorithm), avoiding timegm.
  year -= month <= 2;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const long yoe = year - era * 400;
  const long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097 + doe - 719468;
  return static_cast<std::time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

// Merges a MET Norway Locationforecast answer (wind in m/s, from-direction,
// air temperature and sea-level pressure) into a series. MET Norway has no
// gusts in its compact answer. Returns true when a wind value was found.
bool MergeMet(const std::string& body, OnlineChartsWeather::Series* series, bool wind_only,
              double* elevation = nullptr) {
  online_charts::JsonValue root;
  if (body.empty() || !online_charts::ParseJson(body, &root)) return false;
  // geometry.coordinates is [lon, lat, terrain altitude in metres].
  const online_charts::JsonValue* geometry = root.Get("geometry");
  const online_charts::JsonValue* coordinates = geometry ? geometry->Get("coordinates") : nullptr;
  if (elevation && coordinates && coordinates->IsArray() && coordinates->items.size() > 2 &&
      coordinates->items[2].IsNumber()) {
    *elevation = coordinates->items[2].number;
  }
  const online_charts::JsonValue* properties = root.Get("properties");
  const online_charts::JsonValue* timeseries = properties ? properties->Get("timeseries") : nullptr;
  if (!timeseries || !timeseries->IsArray()) return false;
  std::map<std::time_t, size_t> index;
  for (size_t i = 0; i < series->times.size(); ++i) index[series->times[i]] = i;
  bool any = false;
  for (const auto& step : timeseries->items) {
    const online_charts::JsonValue* time = step.Get("time");
    const online_charts::JsonValue* data = step.Get("data");
    const online_charts::JsonValue* instant = data ? data->Get("instant") : nullptr;
    const online_charts::JsonValue* details = instant ? instant->Get("details") : nullptr;
    if (!time || !details) continue;
    const std::time_t t = ParseUtc(time->text);
    if (t <= 0) continue;
    auto slot = index.find(t);
    if (slot == index.end()) {
      slot = index.emplace(t, series->times.size()).first;
      series->times.push_back(t);
      series->hours.emplace_back();
    }
    OnlineChartsWeather::Hour& hour = series->hours[slot->second];
    auto value = [&](const char* name) {
      const online_charts::JsonValue* v = details->Get(name);
      return v && v->IsNumber() ? v->number : NAN;
    };
    const double speed = value("wind_speed");
    hour.wind_speed = std::isfinite(speed) ? speed * 1.943844 : NAN;
    hour.wind_dir = value("wind_from_direction");
    hour.wind_gust = NAN;
    if (!wind_only) {
      hour.air_temp = value("air_temperature");
      hour.pressure = value("air_pressure_at_sea_level");
    }
    any = any || std::isfinite(hour.wind_speed);
  }
  // Keep the hours in time order.
  std::vector<size_t> order(series->times.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return series->times[a] < series->times[b]; });
  OnlineChartsWeather::Series sorted;
  for (size_t i : order) {
    sorted.times.push_back(series->times[i]);
    sorted.hours.push_back(series->hours[i]);
  }
  *series = std::move(sorted);
  return any;
}

wxString MetUrl(double lat, double lon) {
  return wxString::FromUTF8(("https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=" +
                             Fixed(lat, 4) + "&lon=" + Fixed(lon, 4)).c_str());
}

bool ParseSeries(const std::string& forecast, const std::string& marine,
                 std::vector<OnlineChartsWeather::FieldPoint>* points, bool met = false) {
  online_charts::JsonValue forecast_json;
  online_charts::JsonValue marine_json;
  const bool have_forecast = !met && !forecast.empty() && online_charts::ParseJson(forecast, &forecast_json);
  const bool have_marine = !marine.empty() && online_charts::ParseJson(marine, &marine_json);
  const auto forecast_list = Locations(forecast_json, points->size());
  const auto marine_list = Locations(marine_json, points->size());
  bool any = false;
  for (size_t p = 0; p < points->size(); ++p) {
    auto& series = (*points)[p].series;
    series = OnlineChartsWeather::Series();
    std::map<std::time_t, size_t> index;
    auto slot = [&](std::time_t time) -> OnlineChartsWeather::Hour& {
      auto found = index.find(time);
      if (found != index.end()) return series.hours[found->second];
      index[time] = series.times.size();
      series.times.push_back(time);
      series.hours.emplace_back();
      return series.hours.back();
    };
    const online_charts::JsonValue* hourly =
        have_forecast && !met && forecast_list[p] ? forecast_list[p]->Get("hourly") : nullptr;
    const online_charts::JsonValue* times = hourly ? hourly->Get("time") : nullptr;
    if (times && times->IsArray()) {
      for (size_t i = 0; i < times->items.size(); ++i) {
        if (!times->items[i].IsNumber()) continue;
        auto& hour = slot(static_cast<std::time_t>(times->items[i].number));
        hour.wind_speed = NumberAt(hourly->Get("wind_speed_10m"), i);
        hour.wind_dir = NumberAt(hourly->Get("wind_direction_10m"), i);
        hour.wind_gust = NumberAt(hourly->Get("wind_gusts_10m"), i);
        hour.air_temp = NumberAt(hourly->Get("temperature_2m"), i);
        hour.pressure = NumberAt(hourly->Get("surface_pressure"), i);
        any = any || std::isfinite(hour.wind_speed) || std::isfinite(hour.air_temp);
      }
    }
    hourly = have_marine && marine_list[p] ? marine_list[p]->Get("hourly") : nullptr;
    times = hourly ? hourly->Get("time") : nullptr;
    if (times && times->IsArray()) {
      for (size_t i = 0; i < times->items.size(); ++i) {
        if (!times->items[i].IsNumber()) continue;
        auto& hour = slot(static_cast<std::time_t>(times->items[i].number));
        hour.wave_height = NumberAt(hourly->Get("wave_height"), i);
        hour.wave_dir = NumberAt(hourly->Get("wave_direction"), i);
        hour.wave_period = NumberAt(hourly->Get("wave_period"), i);
        hour.current_speed = NumberAt(hourly->Get("ocean_current_velocity"), i) / 1.852;  // km/h to kn
        hour.current_dir = NumberAt(hourly->Get("ocean_current_direction"), i);
        any = any || std::isfinite(hour.wave_height);
      }
    }
    // Keep the hours in time order whatever order the responses used.
    std::vector<size_t> order(series.times.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return series.times[a] < series.times[b]; });
    OnlineChartsWeather::Series sorted;
    for (size_t i : order) {
      sorted.times.push_back(series.times[i]);
      sorted.hours.push_back(series.hours[i]);
    }
    series = std::move(sorted);
  }
  if (met && !points->empty()) {
    any = MergeMet(forecast, &(*points)[0].series, false) || any;
  }
  return any;
}

// Screen angle (radians, clockwise from up) of true north at a position, so
// symbols stay correct on a rotated chart.
double NorthAngle(PlugIn_ViewPort* viewport, double lat, double lon, wxPoint2DDouble* at) {
  wxPoint2DDouble north;
  GetDoubleCanvasPixLL(viewport, at, lat, lon);
  GetDoubleCanvasPixLL(viewport, &north, std::min(89.9, lat + 0.01), lon);
  const double dx = north.m_x - at->m_x;
  const double dy = north.m_y - at->m_y;
  return (dx == 0 && dy == 0) ? 0.0 : std::atan2(dx, -dy);
}

wxFont PanelFont(int pixels) {
  wxFont* configured = GetOCPNScaledFont_PlugIn("Dialog", 10);
  wxFont font = configured && configured->IsOk()
                    ? wxFont(*configured)
                    : wxFont(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
  if (pixels > 0) font.SetPixelSize(wxSize(0, pixels));
  return font;
}

}  // namespace

struct OnlineChartsWeather::Job {
  std::string key;
  std::vector<std::pair<double, double>> points;
  bool field = false;
  bool met = false;  // The forecast part comes from MET Norway.
  int pending = 0;
  std::string forecast;
  std::string marine;
};

int OnlineChartsWeather::ModelCount() { return kModelCount; }

const char* OnlineChartsWeather::ModelName(int index) {
  return kModels[std::max(0, std::min(kModelCount - 1, index))].name;
}

OnlineChartsWeather::OnlineChartsWeather()
    : http_(std::make_unique<OnlineChartsHttp>(
          "OnlineCharts-OpenCPN/1.5.3 (+https://github.com/Sail-Tech/opencpn-online-charts)")) {
  wxFileName dir(wxStandardPaths::Get().GetUserDataDir(), "");
  dir.AppendDir("online_charts_pi");
  dir.AppendDir("weather_cache");
  cache_dir_ = dir.GetFullPath();
  if (!wxFileName::Mkdir(cache_dir_, 0770, wxPATH_MKDIR_FULL) && !wxDirExists(cache_dir_)) {
    last_error_ = "Weather cache directory is not writable";
  }
  PruneCache();
  timer_.SetOwner(this, kUpdateTimer);
  play_timer_.SetOwner(this, kPlayTimer);
  Bind(wxEVT_TIMER, &OnlineChartsWeather::OnTimer, this, kUpdateTimer);
  Bind(wxEVT_TIMER, &OnlineChartsWeather::OnPlayTimer, this, kPlayTimer);
  timer_.Start(1000);
}

OnlineChartsWeather::~OnlineChartsWeather() {
  Shutdown();
  Unbind(wxEVT_TIMER, &OnlineChartsWeather::OnTimer, this, kUpdateTimer);
  Unbind(wxEVT_TIMER, &OnlineChartsWeather::OnPlayTimer, this, kPlayTimer);
}

void OnlineChartsWeather::Shutdown() {
  if (shutdown_) return;
  shutdown_ = true;
  timer_.Stop();
  play_timer_.Stop();
  if (http_) http_->CancelAll();
  fetching_.clear();
}

void OnlineChartsWeather::Configure(const OnlineChartsWeatherSettings& input) {
  const bool model_changed = input.model != settings_.model;
  settings_ = input;
  settings_.model = std::max(0, std::min(kModelCount - 1, settings_.model));
  settings_.forecast_hour = std::max(0, std::min(72, settings_.forecast_hour));
  settings_.fresh_minutes = std::max(10, std::min(360, settings_.fresh_minutes));
  settings_.keep_hours = std::max(1, std::min(72, settings_.keep_hours));
  if (model_changed) {
    point_ = Reading();
    nodes_.clear();
    visible_nodes_.clear();
  }
  if (!settings_.enabled && http_) {
    http_->CancelAll();
    fetching_.clear();
  }
  last_failure_ = 0;
  PruneCache();
}

void OnlineChartsWeather::SetForecastHour(int hours) {
  settings_.forecast_hour = std::max(0, std::min(72, hours));
}

void OnlineChartsWeather::SetBoat(double lat, double lon) {
  if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) {
    return;
  }
  boat_lat_ = lat;
  boat_lon_ = lon;
  boat_time_ = std::time(nullptr);
}

bool OnlineChartsWeather::BoatKnown() const {
  return boat_time_ > 0 && std::time(nullptr) - boat_time_ < kBoatFixLifetime;
}

void OnlineChartsWeather::OnTimer(wxTimerEvent&) { Update(false); }

void OnlineChartsWeather::RefreshNow() {
  last_failure_ = 0;
  Update(true);
}

std::string OnlineChartsWeather::PointKey(double lat, double lon) const {
  // Rounded to 0.1 degree (~11 km, about the models' grid) so nearby
  // positions share an entry, like the dashboard's cache.
  return std::string("p_") + kModels[settings_.model].id + '_' + Fixed(Round1(lat), 1) + '_' +
         Fixed(Round1(lon), 1);
}

std::string OnlineChartsWeather::NodeKey(double lat, double lon) const {
  return std::string("n_") + kModels[settings_.model].id + '_' + Fixed(lat, 3) + '_' + Fixed(lon, 3);
}

wxString OnlineChartsWeather::CacheFile(const std::string& key) const {
  wxString name = wxString::FromUTF8(key.c_str());
  name.Replace("-", "m");
  return cache_dir_ + wxFileName::GetPathSeparator() + name + ".txt";
}

void OnlineChartsWeather::Update(bool force) {
  if (shutdown_ || !settings_.enabled || !http_) return;
  const std::time_t now = std::time(nullptr);
  const std::time_t fresh = static_cast<std::time_t>(settings_.fresh_minutes) * 60;
  const bool may_retry = (force || last_failure_ == 0 || now - last_failure_ >= kRetryAfterFailure) &&
                         (now >= forecast_blocked_until_ || now >= met_blocked_until_);
  const bool settled = force || now - view_changed_ >= kSettleSeconds;

  // Conditions at the boat, or at the chart centre without a recent fix.
  double lat = NAN;
  double lon = NAN;
  if (BoatKnown()) {
    lat = boat_lat_;
    lon = boat_lon_;
  } else if (view_valid_ && settled) {
    lat = centre_lat_;
    lon = centre_lon_;
  }
  if (std::isfinite(lat) && std::isfinite(lon)) {
    const std::string key = PointKey(lat, lon);
    if (point_.key != key) {
      Reading cached;
      if (LoadCached(key, &cached)) {
        point_ = std::move(cached);
        RequestRefresh(GetOCPNCanvasWindow());
      }
    }
    bool has_current = false;
    for (const auto& hour : point_.points.empty() ? std::vector<Hour>() : point_.points.front().series.hours) {
      has_current = has_current || std::isfinite(hour.current_speed);
    }
    // Readings cached before currents were added are refreshed once (inland
    // points have none and keep their reading).
    const bool stale = point_.key != key || !point_.complete || now - point_.fetched >= fresh ||
                       (settings_.current_field && !has_current && point_.fetched < kCurrentsSince);
    if (stale && may_retry) Fetch(key, {{Round1(lat), Round1(lon)}}, false, force);
  }

  if ((settings_.wind_field || settings_.wave_field) && view_valid_ && settled) {
    UpdateField(now, force ? 0 : fresh);
  }
}

void OnlineChartsWeather::UpdateField(std::time_t now, std::time_t fresh) {
  // The finest lattice that keeps the view to at most kMaxFieldPoints points.
  static const double kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5, 10};
  int lat0 = 0, lat1 = -1, lon0 = 0, lon1 = -1;
  double step = kSteps[0];
  for (double candidate : kSteps) {
    step = candidate;
    lat0 = static_cast<int>(std::ceil(view_south_ / step));
    lat1 = static_cast<int>(std::floor(view_north_ / step));
    lon0 = static_cast<int>(std::ceil(view_west_ / step));
    lon1 = static_cast<int>(std::floor(view_east_ / step));
    const long count = static_cast<long>(std::max(0, lat1 - lat0 + 1)) * std::max(0, lon1 - lon0 + 1);
    if (count <= static_cast<long>(kMaxFieldPoints)) break;
  }
  node_step_ = step;
  visible_nodes_.clear();
  for (int i = lat0; i <= lat1; ++i) {
    for (int j = lon0; j <= lon1; ++j) {
      const double lat = i * step;
      double lon = j * step;
      if (lon > 180) lon -= 360;
      if (lon < -180) lon += 360;
      const std::string key = NodeKey(lat, lon);
      if (!nodes_.count(key)) {
        Node node;
        node.lat = lat;
        node.lon = lon;
        LoadNode(key, &node);
        nodes_[key] = std::move(node);
      }
      visible_nodes_.push_back(key);
    }
  }
  if (nodes_.size() > 3000) {
    const std::set<std::string> keep(visible_nodes_.begin(), visible_nodes_.end());
    for (auto it = nodes_.begin(); it != nodes_.end();) {
      it = keep.count(it->first) ? std::next(it) : nodes_.erase(it);
    }
  }
  std::vector<std::string> need_wind;
  std::vector<std::string> need_wave;
  for (const auto& key : visible_nodes_) {
    const Node& node = nodes_[key];
    if (settings_.wind_field && (now - node.wind_time >= fresh || !std::isfinite(node.elevation)) && !node_requests_.count("w:" + key)) {
      need_wind.push_back(key);
    }
    const bool marine_wanted = settings_.wave_field || settings_.current_field;
    const bool marine_stale = now - node.wave_time >= fresh || (settings_.current_field && !node.has_current);
    if (marine_wanted && marine_stale && !node_requests_.count("m:" + key)) {
      need_wave.push_back(key);
    }
  }
  if (!need_wind.empty()) {
    if (now >= forecast_blocked_until_) {
      FetchNodes(need_wind, false);
    } else if (now >= met_blocked_until_) {
      FetchMetNodes(need_wind);
    }
  }
  if (!need_wave.empty() && now >= marine_blocked_until_) FetchNodes(need_wave, true);
  RequestRefresh(GetOCPNCanvasWindow());
}

void OnlineChartsWeather::NoteFailure(bool marine, long status, const std::string& body) {
  const std::time_t now = std::time(nullptr);
  std::time_t& until = marine ? marine_blocked_until_ : forecast_blocked_until_;
  if (status == 429) {
    // Open-Meteo's free service limits requests per minute, hour and day.
    const bool daily = body.find("Daily") != std::string::npos || body.find("daily") != std::string::npos;
    // Each refusal in a row doubles the wait, up to an hour.
    const std::time_t wait = std::min<std::time_t>(3600, 60L << std::min(6, refusals_++));
    until = now + (daily ? 3600 : wait);
    last_error_ = daily ? "Open-Meteo daily limit reached; cached weather shown"
                        : "Open-Meteo busy; retrying in a minute";
    return;
  }
  until = now + kRetryAfterFailure;
  offline_ = status == 0;
  last_error_ = status > 0 ? wxString::Format("Open-Meteo returned HTTP %ld", status)
                           : wxString("Open-Meteo could not be reached");
}

void OnlineChartsWeather::FetchNodes(const std::vector<std::string>& keys, bool marine) {
  const wxString days = wxString::Format("%d", kForecastDays);
  for (size_t start = 0; start < keys.size(); start += kMaxLocationsPerRequest) {
    const std::vector<std::string> batch(
        keys.begin() + start, keys.begin() + std::min(keys.size(), start + kMaxLocationsPerRequest));
    std::vector<std::pair<double, double>> points;
    for (const auto& key : batch) points.emplace_back(nodes_[key].lat, nodes_[key].lon);
    const wxString lats = PointList(points, true);
    const wxString lons = PointList(points, false);
    wxString url;
    if (marine) {
      url = "https://marine-api.open-meteo.com/v1/marine?latitude=" + lats + "&longitude=" + lons +
            "&hourly=wave_height,wave_direction,ocean_current_velocity,ocean_current_direction"
            "&timeformat=unixtime&forecast_days=" + days;
    } else {
      url = "https://api.open-meteo.com/v1/forecast?latitude=" + lats + "&longitude=" + lons +
            "&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m&wind_speed_unit=kn"
            "&timeformat=unixtime&forecast_days=" + days;
      if (settings_.model > 0) url += wxString("&models=") + kModels[settings_.model].id;
    }
    const std::string tag = marine ? "m:" : "w:";
    for (const auto& key : batch) node_requests_.insert(tag + key);
    const bool started = http_->Get(url, [this, batch, marine, tag](bool ok, long status, std::string body) {
      for (const auto& key : batch) node_requests_.erase(tag + key);
      if (shutdown_) return;
      online_charts::JsonValue root;
      if (!ok || !online_charts::ParseJson(body, &root)) {
        NoteFailure(marine, ok ? 0 : status, body);
        RequestRefresh(GetOCPNCanvasWindow());
        return;
      }
      const auto locations = Locations(root, batch.size());
      const std::time_t now = std::time(nullptr);
      for (size_t i = 0; i < batch.size(); ++i) {
        auto found = nodes_.find(batch[i]);
        if (found == nodes_.end() || !locations[i]) continue;
        Node& node = found->second;
        const online_charts::JsonValue* hourly = locations[i]->Get("hourly");
        const online_charts::JsonValue* elevation = locations[i]->Get("elevation");
        if (!marine && elevation && elevation->IsNumber()) node.elevation = elevation->number;
        const online_charts::JsonValue* times = hourly ? hourly->Get("time") : nullptr;
        if (!times || !times->IsArray()) continue;
        std::map<std::time_t, size_t> index;
        for (size_t h = 0; h < node.series.times.size(); ++h) index[node.series.times[h]] = h;
        for (size_t h = 0; h < times->items.size(); ++h) {
          if (!times->items[h].IsNumber()) continue;
          const std::time_t time = static_cast<std::time_t>(times->items[h].number);
          auto slot = index.find(time);
          if (slot == index.end()) {
            slot = index.emplace(time, node.series.times.size()).first;
            node.series.times.push_back(time);
            node.series.hours.emplace_back();
          }
          Hour& hour = node.series.hours[slot->second];
          if (marine) {
            hour.wave_height = NumberAt(hourly->Get("wave_height"), h);
            hour.wave_dir = NumberAt(hourly->Get("wave_direction"), h);
            hour.current_speed = NumberAt(hourly->Get("ocean_current_velocity"), h) / 1.852;
            hour.current_dir = NumberAt(hourly->Get("ocean_current_direction"), h);
          } else {
            hour.wind_speed = NumberAt(hourly->Get("wind_speed_10m"), h);
            hour.wind_dir = NumberAt(hourly->Get("wind_direction_10m"), h);
            hour.wind_gust = NumberAt(hourly->Get("wind_gusts_10m"), h);
          }
        }
        // Keep the hours in time order after merging.
        std::vector<size_t> order(node.series.times.size());
        for (size_t h = 0; h < order.size(); ++h) order[h] = h;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return node.series.times[a] < node.series.times[b]; });
        Series sorted;
        for (size_t h : order) {
          sorted.times.push_back(node.series.times[h]);
          sorted.hours.push_back(node.series.hours[h]);
        }
        node.series = std::move(sorted);
        (marine ? node.wave_time : node.wind_time) = now;
        if (marine) node.has_current = true;
        if (!marine) node.wind_met = false;
        SaveNode(batch[i], node);
      }
      (marine ? marine_blocked_until_ : forecast_blocked_until_) = 0;
      refusals_ = 0;
      offline_ = false;
      last_error_.clear();
      RequestRefresh(GetOCPNCanvasWindow());
    });
    if (!started) {
      for (const auto& key : batch) node_requests_.erase(tag + key);
      NoteFailure(marine, 0, std::string());
    }
  }
}

void OnlineChartsWeather::FetchMetNodes(const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    if (std::find(met_queue_.begin(), met_queue_.end(), key) == met_queue_.end()) met_queue_.push_back(key);
  }
  PumpMet();
}

void OnlineChartsWeather::PumpMet() {
  constexpr int kMetParallel = 4;
  const std::set<std::string> visible(visible_nodes_.begin(), visible_nodes_.end());
  while (met_active_ < kMetParallel && !met_queue_.empty() && !shutdown_ &&
         std::time(nullptr) >= met_blocked_until_) {
    const std::string key = met_queue_.front();
    met_queue_.erase(met_queue_.begin());
    // Points left behind by a chart move are not worth a request any more.
    if (!visible.count(key) || node_requests_.count("w:" + key) || !nodes_.count(key)) continue;
    const Node& node = nodes_[key];
    node_requests_.insert("w:" + key);
    ++met_active_;
    const bool started = http_->Get(MetUrl(node.lat, node.lon), [this, key](bool ok, long status, std::string body) {
      --met_active_;
      node_requests_.erase("w:" + key);
      if (shutdown_) return;
      auto found = nodes_.find(key);
      if (ok && found != nodes_.end() &&
          MergeMet(body, &found->second.series, true, &found->second.elevation)) {
        found->second.wind_time = std::time(nullptr);
        found->second.wind_met = true;
        SaveNode(key, found->second);
      } else if (!ok) {
        // MET Norway refuses unidentified or excessive use with 403 or 429.
        met_blocked_until_ = std::time(nullptr) + ((status == 403 || status == 429) ? 3600 : kRetryAfterFailure);
        met_queue_.clear();
        last_error_ = status > 0 ? wxString::Format("MET Norway returned HTTP %ld", status)
                                 : wxString("MET Norway could not be reached");
      }
      PumpMet();
      RequestRefresh(GetOCPNCanvasWindow());
    });
    if (!started) {
      --met_active_;
      node_requests_.erase("w:" + key);
    }
  }
}

bool OnlineChartsWeather::ProviderPosition(double* lat, double* lon, bool* at_boat,
                                           double* zoom) const {
  *zoom = view_zoom_;
  if (BoatKnown()) {
    *lat = boat_lat_;
    *lon = boat_lon_;
    *at_boat = true;
    return true;
  }
  *at_boat = false;
  if (!std::isfinite(centre_lat_) || !std::isfinite(centre_lon_)) return false;
  *lat = centre_lat_;
  *lon = centre_lon_;
  return true;
}

bool OnlineChartsWeather::LoadNode(const std::string& key, Node* node) const {
  wxFile file(CacheFile(key), wxFile::read);
  if (!file.IsOpened() || file.Length() <= 0 || file.Length() > 1024 * 1024) return false;
  std::string text(static_cast<size_t>(file.Length()), '\0');
  if (file.Read(&text[0], text.size()) != static_cast<ssize_t>(text.size())) return false;
  const std::time_t now = std::time(nullptr);
  const std::time_t keep = static_cast<std::time_t>(settings_.keep_hours) * 3600;
  Node result;
  size_t start = 0;
  int line_number = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty()) continue;
    online_charts::JsonValue value;
    if (!online_charts::ParseJson(line, &value) || !value.IsArray()) return false;
    const auto number = [&](size_t i) {
      return i < value.items.size() && value.items[i].IsNumber() ? value.items[i].number : NAN;
    };
    if (line_number == 0) {
      result.wind_time = static_cast<std::time_t>(std::isfinite(number(0)) ? number(0) : 0);
      result.wave_time = static_cast<std::time_t>(std::isfinite(number(1)) ? number(1) : 0);
      result.lat = number(2);
      result.lon = number(3);
      result.wind_met = number(4) == 1;
      result.has_current = number(5) == 1;
      result.elevation = number(6);
    } else {
      Hour hour;
      hour.wind_speed = number(1);
      hour.wind_dir = number(2);
      hour.wind_gust = number(3);
      hour.wave_height = number(4);
      hour.wave_dir = number(5);
      hour.current_speed = number(6);
      hour.current_dir = number(7);
      result.series.times.push_back(static_cast<std::time_t>(number(0)));
      result.series.hours.push_back(hour);
    }
    ++line_number;
  }
  // Data older than the offline limit is not shown; it counts as missing.
  if (now - result.wind_time > keep) result.wind_time = 0;
  if (now - result.wave_time > keep) result.wave_time = 0;
  if ((!result.wind_time && !result.wave_time) || !std::isfinite(result.lat)) return false;
  *node = std::move(result);
  return true;
}

void OnlineChartsWeather::SaveNode(const std::string& key, const Node& node) {
  auto value = [](double v) { return std::isfinite(v) ? Fixed(v, 2) : std::string("null"); };
  // One JSON array per line: the download times and position, then one line
  // per hour (time, wind speed, direction and gusts, wave height and direction).
  std::string text = "[" + std::to_string(static_cast<long long>(node.wind_time)) + ',' +
                     std::to_string(static_cast<long long>(node.wave_time)) + ',' + Fixed(node.lat, 4) +
                     ',' + Fixed(node.lon, 4) + ',' + (node.wind_met ? "1" : "0") + ',' +
                     (node.has_current ? "1" : "0") + ',' +
                     (std::isfinite(node.elevation) ? Fixed(node.elevation, 0) : std::string("null")) + "]\n";
  for (size_t i = 0; i < node.series.times.size(); ++i) {
    const Hour& h = node.series.hours[i];
    text += "[" + std::to_string(static_cast<long long>(node.series.times[i])) + ',' + value(h.wind_speed) +
            ',' + value(h.wind_dir) + ',' + value(h.wind_gust) + ',' + value(h.wave_height) + ',' +
            value(h.wave_dir) + ',' + value(h.current_speed) + ',' + value(h.current_dir) + "]\n";
  }
  const wxString path = CacheFile(key);
  const wxString temporary = path + ".tmp";
  wxFile file(temporary, wxFile::write);
  if (!file.IsOpened() || file.Write(text.data(), text.size()) != text.size()) {
    wxRemoveFile(temporary);
    return;
  }
  file.Close();
  if (!wxRenameFile(temporary, path, true)) wxRemoveFile(temporary);
}

void OnlineChartsWeather::Fetch(const std::string& key,
                                const std::vector<std::pair<double, double>>& points, bool field,
                                bool force) {
  (void)force;
  if (std::find(fetching_.begin(), fetching_.end(), key) != fetching_.end()) return;
  const wxString model = kModels[settings_.model].id;
  const wxString lats = PointList(points, true);
  const wxString lons = PointList(points, false);
  const wxString days = wxString::Format("%d", kForecastDays);

  wxString forecast_url;
  wxString marine_url;
  if (!field || settings_.wind_field) {
    forecast_url = "https://api.open-meteo.com/v1/forecast?latitude=" + lats + "&longitude=" + lons +
                   (field ? "&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m"
                          : "&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m,"
                            "temperature_2m,surface_pressure") +
                   "&wind_speed_unit=kn&temperature_unit=celsius&timeformat=unixtime&forecast_days=" +
                   days;
    if (settings_.model > 0) forecast_url += "&models=" + model;
  }
  // While Open-Meteo refuses requests, take wind and air from MET Norway.
  const std::time_t now = std::time(nullptr);
  const bool met = !field && forecast_blocked_until_ > now && now >= met_blocked_until_;
  if (met) forecast_url = MetUrl(points.front().first, points.front().second);
  if (!field || settings_.wave_field || settings_.current_field) {
    marine_url = "https://marine-api.open-meteo.com/v1/marine?latitude=" + lats + "&longitude=" +
                 lons +
                 (field ? "&hourly=wave_height,wave_direction,ocean_current_velocity,ocean_current_direction"
                        : "&hourly=wave_height,wave_direction,wave_period,ocean_current_velocity,"
                          "ocean_current_direction") +
                 "&timeformat=unixtime&forecast_days=" + days;
  }

  auto job = std::make_shared<Job>();
  job->key = key;
  job->points = points;
  job->field = field;
  job->met = met;
  job->pending = (forecast_url.empty() ? 0 : 1) + (marine_url.empty() ? 0 : 1);
  if (job->pending == 0) return;
  fetching_.push_back(key);
  auto request = [this, job](const wxString& url, bool marine) {
    const bool started = http_->Get(url, [this, job, marine](bool ok, long status, std::string body) {
      if (ok) {
        (marine ? job->marine : job->forecast) = std::move(body);
      } else {
        NoteFailure(marine, status, body);
      }
      if (--job->pending == 0) Finish(job);
    });
    if (!started && --job->pending == 0) Finish(job);
  };
  if (!forecast_url.empty()) request(forecast_url, false);
  if (!marine_url.empty() && !shutdown_) request(marine_url, true);
}

void OnlineChartsWeather::Finish(const std::shared_ptr<Job>& job) {
  fetching_.erase(std::remove(fetching_.begin(), fetching_.end(), job->key), fetching_.end());
  if (shutdown_) return;
  const std::time_t now = std::time(nullptr);
  Reading reading;
  reading.key = job->key;
  reading.fetched = now;
  for (const auto& point : job->points) {
    FieldPoint field_point;
    field_point.lat = point.first;
    field_point.lon = point.second;
    reading.points.push_back(field_point);
  }
  reading.met = job->met;
  if (!ParseSeries(job->forecast, job->marine, &reading.points, job->met)) {
    // Keep showing the cached reading, marked offline, until a retry succeeds.
    last_failure_ = now;
    offline_ = true;
    if (last_error_.empty()) last_error_ = "Open-Meteo could not be reached";
    RequestRefresh(GetOCPNCanvasWindow());
    return;
  }
  // A reading missing the forecast or the marine answer is shown but not
  // cached, and is requested again after the back-off.
  reading.complete = !job->forecast.empty() && !job->marine.empty();
  if (reading.complete) {
    SaveCached(job->key, now, job->points, (job->met ? "MET:" : "") + job->forecast, job->marine);
    last_failure_ = 0;
    refusals_ = 0;
    last_error_.clear();
  } else {
    // Retry at once when MET Norway can now stand in for a refused Open-Meteo.
    last_failure_ = (!job->met && forecast_blocked_until_ > now && now >= met_blocked_until_) ? 0 : now;
  }
  point_ = std::move(reading);
  offline_ = false;
  RequestRefresh(GetOCPNCanvasWindow());
}

bool OnlineChartsWeather::LoadCached(const std::string& key, Reading* reading) const {
  const wxString path = CacheFile(key);
  if (!wxFileName::FileExists(path)) return false;
  wxFile file(path, wxFile::read);
  if (!file.IsOpened() || file.Length() <= 0 || file.Length() > 16 * 1024 * 1024) return false;
  std::string text(static_cast<size_t>(file.Length()), '\0');
  if (file.Read(&text[0], text.size()) != static_cast<ssize_t>(text.size())) return false;
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size() && lines.size() < 4) {
    const size_t end = text.find('\n', start);
    lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (lines.size() < 4) return false;
  const std::time_t fetched = static_cast<std::time_t>(std::atoll(lines[0].c_str()));
  const std::time_t now = std::time(nullptr);
  if (fetched <= 0 || now - fetched > static_cast<std::time_t>(settings_.keep_hours) * 3600) {
    return false;
  }
  Reading result;
  result.key = key;
  result.fetched = fetched;
  result.complete = !lines[2].empty() && !lines[3].empty();
  size_t pos = 0;
  const std::string& list = lines[1];
  while (pos < list.size()) {
    const size_t semicolon = list.find(';', pos);
    const std::string item = list.substr(pos, semicolon == std::string::npos ? std::string::npos
                                                                              : semicolon - pos);
    const size_t comma = item.find(',');
    if (comma != std::string::npos) {
      online_charts::JsonValue lat_value;
      online_charts::JsonValue lon_value;
      if (online_charts::ParseJson(item.substr(0, comma), &lat_value) &&
          online_charts::ParseJson(item.substr(comma + 1), &lon_value)) {
        FieldPoint point;
        point.lat = lat_value.number;
        point.lon = lon_value.number;
        result.points.push_back(point);
      }
    }
    if (semicolon == std::string::npos) break;
    pos = semicolon + 1;
  }
  result.met = lines[2].compare(0, 4, "MET:") == 0;
  if (result.met) lines[2].erase(0, 4);
  if (result.points.empty() || !ParseSeries(lines[2], lines[3], &result.points, result.met)) return false;
  *reading = std::move(result);
  return true;
}

void OnlineChartsWeather::SaveCached(const std::string& key, std::time_t fetched,
                                     const std::vector<std::pair<double, double>>& points,
                                     const std::string& forecast, const std::string& marine) {
  std::string text = std::to_string(static_cast<long long>(fetched)) + '\n';
  for (size_t i = 0; i < points.size(); ++i) {
    if (i) text += ';';
    text += Fixed(points[i].first, 4) + ',' + Fixed(points[i].second, 4);
  }
  auto one_line = [](std::string body) {
    std::replace(body.begin(), body.end(), '\n', ' ');
    std::replace(body.begin(), body.end(), '\r', ' ');
    return body;
  };
  text += '\n' + one_line(forecast) + '\n' + one_line(marine) + '\n';
  const wxString path = CacheFile(key);
  const wxString temporary = path + ".tmp";
  wxFile file(temporary, wxFile::write);
  if (!file.IsOpened() || file.Write(text.data(), text.size()) != text.size()) {
    wxRemoveFile(temporary);
    return;
  }
  file.Close();
  if (!wxRenameFile(temporary, path, true)) wxRemoveFile(temporary);
  PruneCache();
}

void OnlineChartsWeather::PruneCache() {
  if (cache_dir_.empty() || !wxDirExists(cache_dir_)) return;
  wxArrayString files;
  wxDir::GetAllFiles(cache_dir_, &files, "*.txt", wxDIR_FILES);
  const std::time_t now = std::time(nullptr);
  const std::time_t keep = static_cast<std::time_t>(settings_.keep_hours) * 3600;
  std::vector<std::pair<std::time_t, wxString>> kept;
  for (const auto& path : files) {
    const wxDateTime modified = wxFileName(path).GetModificationTime();
    const std::time_t time = modified.IsValid() ? modified.GetTicks() : 0;
    if (now - time > keep) {
      wxRemoveFile(path);
    } else {
      kept.emplace_back(time, path);
    }
  }
  if (kept.size() > kMaxCacheFiles) {
    std::sort(kept.begin(), kept.end());
    for (size_t i = 0; i + kMaxCacheFiles < kept.size(); ++i) wxRemoveFile(kept[i].second);
  }
}

wxString OnlineChartsWeather::ClearCache() {
  size_t removed = 0;
  if (wxDirExists(cache_dir_)) {
    wxArrayString files;
    wxDir::GetAllFiles(cache_dir_, &files, "*.txt*", wxDIR_FILES);
    for (const auto& path : files) {
      if (wxRemoveFile(path)) ++removed;
    }
  }
  point_ = Reading();
  nodes_.clear();
  visible_nodes_.clear();
  last_failure_ = 0;
  forecast_blocked_until_ = 0;
  marine_blocked_until_ = 0;
  met_blocked_until_ = 0;
  met_queue_.clear();
  RequestRefresh(GetOCPNCanvasWindow());
  return wxString::Format("Removed %u cached weather readings.", static_cast<unsigned>(removed));
}

wxString OnlineChartsWeather::StatusText() const {
  if (!settings_.enabled) return "Weather is off.";
  const std::time_t now = std::time(nullptr);
  wxString text;
  if (point_.fetched > 0) {
    text = wxString::Format("Conditions: %s %s, updated %s", BoatKnown() ? "at the boat" : "at the chart centre",
                            offline_ ? "(offline, from cache)" : "(online)", Age(now - point_.fetched));
  } else {
    text = BoatKnown() ? "Conditions: waiting for the first forecast at the boat"
                       : "Conditions: waiting for the first forecast at the chart centre";
  }
  if (settings_.wind_field || settings_.wave_field) {
    size_t wind = 0;
    size_t waves = 0;
    for (const auto& key : visible_nodes_) {
      auto found = nodes_.find(key);
      if (found == nodes_.end()) continue;
      if (found->second.wind_time) ++wind;
      if (found->second.wave_time) ++waves;
    }
    text += visible_nodes_.empty()
                ? wxString("\nField: waiting for the chart to settle")
                : wxString::Format(L"\nField: %u points every %g\u00b0; wind at %u, waves at %u",
                                   static_cast<unsigned>(visible_nodes_.size()), node_step_,
                                   static_cast<unsigned>(wind), static_cast<unsigned>(waves));
  }
  size_t files = 0;
  if (wxDirExists(cache_dir_)) {
    wxArrayString list;
    wxDir::GetAllFiles(cache_dir_, &list, "*.txt", wxDIR_FILES);
    files = list.size();
  }
  text += wxString::Format("\nCached readings: %u (kept up to %d h)", static_cast<unsigned>(files),
                           settings_.keep_hours);
  if (!last_error_.empty()) text += "\nLast issue: " + last_error_;
  return text;
}

std::time_t OnlineChartsWeather::TargetTime() const {
  return std::time(nullptr) + static_cast<std::time_t>(settings_.forecast_hour) * 3600;
}

const OnlineChartsWeather::Hour* OnlineChartsWeather::HourAt(const Series& series) const {
  if (series.times.empty()) return nullptr;
  const std::time_t target = TargetTime();
  size_t best = 0;
  for (size_t i = 1; i < series.times.size(); ++i) {
    if (std::llabs(static_cast<long long>(series.times[i] - target)) <
        std::llabs(static_cast<long long>(series.times[best] - target))) {
      best = i;
    }
  }
  // Outside the downloaded window (an old cache): show nothing rather than a
  // reading for the wrong time.
  if (std::llabs(static_cast<long long>(series.times[best] - target)) > 3 * 3600) return nullptr;
  return &series.hours[best];
}

/* ---------- Drawing ---------- */

void OnlineChartsWeather::DrawSegments(piDC* dc, const std::vector<float>& lines,
                                       const std::vector<float>& triangles,
                                       const wxColour& colour, double width) {
  if (gl_pass_) {
    glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
    glLineWidth(static_cast<GLfloat>(std::max(1.0, width * pixel_scale_)));
    glBegin(GL_LINES);
    for (size_t i = 0; i + 3 < lines.size(); i += 4) {
      glVertex2f(lines[i], lines[i + 1]);
      glVertex2f(lines[i + 2], lines[i + 3]);
    }
    glEnd();
    glBegin(GL_TRIANGLES);
    for (size_t i = 0; i + 1 < triangles.size(); i += 2) glVertex2f(triangles[i], triangles[i + 1]);
    glEnd();
    return;
  }
  dc->SetPen(wxPen(colour, std::max(1, static_cast<int>(std::lround(width))), wxPENSTYLE_SOLID));
  for (size_t i = 0; i + 3 < lines.size(); i += 4) {
    dc->DrawLine(static_cast<int>(lines[i]), static_cast<int>(lines[i + 1]),
                 static_cast<int>(lines[i + 2]), static_cast<int>(lines[i + 3]));
  }
  dc->SetBrush(wxBrush(colour));
  for (size_t i = 0; i + 5 < triangles.size(); i += 6) {
    wxPoint points[3] = {
        wxPoint(static_cast<int>(triangles[i]), static_cast<int>(triangles[i + 1])),
        wxPoint(static_cast<int>(triangles[i + 2]), static_cast<int>(triangles[i + 3])),
        wxPoint(static_cast<int>(triangles[i + 4]), static_cast<int>(triangles[i + 5]))};
    dc->DrawPolygon(3, points);
  }
}

// A standard wind barb in knots. The staff starts at the position and points
// to where the wind comes from. Feathers sit at the far end, on the right of
// the staff looking outwards, slanting outwards: a pennant (filled triangle) is
// 50 kn, a full barb 10 kn and a half barb 5 kn, ordered pennants, full barbs,
// half barbs from the tip inwards, with a lone half barb set one step in.
// Under 2.5 kn a double circle marks calm.
void OnlineChartsWeather::DrawBarb(piDC* dc, PlugIn_ViewPort* viewport, double lat, double lon,
                                   double speed, double dir, double size, const wxColour& colour) {
  if (!std::isfinite(speed)) return;
  wxPoint2DDouble at;
  const double north = NorthAngle(viewport, lat, lon, &at);
  if (at.m_x < -size || at.m_y < -size || at.m_x > viewport->pix_width + size ||
      at.m_y > viewport->pix_height + size) {
    return;
  }
  const double angle = north + (std::isfinite(dir) ? dir : 0.0) * kPi / 180.0;
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  // Local frame: the staff runs from (0,0) to (0,-staff); +x is the right of
  // the staff looking along it.
  auto map = [&](double x, double y, std::vector<float>* out) {
    out->push_back(static_cast<float>(at.m_x + x * c - y * s));
    out->push_back(static_cast<float>(at.m_y + x * s + y * c));
  };
  std::vector<float> lines;
  std::vector<float> triangles;
  if (speed < 2.5) {
    for (double r : {size * 0.07, size * 0.13}) {
      for (int i = 0; i < 24; ++i) {
        const double a0 = i * kPi / 12;
        const double a1 = (i + 1) * kPi / 12;
        map(r * std::cos(a0), r * std::sin(a0), &lines);
        map(r * std::cos(a1), r * std::sin(a1), &lines);
      }
    }
  } else {
    const double staff = size * 0.46;
    const double feather = size * 0.24;  // Full barb length.
    const double across = feather * 0.87;  // 60 degrees from the staff...
    const double out = feather * 0.5;      // ...slanting towards the tip.
    const double step = size * 0.075;      // Spacing between feathers.
    const double pennant = size * 0.11;    // Pennant base along the staff.
    const double tip = -staff;
    long remaining = std::lround(speed / 5.0) * 5;
    map(0, 0, &lines);
    map(0, tip, &lines);
    double pos = tip;
    bool any = false;
    while (remaining >= 50) {
      map(0, pos, &triangles);
      map(across, pos, &triangles);
      map(0, pos + pennant, &triangles);
      pos += pennant + step * 0.4;
      remaining -= 50;
      any = true;
    }
    while (remaining >= 10) {
      map(0, pos, &lines);
      map(across, pos - out, &lines);
      pos += step;
      remaining -= 10;
      any = true;
    }
    if (remaining >= 5) {
      if (!any) pos += step;  // A lone half barb sits one step in from the tip.
      map(0, pos, &lines);
      map(across * 0.5, pos - out * 0.5, &lines);
    }
  }
  const double width = std::max(1.5, size / 28.0);
  DrawSegments(dc, lines, triangles, Outline(), width + 2.0);
  DrawSegments(dc, lines, triangles, colour, width);
}

// A red arrow pointing the way the surface current flows, longer for a
// stronger current: 14 px at slack water up to 46 px from 3 kn. A thin arrow
// with an open head marks currents under 0.1 kn. scale is canvas pixels per
// logical pixel (larger at the boat).
void OnlineChartsWeather::DrawCurrentArrow(piDC* dc, PlugIn_ViewPort* viewport, double lat,
                                           double lon, double speed, double dir, double scale,
                                           double offset_y) {
  if (!std::isfinite(speed) || !std::isfinite(dir)) return;
  wxPoint2DDouble at;
  const double north = NorthAngle(viewport, lat, lon, &at);
  at.m_y += offset_y;
  const double angle = north + dir * kPi / 180.0;
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  auto map = [&](double x, double y, std::vector<float>* out) {
    out->push_back(static_cast<float>(at.m_x + x * c - y * s));
    out->push_back(static_cast<float>(at.m_y + x * s + y * c));
  };
  const double length = (14.0 + std::min(speed, 3.0) / 3.0 * 32.0) * scale;
  const double head = 7.0 * scale;
  std::vector<float> lines;
  std::vector<float> triangles;
  // Centred on the point, pointing along -y before rotation.
  map(0, length / 2, &lines);
  map(0, -length / 2 + head * 0.8, &lines);
  map(0, -length / 2, &triangles);
  map(-head * 0.6, -length / 2 + head * 1.3, &triangles);
  map(head * 0.6, -length / 2 + head * 1.3, &triangles);
  const double width = std::max(1.5, 2.2 * scale);
  const wxColour red(214, 40, 40);
  DrawSegments(dc, lines, triangles, Outline(), width + 2.0);
  DrawSegments(dc, lines, triangles, red, width);
}

// An arrow pointing the way the waves travel (Open-Meteo gives the direction
// they come from), coloured by wave height.
void OnlineChartsWeather::DrawWaveArrow(piDC* dc, PlugIn_ViewPort* viewport, double lat,
                                        double lon, double height, double dir, double size,
                                        double offset_x) {
  if (!std::isfinite(height) || !std::isfinite(dir)) return;
  wxPoint2DDouble at;
  const double north = NorthAngle(viewport, lat, lon, &at);
  at.m_x += offset_x;
  const double angle = north + (dir + 180.0) * kPi / 180.0;
  const double c = std::cos(angle);
  const double s = std::sin(angle);
  auto map = [&](double x, double y, std::vector<float>* out) {
    out->push_back(static_cast<float>(at.m_x + x * c - y * s));
    out->push_back(static_cast<float>(at.m_y + x * s + y * c));
  };
  const double half = size * 0.34;
  const double head = size * 0.13;
  std::vector<float> lines;
  map(0, half, &lines);
  map(0, -half, &lines);
  map(-head, -half + head, &lines);
  map(0, -half, &lines);
  map(0, -half, &lines);
  map(head, -half + head, &lines);
  const double width = std::max(1.5, size / 16.0);
  DrawSegments(dc, lines, {}, Outline(), width + 2.0);
  DrawSegments(dc, lines, {}, WaveColour(height), width);
}

OnlineChartsConditions OnlineChartsWeather::Current() const {
  OnlineChartsConditions c;
  const std::time_t now = std::time(nullptr);
  c.place = BoatKnown() ? "At the boat" : "At the chart centre";
  const Hour* hour = point_.points.empty() ? nullptr : HourAt(point_.points.front().series);
  if (hour) {
    if (std::isfinite(hour->wind_speed)) {
      c.wind = wxString::Format("%.0f kn", hour->wind_speed);
      if (std::isfinite(hour->wind_dir)) c.wind += wxString(" ") + Compass(hour->wind_dir);
    }
    if (std::isfinite(hour->wind_gust)) c.gusts = wxString::Format("%.0f kn", hour->wind_gust);
    if (std::isfinite(hour->air_temp)) {
      c.air_temp = settings_.temp_fahrenheit
                       ? wxString::Format(L"%.1f °F", hour->air_temp * 9.0 / 5.0 + 32.0)
                       : wxString::Format(L"%.1f °C", hour->air_temp);
    }
    if (std::isfinite(hour->pressure)) c.pressure = wxString::Format("%.0f hPa", hour->pressure);
    if (std::isfinite(hour->wave_height)) {
      c.waves = settings_.wave_feet ? wxString::Format("%.1f ft", hour->wave_height * 3.28084)
                                    : wxString::Format("%.1f m", hour->wave_height);
      if (std::isfinite(hour->wave_dir)) c.waves += wxString(" ") + Compass(hour->wave_dir);
      if (std::isfinite(hour->wave_period)) c.waves += wxString::Format(L" · %.0f s", hour->wave_period);
    }
    if (std::isfinite(hour->current_speed)) {
      c.current = wxString::Format("%.1f kn", hour->current_speed);
      if (std::isfinite(hour->current_dir)) c.current += wxString(" to ") + Compass(hour->current_dir);
    }
  }
  const wxDateTime when(static_cast<time_t>(TargetTime()));
  c.when = when.Format("%a %H:%M");
  c.when += settings_.forecast_hour ? wxString::Format(" (+%d h)", settings_.forecast_hour)
                                    : wxString(" (now)");
  c.when += wxString(L" · ") + (point_.met ? wxString("MET Norway")
                                            : wxString::FromUTF8(kModels[settings_.model].name));
  if (!settings_.enabled) {
    c.status = "Weather is off";
  } else if (point_.fetched == 0) {
    c.status = offline_ ? "Offline; no cached forecast for this area" : "Loading the forecast...";
  } else if (!hour) {
    c.status = "No forecast for this hour in the cached reading";
  } else {
    c.status = wxString(offline_ ? L"Offline · cached " : (point_.met ? L"MET Norway · updated "
                                                                    : L"Open-Meteo · updated ")) +
               Age(now - point_.fetched);
  }
  // While Open-Meteo refuses requests and nothing replaces it, say so.
  if (settings_.enabled && !last_error_.empty() && !point_.met &&
      (forecast_blocked_until_ > now || marine_blocked_until_ > now)) {
    c.status = last_error_;
  }
  return c;
}

/* ---------- Shapes and text in canvas pixels, for both canvases ---------- */

void OnlineChartsWeather::FillCircle(piDC* dc, double x, double y, double r,
                                     const wxColour& colour) {
  if (gl_pass_) {
    glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(x, y);
    for (int i = 0; i <= 32; ++i) {
      const double a = i * 2 * kPi / 32;
      glVertex2d(x + r * std::cos(a), y + r * std::sin(a));
    }
    glEnd();
    return;
  }
  dc->SetPen(*wxTRANSPARENT_PEN);
  dc->SetBrush(wxBrush(colour));
  dc->DrawCircle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(r));
}

void OnlineChartsWeather::FillRoundRect(piDC* dc, double x, double y, double w, double h,
                                        double r, const wxColour& colour) {
  r = std::min(r, std::min(w, h) / 2);
  if (gl_pass_) {
    glColor4ub(colour.Red(), colour.Green(), colour.Blue(), colour.Alpha());
    glBegin(GL_POLYGON);
    const double cx[4] = {x + w - r, x + w - r, x + r, x + r};
    const double cy[4] = {y + r, y + h - r, y + h - r, y + r};
    for (int corner = 0; corner < 4; ++corner) {
      for (int i = 0; i <= 8; ++i) {
        const double a = (corner * 90 - 90 + i * 90.0 / 8) * kPi / 180;
        glVertex2d(cx[corner] + r * std::cos(a), cy[corner] + r * std::sin(a));
      }
    }
    glEnd();
    return;
  }
  dc->SetPen(*wxTRANSPARENT_PEN);
  dc->SetBrush(wxBrush(colour));
  dc->DrawRoundedRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
                           static_cast<int>(h), r);
}

double OnlineChartsWeather::TextWidth(piDC* dc, const wxString& text, double height, bool bold) {
  if (gl_pass_) {
    online_charts::GlTextAtlas& atlas = bold ? text_bold_ : text_;
    wxFont font = PanelFont(13);
    if (bold) font.SetWeight(wxFONTWEIGHT_BOLD);
    if (!atlas.Ensure(font)) return 0;
    float w = 0;
    atlas.Measure(text, static_cast<float>(height), &w, nullptr);
    return w;
  }
  wxFont font = PanelFont(static_cast<int>(std::lround(height)));
  if (bold) font.SetWeight(wxFONTWEIGHT_BOLD);
  dc->SetFont(font);
  wxCoord w = 0, h = 0;
  dc->GetTextExtent(text, &w, &h);
  return w;
}

void OnlineChartsWeather::Text(piDC* dc, const wxString& text, double x, double y, double height,
                               const wxColour& colour, bool bold) {
  if (gl_pass_) {
    online_charts::GlTextAtlas& atlas = bold ? text_bold_ : text_;
    wxFont font = PanelFont(13);
    if (bold) font.SetWeight(wxFONTWEIGHT_BOLD);
    if (!atlas.Ensure(font)) return;
    atlas.Draw(text, static_cast<float>(x), static_cast<float>(y), static_cast<float>(height), colour,
               wxColour(0, 0, 0, 0));
    return;
  }
  wxFont font = PanelFont(static_cast<int>(std::lround(height)));
  if (bold) font.SetWeight(wxFONTWEIGHT_BOLD);
  dc->SetFont(font);
  dc->SetTextForeground(colour);
  dc->DrawText(text, static_cast<int>(x), static_cast<int>(y));
}

void OnlineChartsWeather::DrawPanel(piDC* dc, PlugIn_ViewPort* viewport) {
  (void)viewport;
  const OnlineChartsConditions c = Current();
  // wxArrayString rather than std::vector<wxString>: MSVC builds of
  // wxWidgets import that vector type from the wx DLL only in part.
  wxArrayString lines;
  lines.push_back("Weather " + c.place.Lower());
  if (!c.wind.empty()) lines.push_back("Wind " + c.wind + (c.gusts.empty() ? wxString() : ", gusts " + c.gusts));
  wxString air = c.air_temp.empty() ? wxString() : "Air " + c.air_temp;
  if (!c.pressure.empty()) air += (air.empty() ? wxString() : wxString(L" · ")) + c.pressure;
  if (!air.empty()) lines.push_back(air);
  if (!c.waves.empty()) lines.push_back("Waves " + c.waves);
  if (!c.current.empty()) lines.push_back("Current " + c.current);
  lines.push_back(c.when);
  lines.push_back(c.status);

  const double ps = pixel_scale_;
  const double text_height = 13 * ps;
  const double line_step = text_height * 1.25;
  const double pad = 8 * ps;
  const double left = 60 * ps;
  const double top = 10 * ps;
  double width = 0;
  for (size_t i = 0; i < lines.size(); ++i) width = std::max(width, TextWidth(dc, lines[i], text_height));
  const double box_h = line_step * lines.size() + 2 * pad - (line_step - text_height);
  FillRoundRect(dc, left, top, width + 2 * pad, box_h, 6 * ps, wxColour(255, 255, 255, 225));
  for (size_t i = 0; i < lines.size(); ++i) {
    const wxColour colour = i == 0 ? wxColour(7, 59, 85)
                                   : (i + 2 >= lines.size() ? wxColour(90, 105, 112) : wxColour(40, 55, 64));
    Text(dc, lines[i], left + pad, top + pad + line_step * i, text_height, colour, i == 0);
  }
}

/* ---------- Forecast time bar ---------- */

OnlineChartsWeather::TimelineLayout OnlineChartsWeather::Layout() const {
  TimelineLayout l;
  if (canvas_w_ < 340 || canvas_h_ < 220) return l;
  // Sits on the bottom of the chart, the same height as OpenCPN's zoom and
  // scale controls in the bottom-right corner and just to their left. The
  // plugin API does not expose their size: these are their measured size at
  // the default GUI scale (251 x 42 points, 8 points from the right and 7 from
  // the bottom), scaled with OpenCPN's toolbar scale. The bar also keeps clear
  // of the scale bar and attribution in the bottom-left corner.
  double gui = GetOCPNGUIToolScaleFactor_PlugIn();
  if (!std::isfinite(gui) || gui <= 0) gui = 1.0;
  const double controls_w = 252 * gui;
  const double controls_h = 42 * gui;
  const double right = canvas_w_ - 8 - controls_w - 10;
  const double left = std::max(250.0, right - 640);
  l.h = controls_h;
  l.y = canvas_h_ - 7 - l.h;
  l.x = left;
  l.w = right - left;
  if (l.w < 330) {
    // Narrow chart: centre the bar above the bottom controls instead.
    l.w = std::min(640.0, canvas_w_ - 40);
    l.x = (canvas_w_ - l.w) / 2;
    l.y = canvas_h_ - 7 - controls_h - 8 - l.h;
  }
  l.play_r = l.h * 0.36;
  l.play_x = l.x + 8 + l.play_r;
  l.play_y = l.y + l.h / 2;
  l.now_h = l.h * 0.66;
  l.now_w = 52;
  l.now_x = l.x + l.w - 8 - l.now_w;
  l.now_y = l.y + (l.h - l.now_h) / 2;
  l.label_x = l.now_x - 12;
  l.track_x0 = l.play_x + l.play_r + 14;
  l.track_x1 = l.label_x - 84;
  l.track_y = l.play_y;
  l.valid = l.track_x1 - l.track_x0 > 60;
  return l;
}

int OnlineChartsWeather::HourAtX(double x) const {
  const TimelineLayout l = Layout();
  if (!l.valid) return settings_.forecast_hour;
  const double f = (x - l.track_x0) / (l.track_x1 - l.track_x0);
  return std::max(0, std::min(72, static_cast<int>(std::lround(f * 72))));
}

void OnlineChartsWeather::DrawTimeline(piDC* dc) {
  const TimelineLayout l = Layout();
  if (!l.valid) return;
  const double s = pixel_scale_;
  FillRoundRect(dc, l.x * s, l.y * s, l.w * s, l.h * s, 10 * s, wxColour(16, 38, 52, 215));

  // Play / pause button.
  FillCircle(dc, l.play_x * s, l.play_y * s, l.play_r * s, wxColour(31, 143, 195));
  const wxColour white(255, 255, 255);
  if (Playing()) {
    FillRoundRect(dc, (l.play_x - 6) * s, (l.play_y - 6) * s, 4 * s, 12 * s, 1 * s, white);
    FillRoundRect(dc, (l.play_x + 2) * s, (l.play_y - 6) * s, 4 * s, 12 * s, 1 * s, white);
  } else if (gl_pass_) {
    glColor4ub(255, 255, 255, 255);
    glBegin(GL_TRIANGLES);
    glVertex2d((l.play_x - 4) * s, (l.play_y - 6.5) * s);
    glVertex2d((l.play_x + 7) * s, l.play_y * s);
    glVertex2d((l.play_x - 4) * s, (l.play_y + 6.5) * s);
    glEnd();
  } else {
    wxPoint triangle[3] = {wxPoint(static_cast<int>(l.play_x - 5), static_cast<int>(l.play_y - 8)),
                           wxPoint(static_cast<int>(l.play_x + 8), static_cast<int>(l.play_y)),
                           wxPoint(static_cast<int>(l.play_x - 5), static_cast<int>(l.play_y + 8))};
    dc->SetBrush(wxBrush(white));
    dc->DrawPolygon(3, triangle);
  }

  // Track, elapsed part and thumb.
  const double f = settings_.forecast_hour / 72.0;
  const double thumb = l.track_x0 + (l.track_x1 - l.track_x0) * f;
  FillRoundRect(dc, l.track_x0 * s, (l.track_y - 3) * s, (l.track_x1 - l.track_x0) * s, 6 * s, 3 * s,
                wxColour(255, 255, 255, 60));
  FillRoundRect(dc, l.track_x0 * s, (l.track_y - 3) * s, std::max(6.0, thumb - l.track_x0) * s, 6 * s,
                3 * s, wxColour(56, 170, 222));
  FillCircle(dc, thumb * s, l.track_y * s, 8 * s, white);

  // "Now" or "+N h", the local time, and the Now button.
  const wxString big = settings_.forecast_hour ? wxString::Format("+%d h", settings_.forecast_hour)
                                               : wxString("Now");
  const wxString time_text = wxDateTime(static_cast<time_t>(TargetTime())).Format("%a %H:00");
  const double big_h = l.h * 0.38 * s;
  const double small_h = l.h * 0.28 * s;
  Text(dc, big, l.label_x * s - TextWidth(dc, big, big_h, true), (l.y + l.h * 0.08) * s, big_h, white, true);
  Text(dc, time_text, l.label_x * s - TextWidth(dc, time_text, small_h), (l.y + l.h * 0.56) * s, small_h,
       wxColour(200, 215, 222));
  FillRoundRect(dc, l.now_x * s, l.now_y * s, l.now_w * s, l.now_h * s, 8 * s,
                settings_.forecast_hour ? wxColour(255, 255, 255, 45) : wxColour(255, 255, 255, 20));
  const double now_h = l.h * 0.32 * s;
  Text(dc, "Now", (l.now_x + l.now_w / 2) * s - TextWidth(dc, "Now", now_h, true) / 2,
       l.now_y * s + (l.now_h * s - now_h * 1.2) / 2, now_h, white, true);
}

void OnlineChartsWeather::DrawReadout(piDC* dc) {
  if (hover_ < 0 || static_cast<size_t>(hover_) >= visible_nodes_.size()) return;
  auto node = nodes_.find(visible_nodes_[hover_]);
  if (node == nodes_.end()) return;
  const Hour* hour = HourAt(node->second.series);
  if (!hour) return;
  wxString text;
  if (std::isfinite(hour->wind_speed)) {
    text = wxString::Format("Wind %.0f kn", hour->wind_speed);
    if (std::isfinite(hour->wind_gust)) text += wxString::Format(", gusts %.0f kn", hour->wind_gust);
    if (std::isfinite(hour->wind_dir)) text += wxString(" from ") + Compass(hour->wind_dir);
  }
  if (std::isfinite(hour->wave_height)) {
    if (!text.empty()) text += L" · ";
    text += settings_.wave_feet ? wxString::Format("Waves %.1f ft", hour->wave_height * 3.28084)
                                : wxString::Format("Waves %.1f m", hour->wave_height);
  }
  if (std::isfinite(hour->current_speed)) {
    if (!text.empty()) text += L" · ";
    text += wxString::Format("Current %.1f kn", hour->current_speed);
    if (std::isfinite(hour->current_dir)) text += wxString(" towards ") + Compass(hour->current_dir);
  }
  if (text.empty()) return;
  const double s = pixel_scale_;
  const double h = 13 * s;
  const double w = TextWidth(dc, text, h) + 16 * s;
  double x = (hover_x_ + 16) * s;
  double y = (hover_y_ + 16) * s;
  if (x + w > canvas_w_ * s) x = (hover_x_ - 16) * s - w;
  FillRoundRect(dc, x, y, w, h + 12 * s, 5 * s, wxColour(255, 255, 255, 235));
  Text(dc, text, x + 8 * s, y + 6 * s, h, wxColour(40, 55, 64));
}

void OnlineChartsWeather::SetPlaying(bool playing) {
  if (playing == Playing()) return;
  if (playing) {
    if (settings_.forecast_hour >= 72) settings_.forecast_hour = 0;
    play_timer_.Start(700);
  } else {
    play_timer_.Stop();
  }
  RequestRefresh(GetOCPNCanvasWindow());
}

void OnlineChartsWeather::OnPlayTimer(wxTimerEvent&) {
  if (settings_.forecast_hour >= 72) {
    play_timer_.Stop();
  } else {
    ++settings_.forecast_hour;
  }
  RequestRefresh(GetOCPNCanvasWindow());
}

bool OnlineChartsWeather::OnMouse(const wxMouseEvent& event) {
  if (shutdown_ || !settings_.enabled) return false;
  // Use the cursor position in the canvas's own logical coordinates rather than
  // the event's, which OpenCPN may have scaled for a Retina OpenGL canvas.
  wxWindow* window = wxDynamicCast(event.GetEventObject(), wxWindow);
  if (!window) window = GetOCPNCanvasWindow();
  const wxPoint cursor = window ? window->ScreenToClient(wxGetMousePosition()) : event.GetPosition();
  const double x = cursor.x;
  const double y = cursor.y;
  const TimelineLayout l = settings_.show_timeline ? Layout() : TimelineLayout();
  if (dragging_) {
    if (event.LeftIsDown() && (event.Dragging() || event.Moving())) {
      SetForecastHour(HourAtX(x));
      RequestRefresh(GetOCPNCanvasWindow());
      return true;
    }
    dragging_ = false;
    return true;
  }
  const bool on_bar = l.valid && x >= l.x && x <= l.x + l.w && y >= l.y && y <= l.y + l.h;
  if (on_bar) {
    if (hover_ >= 0) {
      hover_ = -1;
      RequestRefresh(GetOCPNCanvasWindow());
    }
    if (event.LeftDown()) {
      if (std::hypot(x - l.play_x, y - l.play_y) <= l.play_r + 4) {
        SetPlaying(!Playing());
      } else if (x >= l.now_x && x <= l.now_x + l.now_w && y >= l.now_y && y <= l.now_y + l.now_h) {
        SetPlaying(false);
        SetForecastHour(0);
      } else if (x >= l.track_x0 - 12 && x <= l.track_x1 + 12) {
        SetPlaying(false);
        dragging_ = true;
        SetForecastHour(HourAtX(x));
      }
      RequestRefresh(GetOCPNCanvasWindow());
    }
    // Keep clicks, drags and the wheel over the bar from moving the chart.
    return !event.Moving();
  }
  if (event.Moving() || event.Leaving()) {
    int best = -1;
    double best_distance = 22;
    if (event.Moving()) {
      for (const auto& mark : marks_) {
        const double distance = std::hypot(mark.x - x, mark.y - y);
        if (distance < best_distance) {
          best_distance = distance;
          best = static_cast<int>(mark.point);
        }
      }
    }
    if (best != hover_ || (best >= 0 && (hover_x_ != x || hover_y_ != y))) {
      const bool changed = best != hover_;
      hover_ = best;
      hover_x_ = x;
      hover_y_ = y;
      if (changed || best >= 0) RequestRefresh(GetOCPNCanvasWindow());
    }
  }
  return false;
}

void OnlineChartsWeather::Render(piDC* dc, PlugIn_ViewPort* viewport, double pixel_scale) {
  if (shutdown_ || !viewport || !viewport->bValid) return;
  gl_pass_ = dc == nullptr;
  pixel_scale_ = std::max(1.0, pixel_scale);
  canvas_w_ = viewport->pix_width / pixel_scale_;
  canvas_h_ = viewport->pix_height / pixel_scale_;
  const double zoom = online_charts::ViewportZoom(viewport, pixel_scale_);
  if (zoom > 0) view_zoom_ = std::max(3.0, std::min(18.0, zoom - 1.0));

  // Track the chart so fetches wait until it has stopped moving.
  if (std::isfinite(viewport->lat_min) && std::isfinite(viewport->lat_max) &&
      std::isfinite(viewport->lon_min) && std::isfinite(viewport->lon_max)) {
    const double south = std::min(viewport->lat_min, viewport->lat_max);
    const double north = std::max(viewport->lat_min, viewport->lat_max);
    const double west = viewport->lon_min;
    const double east = viewport->lon_max;
    const double tolerance = std::max(0.001, (north - south) * 0.02);
    if (!view_valid_ || std::fabs(south - view_south_) > tolerance ||
        std::fabs(north - view_north_) > tolerance || std::fabs(west - view_west_) > tolerance ||
        std::fabs(east - view_east_) > tolerance) {
      view_changed_ = std::time(nullptr);
    }
    view_south_ = south;
    view_north_ = north;
    view_west_ = west;
    view_east_ = east;
    centre_lat_ = viewport->clat;
    centre_lon_ = viewport->clon;
    view_valid_ = west < east;
  }
  marks_.clear();
  if (!settings_.enabled) return;

  if (gl_pass_) {
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_LINE_BIT | GL_CURRENT_BIT |
                 GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
  }

  const double ps = pixel_scale_;
  if (settings_.wind_field || settings_.wave_field) {
    for (size_t i = 0; i < visible_nodes_.size(); ++i) {
      auto found = nodes_.find(visible_nodes_[i]);
      if (found == nodes_.end()) continue;
      const Node& point = found->second;
      if (point.lat < view_south_ || point.lat > view_north_ || point.lon < view_west_ ||
          point.lon > view_east_) {
        continue;
      }
      const Hour* hour = HourAt(point.series);
      if (!hour) continue;
      if (settings_.wind_field) {
        DrawBarb(dc, viewport, point.lat, point.lon, hour->wind_speed, hour->wind_dir, 44 * ps,
                 BarbColour());
      }
      // The marine model answers land points with its nearest sea cell, so
      // waves and currents are left out where the terrain is above 2 m.
      const bool land = std::isfinite(point.elevation) && point.elevation > 2.0;
      if (settings_.wave_field && !land) {
        DrawWaveArrow(dc, viewport, point.lat, point.lon, hour->wave_height, hour->wave_dir,
                      30 * ps, settings_.wind_field ? 22 * ps : 0);
      }
      if (settings_.current_field && !land) {
        DrawCurrentArrow(dc, viewport, point.lat, point.lon, hour->current_speed, hour->current_dir,
                         ps, 24 * ps);
      }
      wxPoint2DDouble at;
      GetDoubleCanvasPixLL(viewport, &at, point.lat, point.lon);
      marks_.push_back(Mark{at.m_x / ps, at.m_y / ps, i});
    }
  }
  if (settings_.wind_barb && BoatKnown() && !point_.points.empty()) {
    const Hour* hour = HourAt(point_.points.front().series);
    if (hour) {
      DrawBarb(dc, viewport, boat_lat_, boat_lon_, hour->wind_speed, hour->wind_dir, 64 * ps,
               BarbColour());
    }
  }
  if (settings_.current_field && BoatKnown() && !point_.points.empty()) {
    const Hour* hour = HourAt(point_.points.front().series);
    if (hour) {
      DrawCurrentArrow(dc, viewport, boat_lat_, boat_lon_, hour->current_speed, hour->current_dir,
                       1.5 * ps, 0);
    }
  }
  if (settings_.show_panel) DrawPanel(dc, viewport);
  if (settings_.show_timeline) DrawTimeline(dc);
  DrawReadout(dc);

  if (gl_pass_) glPopAttrib();
}
