#ifndef ONLINE_CHARTS_PREFERENCES_H_
#define ONLINE_CHARTS_PREFERENCES_H_

#include "online_charts_overlay.h"
#include "online_charts_weather.h"

class wxWindow;

// Shows the native OpenCPN preferences dialog. Returns true when the user
// accepts changes; cache actions, weather refresh and the forecast hour act
// immediately while the dialog is open.
bool ShowOnlineChartsPreferences(wxWindow* parent, OnlineChartsSettings* settings,
                                 OnlineChartsOverlay* overlay,
                                 OnlineChartsWeatherSettings* weather_settings,
                                 OnlineChartsWeather* weather);

#endif  // ONLINE_CHARTS_PREFERENCES_H_
