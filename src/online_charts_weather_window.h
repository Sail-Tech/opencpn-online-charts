#ifndef ONLINE_CHARTS_WEATHER_WINDOW_H_
#define ONLINE_CHARTS_WEATHER_WINDOW_H_

#include <functional>

#include <wx/frame.h>
#include <wx/timer.h>

#include "online_charts_weather.h"

class wxCheckBox;
class wxChoice;
class wxStaticText;

// Small floating window with the conditions at the forecast time and the
// weather choices, like the Conditions section of the browser dashboard. It is
// shown and hidden with the Weather toolbar button.
class OnlineChartsWeatherWindow : public wxFrame {
 public:
  using ApplyFn = std::function<void(const OnlineChartsWeatherSettings&)>;
  using CloseFn = std::function<void()>;

  OnlineChartsWeatherWindow(wxWindow* parent, OnlineChartsWeather* weather, ApplyFn apply,
                            CloseFn on_close);
  ~OnlineChartsWeatherWindow() override;

  // Copies the current weather settings into the controls.
  void Sync();

 private:
  wxStaticText* AddTile(wxWindow* parent, wxSizer* grid, const wxString& label);
  void UpdateValues();
  void Apply();

  OnlineChartsWeather* weather_;
  ApplyFn apply_;
  CloseFn on_close_;
  wxTimer timer_;

  wxStaticText* place_ = nullptr;
  wxStaticText* wind_ = nullptr;
  wxStaticText* gusts_ = nullptr;
  wxStaticText* air_temp_ = nullptr;
  wxStaticText* pressure_ = nullptr;
  wxStaticText* waves_ = nullptr;
  wxStaticText* current_ = nullptr;
  wxStaticText* when_ = nullptr;
  wxStaticText* status_ = nullptr;
  wxCheckBox* enabled_ = nullptr;
  wxCheckBox* wind_barb_ = nullptr;
  wxCheckBox* wind_field_ = nullptr;
  wxCheckBox* wave_field_ = nullptr;
  wxCheckBox* current_field_ = nullptr;
  wxCheckBox* timeline_ = nullptr;
  wxChoice* model_ = nullptr;
};

#endif  // ONLINE_CHARTS_WEATHER_WINDOW_H_
