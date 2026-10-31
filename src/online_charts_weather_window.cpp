#include "online_charts_weather_window.h"

#include <algorithm>
#include <utility>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace {

const int kGap = 8;

wxString Dash(const wxString& value) { return value.empty() ? wxString(L"—") : value; }

void SetLabelIfChanged(wxStaticText* text, const wxString& value) {
  if (text->GetLabel() != value) text->SetLabel(value);
}

}  // namespace

OnlineChartsWeatherWindow::OnlineChartsWeatherWindow(wxWindow* parent, OnlineChartsWeather* weather,
                                                     ApplyFn apply, CloseFn on_close)
    : wxFrame(parent, wxID_ANY, "Weather", wxDefaultPosition, wxDefaultSize,
              wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxFRAME_TOOL_WINDOW |
                  wxFRAME_FLOAT_ON_PARENT | wxFRAME_NO_TASKBAR),
      weather_(weather),
      apply_(std::move(apply)),
      on_close_(std::move(on_close)) {
  auto* panel = new wxPanel(this);
  auto* main = new wxBoxSizer(wxVERTICAL);

  place_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
  wxFont small = place_->GetFont();
  small.SetPointSize(std::max(8, small.GetPointSize() - 1));
  place_->SetFont(small);
  main->Add(place_, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);

  auto* grid = new wxGridSizer(2, kGap, kGap);
  wind_ = AddTile(panel, grid, "WIND");
  gusts_ = AddTile(panel, grid, "GUSTS");
  air_temp_ = AddTile(panel, grid, "AIR TEMP");
  pressure_ = AddTile(panel, grid, "PRESSURE");
  waves_ = AddTile(panel, grid, "WAVES");
  current_ = AddTile(panel, grid, "CURRENT");
  main->Add(grid, 0, wxALL | wxEXPAND, 10);

  when_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
  when_->SetFont(small);
  main->Add(when_, 0, wxLEFT | wxRIGHT | wxEXPAND, 10);
  status_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
  status_->SetFont(small);
  status_->SetForegroundColour(wxColour(110, 125, 132));
  main->Add(status_, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 10);

  auto add_check = [&](const wxString& label) {
    auto* check = new wxCheckBox(panel, wxID_ANY, label);
    main->Add(check, 0, wxLEFT | wxRIGHT | wxTOP, 10);
    check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });
    return check;
  };
  enabled_ = add_check("Wind, temperature and waves (Open-Meteo)");
  wind_barb_ = add_check("Wind arrow at the boat (barb, in knots)");
  wind_field_ = add_check("Wind field across the chart");
  wave_field_ = add_check("Wave arrows across the chart");
  current_field_ = add_check("Current arrows (red) on the chart");
  timeline_ = add_check("Forecast time bar on the chart");

  main->Add(new wxStaticText(panel, wxID_ANY, "Weather model"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
  model_ = new wxChoice(panel, wxID_ANY);
  for (int i = 0; i < OnlineChartsWeather::ModelCount(); ++i) {
    model_->Append(wxString::FromUTF8(OnlineChartsWeather::ModelName(i)));
  }
  model_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Apply(); });
  main->Add(model_, 0, wxALL | wxEXPAND, 10);

  panel->SetSizer(main);
  auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
  frame_sizer->Add(panel, 1, wxEXPAND);
  SetSizerAndFit(frame_sizer);
  SetMinSize(GetSize());

  // Closing only hides the window; the toolbar button shows it again.
  Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
    if (!event.CanVeto()) {
      event.Skip();
      return;
    }
    Hide();
    if (on_close_) on_close_();
  });
  timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdateValues(); });
  timer_.Start(500);

  Sync();
}

OnlineChartsWeatherWindow::~OnlineChartsWeatherWindow() { timer_.Stop(); }

wxStaticText* OnlineChartsWeatherWindow::AddTile(wxWindow* parent, wxSizer* grid,
                                                 const wxString& label) {
  // Fixed light colours, like the dashboard tiles, readable in light and dark mode.
  auto* tile = new wxPanel(parent);
  tile->SetBackgroundColour(wxColour(238, 243, 245));
  auto* sizer = new wxBoxSizer(wxVERTICAL);
  auto* name = new wxStaticText(tile, wxID_ANY, label);
  wxFont name_font = name->GetFont();
  name_font.SetPointSize(std::max(8, name_font.GetPointSize() - 2));
  name->SetFont(name_font);
  name->SetForegroundColour(wxColour(98, 116, 125));
  auto* value = new wxStaticText(tile, wxID_ANY, L"—");
  wxFont value_font = value->GetFont();
  value_font.SetPointSize(value_font.GetPointSize() + 3);
  value_font.SetWeight(wxFONTWEIGHT_BOLD);
  value->SetFont(value_font);
  value->SetForegroundColour(wxColour(20, 32, 40));
  value->SetMinSize(wxSize(118, -1));
  sizer->Add(name, 0, wxLEFT | wxRIGHT | wxTOP, 9);
  sizer->Add(value, 0, wxALL | wxEXPAND, 9);
  tile->SetSizer(sizer);
  grid->Add(tile, 0, wxEXPAND);
  return value;
}

void OnlineChartsWeatherWindow::Sync() {
  const OnlineChartsWeatherSettings& s = weather_->Settings();
  enabled_->SetValue(s.enabled);
  wind_barb_->SetValue(s.wind_barb);
  wind_field_->SetValue(s.wind_field);
  wave_field_->SetValue(s.wave_field);
  current_field_->SetValue(s.current_field);
  timeline_->SetValue(s.show_timeline);
  model_->SetSelection(s.model);
  UpdateValues();
}

void OnlineChartsWeatherWindow::Apply() {
  OnlineChartsWeatherSettings s = weather_->Settings();
  s.enabled = enabled_->GetValue();
  s.wind_barb = wind_barb_->GetValue();
  s.wind_field = wind_field_->GetValue();
  s.wave_field = wave_field_->GetValue();
  s.current_field = current_field_->GetValue();
  s.show_timeline = timeline_->GetValue();
  s.model = std::max(0, model_->GetSelection());
  if (apply_) apply_(s);
  UpdateValues();
}

void OnlineChartsWeatherWindow::UpdateValues() {
  if (!IsShown()) return;
  const OnlineChartsConditions c = weather_->Current();
  SetLabelIfChanged(place_, c.place);
  SetLabelIfChanged(wind_, Dash(c.wind));
  SetLabelIfChanged(gusts_, Dash(c.gusts));
  SetLabelIfChanged(air_temp_, Dash(c.air_temp));
  SetLabelIfChanged(pressure_, Dash(c.pressure));
  SetLabelIfChanged(waves_, Dash(c.waves));
  SetLabelIfChanged(current_, Dash(c.current));
  SetLabelIfChanged(when_, c.when);
  SetLabelIfChanged(status_, c.status);
}
