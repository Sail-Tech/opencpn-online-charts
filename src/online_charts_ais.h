#ifndef ONLINE_CHARTS_AIS_H_
#define ONLINE_CHARTS_AIS_H_

#include <array>
#include <ctime>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include "online_charts_aisdata.h"

class OnlineChartsHttp;

// AIS demo: live vessels in the chart view from Open Waters AIS
// (https://openwaters.io/ais/), received as server-sent events over HTTPS and
// handed to OpenCPN as NMEA sentences, so they show as OpenCPN's own AIS
// targets. Anonymous access: one stream, an area of up to 100 square degrees
// and about 20 events a second. Runs on the GUI thread.
class OnlineChartsAis : public wxEvtHandler {
 public:
  using Sink = std::function<void(const wxString& sentence)>;

  explicit OnlineChartsAis(Sink sink);
  ~OnlineChartsAis() override;

  void SetEnabled(bool enabled);
  bool Enabled() const { return enabled_; }
  // The chart's visible area; the subscription follows it.
  void SetView(double south, double west, double north, double east);
  void Shutdown();

  // Attribution of the sources heard so far, for the chart.
  std::vector<wxString> Credits() const;
  wxString StatusText() const;

 private:
  void Connect();
  void Schedule(int milliseconds);
  void OnTimer(wxTimerEvent& event);
  void OnData(const std::string& chunk);
  void OnDone(bool ok, long status);
  void HandleEvent(const std::string& data);
  bool NeedsNewArea() const;

  Sink sink_;
  std::unique_ptr<OnlineChartsHttp> http_;
  wxTimer timer_;
  online_charts::SseParser parser_;
  bool enabled_ = false;
  bool shutdown_ = false;
  int stream_id_ = 0;
  bool welcomed_ = false;
  int attempt_ = 0;
  std::time_t last_connect_ = 0;
  bool has_view_ = false;
  std::array<double, 4> view_{};      // south, west, north, east
  std::array<double, 4> box_{};       // Subscribed: minLat, minLon, maxLat, maxLon.
  std::array<double, 4> box_view_{};  // The view the subscription was made for.
  int sequence_ = 0;
  std::set<std::string> credits_;
  unsigned long long events_ = 0;
  unsigned long long sentences_ = 0;
  unsigned long long stale_ = 0;
  wxString state_ = "Off";
  wxString last_error_;
};

#endif  // ONLINE_CHARTS_AIS_H_
