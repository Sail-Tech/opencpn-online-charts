#include "online_charts_preferences.h"

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/datetime.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/timer.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/string.h>

#include "ocpn_plugin.h"

namespace {

wxCheckBox* AddCheck(wxWindow* parent, wxSizer* sizer, const wxString& label,
                     bool value) {
  auto* check = new wxCheckBox(parent, wxID_ANY, label);
  check->SetValue(value);
  sizer->Add(check, 0, wxALL | wxEXPAND, 4);
  return check;
}

wxString CacheSize(uint64_t bytes) {
  return wxString::Format("%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}


wxChoice* AddChoice(wxWindow* parent, wxSizer* sizer, const wxString& label,
                    const wxArrayString& items, int selection) {
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  row->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
  choice->SetSelection(selection);
  row->Add(choice, 1, wxEXPAND);
  sizer->Add(row, 0, wxALL | wxEXPAND, 6);
  return choice;
}

wxString ForecastHourLabel(int hours) {
  const wxDateTime when = wxDateTime::Now() + wxTimeSpan::Hours(hours);
  return hours == 0 ? wxString("Now") : wxString::Format(L"+%d h \u00b7 %s", hours, when.Format("%a %H:00"));
}

}  // namespace

bool ShowOnlineChartsPreferences(wxWindow* parent, OnlineChartsSettings* settings,
                                 OnlineChartsOverlay* overlay,
                                 OnlineChartsWeatherSettings* weather_settings,
                                 OnlineChartsWeather* weather,
                                 const std::function<wxString()>& ais_status) {
  if (!settings || !overlay || !weather_settings || !weather) return false;
  wxDialog dialog(parent, wxID_ANY, "Online Charts preferences", wxDefaultPosition,
                  wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* main = new wxBoxSizer(wxVERTICAL);
  auto* notebook = new wxNotebook(&dialog, wxID_ANY);
  auto* chart_page = new wxPanel(notebook);
  auto* chart_sizer = new wxBoxSizer(wxVERTICAL);
  auto* weather_page = new wxPanel(notebook);
  auto* weather_sizer = new wxBoxSizer(wxVERTICAL);
  auto* cache_page = new wxPanel(notebook);
  auto* cache_sizer = new wxBoxSizer(wxVERTICAL);

  auto* layers_box = new wxStaticBoxSizer(wxVERTICAL, chart_page, "Chart layers");
  auto* layers = layers_box->GetStaticBox();
  auto* enabled = AddCheck(layers, layers_box, "Enable online bathymetry overlay",
                           settings->overlay_enabled);
  auto* base_map = AddCheck(layers, layers_box,
                            "Show a public raster base map (drawn over your OpenCPN charts)",
                            settings->show_base_map);
  base_map->SetToolTip(
      "OpenCPN draws plug-ins above its charts, so the base map covers them, o-charts included. "
      "Use it where you have no charts, or lower its opacity.");
  auto* base_map_row = new wxBoxSizer(wxHORIZONTAL);
  base_map_row->Add(new wxStaticText(layers, wxID_ANY, "Base map source"), 0,
                    wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* base_map_provider = new wxChoice(layers, wxID_ANY);
  base_map_provider->Append("OpenStreetMap");
  base_map_provider->Append("Esri World Ocean Base");
  base_map_provider->SetSelection(settings->base_map_provider == 1 ? 1 : 0);
  base_map_row->Add(base_map_provider, 1, wxEXPAND);
  base_map_row->Add(new wxStaticText(layers, wxID_ANY, "Opacity (%)"), 0,
                    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 12);
  auto* base_map_opacity = new wxSpinCtrl(layers, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                          wxDefaultSize, wxSP_ARROW_KEYS, 10, 100,
                                          settings->base_map_opacity);
  base_map_row->Add(base_map_opacity, 0, wxALIGN_CENTER_VERTICAL);
  layers_box->Add(base_map_row, 0, wxALL | wxEXPAND, 6);
  auto* seamarks = AddCheck(layers, layers_box,
                            "Show OpenSeaMap seamarks (buoys, lights, beacons, areas)",
                            settings->show_seamarks);
  auto* contours = AddCheck(layers, layers_box, "Show depth contours and contour labels",
                            settings->show_contours);
  auto* soundings = AddCheck(layers, layers_box, "Show point soundings",
                             settings->show_soundings);
  auto* shading = AddCheck(layers, layers_box, "Show depth colours and shading",
                           settings->show_shading);
  auto* colours_row = new wxBoxSizer(wxHORIZONTAL);
  colours_row->Add(new wxStaticText(layers, wxID_ANY, "Depth colours"), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* depth_colours = new wxChoice(layers, wxID_ANY);
  depth_colours->Append("Chart (light blues)");
  depth_colours->Append("Topo (a colour per depth band)");
  depth_colours->SetSelection(settings->depth_colours == 1 ? 1 : 0);
  depth_colours->SetToolTip(
      "Topo, as on Open Waters' chart view: red in the shallows through orange, yellow and "
      "green to deep blue, so banks and channels stand out. Drying areas are pink.");
  colours_row->Add(depth_colours, 1, wxEXPAND);
  colours_row->Add(new wxStaticText(layers, wxID_ANY, "Opacity (%)"), 0,
                   wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 12);
  auto* depth_opacity = new wxSpinCtrl(layers, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                       wxDefaultSize, wxSP_ARROW_KEYS, 10, 100,
                                       settings->depth_opacity);
  colours_row->Add(depth_opacity, 0, wxALIGN_CENTER_VERTICAL);
  layers_box->Add(colours_row, 0, wxALL | wxEXPAND, 6);
  auto* chart_status = AddCheck(layers, layers_box, "Show status and attribution on the chart",
                                settings->show_status);

  auto* units_row = new wxBoxSizer(wxHORIZONTAL);
  units_row->Add(new wxStaticText(layers, wxID_ANY, "Depth units"), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* units = new wxChoice(layers, wxID_ANY);
  units->Append("Metres (m)");
  units->Append("Feet (ft)");
  units->SetSelection(settings->depth_feet ? 1 : 0);
  units_row->Add(units, 0, wxALIGN_CENTER_VERTICAL);
  layers_box->Add(units_row, 0, wxALL, 6);

  auto* label_size_row = new wxBoxSizer(wxHORIZONTAL);
  label_size_row->Add(new wxStaticText(layers, wxID_ANY, "Depth label size (%)"), 0,
                      wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* depth_label_size = new wxSpinCtrl(
      layers, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
      wxSP_ARROW_KEYS, 50, 300, settings->label_scale);
  depth_label_size->SetToolTip(
      "100% matches the browser dashboard: about 10 px when zoomed out, growing to 18 px "
      "when zoomed in. Overlapping labels are omitted.");
  label_size_row->Add(depth_label_size, 0, wxALIGN_CENTER_VERTICAL);
  layers_box->Add(label_size_row, 0, wxALL, 6);
  auto* source_note = new wxStaticText(
      layers, wxID_ANY,
      "Source: Open Waters Seascape (public vector tiles, zoom 4-15).\n"
      "European coverage is commonly EMODnet (~115 m native resolution).\n"
      "Base maps are network-only, visible-area raster tiles with bounded memory cache,\n"
      "drawn over the OpenCPN charts.\n"
      "This is not C-MAP HRB, Genesis or i-Boating sonar contour data.");
  layers_box->Add(source_note, 0, wxALL | wxEXPAND, 6);
  chart_sizer->Add(layers_box, 0, wxALL | wxEXPAND, 8);

  auto* ais_box = new wxStaticBoxSizer(wxVERTICAL, chart_page, "AIS demo");
  auto* ais_parent = ais_box->GetStaticBox();
  auto* ais_demo = AddCheck(ais_parent, ais_box,
                            "Show live vessels from Open Waters AIS in the chart view "
                            "(testing only, not for navigation)",
                            settings->ais_demo);
  auto* ais_note = new wxStaticText(
      ais_parent, wxID_ANY,
      "The vessels appear as OpenCPN AIS targets, received over the internet from shore\n"
      "stations and AISHub, up to a minute late. Not a replacement for an AIS receiver.\n"
      "If OpenCPN forwards NMEA to other devices, they get these targets too.");
  ais_box->Add(ais_note, 0, wxALL | wxEXPAND, 6);
  auto* ais_state = new wxStaticText(ais_parent, wxID_ANY, ais_status ? ais_status() : wxString());
  ais_box->Add(ais_state, 0, wxALL | wxEXPAND, 6);
  chart_sizer->Add(ais_box, 0, wxALL | wxEXPAND, 8);

  auto* cache_box = new wxStaticBoxSizer(wxVERTICAL, cache_page, "Offline bathymetry cache");
  auto* cache = cache_box->GetStaticBox();
  auto* cache_enabled = AddCheck(cache, cache_box, "Cache bathymetry tiles for offline use",
                                 settings->cache_enabled);
  auto* mode_row = new wxBoxSizer(wxHORIZONTAL);
  mode_row->Add(new wxStaticText(cache, wxID_ANY, "Mode"), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* cache_mode = new wxChoice(cache, wxID_ANY);
  cache_mode->Append("Network first, cache when offline");
  cache_mode->Append("Cache first (saves data)");
  cache_mode->Append("Offline only (no downloads)");
  cache_mode->SetSelection(static_cast<int>(settings->cache_mode));
  mode_row->Add(cache_mode, 1, wxEXPAND);
  cache_box->Add(mode_row, 0, wxALL | wxEXPAND, 6);

  auto* cache_grid = new wxFlexGridSizer(3, 2, 6, 12);
  cache_grid->AddGrowableCol(1, 1);
  cache_grid->Add(new wxStaticText(cache, wxID_ANY, "Max size (MB)"), 0,
                  wxALIGN_CENTER_VERTICAL);
  auto* max_mb = new wxSpinCtrl(cache, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxDefaultSize, wxSP_ARROW_KEYS, 10, 20000,
                                settings->cache_max_mb);
  cache_grid->Add(max_mb, 0, wxEXPAND);
  cache_grid->Add(new wxStaticText(cache, wxID_ANY, "Refresh after (days)"), 0,
                  wxALIGN_CENTER_VERTICAL);
  auto* max_age = new wxSpinCtrl(cache, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxDefaultSize, wxSP_ARROW_KEYS, 1, 365,
                                 settings->cache_max_age_days);
  cache_grid->Add(max_age, 0, wxEXPAND);
  cache_grid->Add(new wxStaticText(cache, wxID_ANY, "Extra zoom levels to save"), 0,
                  wxALIGN_CENTER_VERTICAL);
  auto* prefetch_levels = new wxSpinCtrl(cache, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 0, 4,
                                         settings->prefetch_levels);
  cache_grid->Add(prefetch_levels, 0, wxEXPAND);
  cache_box->Add(cache_grid, 0, wxALL | wxEXPAND, 6);

  auto* actions = new wxBoxSizer(wxHORIZONTAL);
  auto* save_area = new wxButton(cache, wxID_ANY, "Save visible area for offline use");
  auto* clear_cache = new wxButton(cache, wxID_ANY, "Clear cache");
  auto* refresh_status = new wxButton(cache, wxID_ANY, "Refresh status");
  actions->Add(save_area, 0, wxRIGHT, 5);
  actions->Add(clear_cache, 0, wxRIGHT, 5);
  actions->Add(refresh_status, 0);
  cache_box->Add(actions, 0, wxALL | wxEXPAND, 6);
  auto* cache_note = new wxStaticText(
      cache, wxID_ANY,
      "Network and cache modes fall back to stored tiles after a failure.\n"
      "Offline only never downloads. Disabling the cache keeps existing files.\n"
      "The cache contains Open Waters bathymetry only; OpenCPN charts are unchanged.");
  cache_box->Add(cache_note, 0, wxALL | wxEXPAND, 6);
  cache_sizer->Add(cache_box, 0, wxALL | wxEXPAND, 8);

  auto* status_box = new wxStaticBoxSizer(wxVERTICAL, cache_page, "Source and rendering status");
  auto* status_parent = status_box->GetStaticBox();
  auto* status_text = new wxStaticText(status_parent, wxID_ANY, wxEmptyString);
  status_text->SetMinSize(wxSize(620, 125));
  status_box->Add(status_text, 1, wxALL | wxEXPAND, 6);
  cache_sizer->Add(status_box, 0, wxALL | wxEXPAND, 8);

  auto update_status = [&]() {
    const OnlineChartsStatus status = overlay->Status();
    wxString rendering = status.detail;
    if (rendering.empty()) {
      rendering = "No render callback recorded yet. Move or redraw the chart, then refresh.";
    }
    wxString text = rendering + "\n";
    text += wxString::Format(
        L"Viewport zoom: %d · Visible/ready tiles: %u/%u · Drawn: %u areas, %u contours, %u soundings\n",
        status.zoom, static_cast<unsigned>(status.visible_tiles),
        static_cast<unsigned>(status.ready_tiles),
        static_cast<unsigned>(status.drawn_areas),
        static_cast<unsigned>(status.drawn_contours),
        static_cast<unsigned>(status.drawn_soundings));
    text += wxString::Format(L"Base map tiles: %u · Depth colour tiles: %u · Seamark tiles: %u · Depth labels: %d px · Drawing: %.1f ms per frame\n",
                             static_cast<unsigned>(status.base_tiles),
                             static_cast<unsigned>(status.depth_tiles),
                             static_cast<unsigned>(status.seamark_tiles),
                             status.depth_label_size, status.render_ms);
    text += wxString::Format(
        "Downloads: %u active, %u queued, %llu succeeded, %llu failed\n",
        static_cast<unsigned>(status.active_requests),
        static_cast<unsigned>(status.queued_requests),
        static_cast<unsigned long long>(status.downloads_succeeded),
        static_cast<unsigned long long>(status.downloads_failed));
    text += wxString::Format(L"Disk cache: %u tiles · %s",
                             static_cast<unsigned>(status.disk_tiles),
                             CacheSize(status.disk_bytes));
    if (!status.last_error.empty()) text += "\nLast bathymetry issue: " + status.last_error;
    if (!status.base_error.empty()) text += "\nLast base map issue: " + status.base_error;
    status_text->SetLabel(text);
  };

  refresh_status->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { update_status(); });
  clear_cache->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    const wxString result = overlay->ClearCache();
    update_status();
    wxMessageBox(result, "Online Charts", wxOK | wxICON_INFORMATION, &dialog);
  });
  save_area->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    // Apply the cache controls on screen to this action without closing the dialog.
    OnlineChartsSettings current = *settings;
    current.cache_enabled = cache_enabled->GetValue();
    current.cache_mode = static_cast<OnlineChartsCacheMode>(cache_mode->GetSelection());
    current.prefetch_levels = prefetch_levels->GetValue();
    overlay->Configure(current);
    const wxString result = overlay->SaveVisibleArea();
    update_status();
    wxMessageBox(result, "Online Charts", wxOK | wxICON_INFORMATION, &dialog);
  });

  /* ---------- Weather ---------- */
  const OnlineChartsWeatherSettings original_weather = *weather_settings;
  auto* forecast_box = new wxStaticBoxSizer(wxVERTICAL, weather_page, "Marine forecast (Open-Meteo)");
  auto* forecast = forecast_box->GetStaticBox();
  auto* weather_enabled = AddCheck(forecast, forecast_box,
                                   "Show wind, temperature and waves (Open-Meteo forecast)",
                                   weather_settings->enabled);
  auto* weather_panel = AddCheck(forecast, forecast_box, "Also show the conditions as a panel on the chart",
                                 weather_settings->show_panel);
  auto* weather_timeline = AddCheck(forecast, forecast_box,
                                    "Show the forecast time bar on the chart (play, drag, Now)",
                                    weather_settings->show_timeline);
  auto* wind_barb = AddCheck(forecast, forecast_box,
                             "Show the wind arrow at the boat (maritime wind barb, in knots)",
                             weather_settings->wind_barb);
  auto* wind_field = AddCheck(forecast, forecast_box, "Show a wind field across the whole chart",
                              weather_settings->wind_field);
  auto* wave_field = AddCheck(forecast, forecast_box, "Show wave arrows across the chart",
                              weather_settings->wave_field);
  auto* current_field = AddCheck(forecast, forecast_box,
                                 "Show ocean current arrows in red (across the chart and at the boat)",
                                 weather_settings->current_field);
  wxArrayString models;
  for (int i = 0; i < OnlineChartsWeather::ModelCount(); ++i) {
    models.Add(wxString::FromUTF8(OnlineChartsWeather::ModelName(i)));
  }
  auto* weather_model = AddChoice(forecast, forecast_box, "Weather model", models,
                                  weather_settings->model);
  wxArrayString temp_units;
  temp_units.Add(L"Celsius (\u00b0C)");
  temp_units.Add(L"Fahrenheit (\u00b0F)");
  auto* temp_choice = AddChoice(forecast, forecast_box, "Air temperature units", temp_units,
                                weather_settings->temp_fahrenheit ? 1 : 0);
  wxArrayString wave_units;
  wave_units.Add("Metres (m)");
  wave_units.Add("Feet (ft)");
  auto* wave_choice = AddChoice(forecast, forecast_box, "Wave height units", wave_units,
                                weather_settings->wave_feet ? 1 : 0);

  auto* hour_row = new wxBoxSizer(wxHORIZONTAL);
  hour_row->Add(new wxStaticText(forecast, wxID_ANY, "Forecast time"), 0,
                wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
  auto* hour_slider = new wxSlider(forecast, wxID_ANY, weather_settings->forecast_hour, 0, 72,
                                   wxDefaultPosition, wxSize(260, -1));
  hour_row->Add(hour_slider, 1, wxALIGN_CENTER_VERTICAL);
  auto* hour_label = new wxStaticText(forecast, wxID_ANY, ForecastHourLabel(weather_settings->forecast_hour),
                                      wxDefaultPosition, wxSize(150, -1));
  hour_row->Add(hour_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
  forecast_box->Add(hour_row, 0, wxALL | wxEXPAND, 6);
  weather_sizer->Add(forecast_box, 0, wxALL | wxEXPAND, 8);

  auto* weather_cache_box = new wxStaticBoxSizer(wxVERTICAL, weather_page, "Weather cache");
  auto* weather_cache = weather_cache_box->GetStaticBox();
  auto* weather_grid = new wxFlexGridSizer(2, 2, 6, 12);
  weather_grid->AddGrowableCol(1, 1);
  weather_grid->Add(new wxStaticText(weather_cache, wxID_ANY, "Reuse a reading without a new request for (minutes)"),
                    0, wxALIGN_CENTER_VERTICAL);
  auto* fresh_minutes = new wxSpinCtrl(weather_cache, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                       wxDefaultSize, wxSP_ARROW_KEYS, 10, 360,
                                       weather_settings->fresh_minutes);
  weather_grid->Add(fresh_minutes, 0, wxEXPAND);
  weather_grid->Add(new wxStaticText(weather_cache, wxID_ANY, "Keep readings for offline use (hours)"), 0,
                    wxALIGN_CENTER_VERTICAL);
  auto* keep_hours = new wxSpinCtrl(weather_cache, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxDefaultSize, wxSP_ARROW_KEYS, 1, 72,
                                    weather_settings->keep_hours);
  weather_grid->Add(keep_hours, 0, wxEXPAND);
  weather_cache_box->Add(weather_grid, 0, wxALL | wxEXPAND, 6);
  auto* weather_actions = new wxBoxSizer(wxHORIZONTAL);
  auto* refresh_weather = new wxButton(weather_cache, wxID_ANY, "Refresh weather now");
  auto* clear_weather = new wxButton(weather_cache, wxID_ANY, "Clear weather cache");
  weather_actions->Add(refresh_weather, 0, wxRIGHT, 5);
  weather_actions->Add(clear_weather, 0);
  weather_cache_box->Add(weather_actions, 0, wxALL | wxEXPAND, 6);
  auto* weather_status = new wxStaticText(weather_cache, wxID_ANY, weather->StatusText());
  weather_status->SetMinSize(wxSize(620, 70));
  weather_cache_box->Add(weather_status, 0, wxALL | wxEXPAND, 6);
  auto* weather_note = new wxStaticText(
      weather_cache, wxID_ANY,
      "Readings are reused for the time above, then refreshed when online and every few hours.\n"
      "Without a connection the last reading is kept up to 72 hours, labelled with its age.\n"
      "Weather data by Open-Meteo.com (CC BY 4.0). Not a substitute for an official marine forecast.");
  weather_cache_box->Add(weather_note, 0, wxALL | wxEXPAND, 6);
  weather_sizer->Add(weather_cache_box, 0, wxALL | wxEXPAND, 8);

  auto weather_from_controls = [&]() {
    OnlineChartsWeatherSettings current = *weather_settings;
    current.enabled = weather_enabled->GetValue();
    current.show_panel = weather_panel->GetValue();
    current.show_timeline = weather_timeline->GetValue();
    current.wind_barb = wind_barb->GetValue();
    current.wind_field = wind_field->GetValue();
    current.wave_field = wave_field->GetValue();
    current.current_field = current_field->GetValue();
    current.model = std::max(0, weather_model->GetSelection());
    current.temp_fahrenheit = temp_choice->GetSelection() == 1;
    current.wave_feet = wave_choice->GetSelection() == 1;
    current.forecast_hour = hour_slider->GetValue();
    current.fresh_minutes = fresh_minutes->GetValue();
    current.keep_hours = keep_hours->GetValue();
    return current;
  };
  // The forecast time moves the chart's weather straight away.
  hour_slider->Bind(wxEVT_SLIDER, [&](wxCommandEvent&) {
    hour_label->SetLabel(ForecastHourLabel(hour_slider->GetValue()));
    weather->SetForecastHour(hour_slider->GetValue());
    RequestRefresh(GetOCPNCanvasWindow());
  });
  refresh_weather->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    weather->Configure(weather_from_controls());
    weather->RefreshNow();
    weather_status->SetLabel(weather->StatusText());
  });
  clear_weather->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    const wxString result = weather->ClearCache();
    weather_status->SetLabel(weather->StatusText());
    wxMessageBox(result, "Online Charts", wxOK | wxICON_INFORMATION, &dialog);
  });
  wxTimer status_timer(&dialog);
  dialog.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
    weather_status->SetLabel(weather->StatusText());
    if (ais_status) ais_state->SetLabel(ais_status());
  });
  status_timer.Start(2000);

  chart_page->SetSizer(chart_sizer);
  weather_page->SetSizer(weather_sizer);
  cache_page->SetSizer(cache_sizer);
  notebook->AddPage(chart_page, "Chart");
  notebook->AddPage(weather_page, "Weather");
  notebook->AddPage(cache_page, "Offline cache and status");
  main->Add(notebook, 1, wxALL | wxEXPAND, 8);

  auto* warning = new wxStaticText(
      &dialog, wxID_ANY,
      "Experimental overlay. Not for primary navigation. Check provider licences and terms.");
  main->Add(warning, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 8);
  main->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
            wxALL | wxEXPAND, 8);

  dialog.SetSizer(main);
  update_status();
  dialog.Fit();
  dialog.SetMinSize(dialog.GetSize());
  dialog.CentreOnParent();

  const int result = dialog.ShowModal();
  status_timer.Stop();
  if (result != wxID_OK) {
    // "Save visible area", "Refresh weather now" and the forecast time apply
    // while the dialog is open; cancelling must not leave them active.
    overlay->Configure(*settings);
    weather->Configure(original_weather);
    RequestRefresh(GetOCPNCanvasWindow());
    return false;
  }
  *weather_settings = weather_from_controls();
  settings->overlay_enabled = enabled->GetValue();
  settings->show_contours = contours->GetValue();
  settings->show_soundings = soundings->GetValue();
  settings->show_shading = shading->GetValue();
  settings->show_status = chart_status->GetValue();
  settings->show_base_map = base_map->GetValue();
  settings->show_seamarks = seamarks->GetValue();
  settings->base_map_provider = base_map_provider->GetSelection() == 1 ? 1 : 0;
  settings->base_map_opacity = base_map_opacity->GetValue();
  settings->depth_colours = depth_colours->GetSelection() == 1 ? 1 : 0;
  settings->depth_opacity = depth_opacity->GetValue();
  settings->ais_demo = ais_demo->GetValue();
  settings->depth_feet = units->GetSelection() == 1;
  settings->label_scale = depth_label_size->GetValue();
  settings->cache_enabled = cache_enabled->GetValue();
  const int mode = cache_mode->GetSelection();
  settings->cache_mode = mode >= 0 && mode <= 2
                             ? static_cast<OnlineChartsCacheMode>(mode)
                             : OnlineChartsCacheMode::kNetworkFirst;
  settings->cache_max_mb = max_mb->GetValue();
  settings->cache_max_age_days = max_age->GetValue();
  settings->prefetch_levels = prefetch_levels->GetValue();
  return true;
}
