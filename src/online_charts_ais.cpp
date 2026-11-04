#include "online_charts_ais.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <wx/log.h>

#include "online_charts_http.h"

namespace {

constexpr char kStreamUrl[] = "https://ais.openwaters.io/v1/stream";
constexpr double kMaxArea = 99.0;           // Square degrees; anonymous access allows 100.
constexpr double kMargin = 0.25;            // The box reaches a quarter of the view beyond each side.
constexpr double kMaxPositionAge = 600.0;   // Seconds; OpenCPN drops a target after 10 minutes.
constexpr int kRetryMs[] = {2000, 5000, 15000, 30000, 60000};
constexpr std::time_t kMinConnectInterval = 4;  // Seconds; the server allows 20 connects a minute.

double Area(const std::array<double, 4>& b) { return (b[2] - b[0]) * (b[3] - b[1]); }

}  // namespace

OnlineChartsAis::OnlineChartsAis(Sink sink)
    : sink_(std::move(sink)),
      http_(std::make_unique<OnlineChartsHttp>(
          "OnlineCharts-OpenCPN/1.6.0 (+https://github.com/Sail-Tech/opencpn-online-charts)")) {
  timer_.SetOwner(this);
  Bind(wxEVT_TIMER, &OnlineChartsAis::OnTimer, this);
}

OnlineChartsAis::~OnlineChartsAis() {
  Shutdown();
  Unbind(wxEVT_TIMER, &OnlineChartsAis::OnTimer, this);
}

void OnlineChartsAis::Shutdown() {
  shutdown_ = true;
  enabled_ = false;
  timer_.Stop();
  if (http_) http_->CancelAll();
  stream_id_ = 0;
}

void OnlineChartsAis::SetEnabled(bool enabled) {
  if (shutdown_ || enabled == enabled_) return;
  enabled_ = enabled;
  if (!enabled_) {
    timer_.Stop();
    if (stream_id_) http_->Cancel(stream_id_);
    stream_id_ = 0;
    state_ = "Off";
    wxLogMessage("Online Charts: AIS demo off");
    return;
  }
  attempt_ = 0;
  state_ = has_view_ ? "Connecting" : "Waiting for the chart";
  wxLogMessage("Online Charts: AIS demo on (Open Waters AIS)");
  if (has_view_) Schedule(200);
}

void OnlineChartsAis::SetView(double south, double west, double north, double east) {
  if (!std::isfinite(south) || !std::isfinite(west) || !std::isfinite(north) ||
      !std::isfinite(east)) {
    return;
  }
  // Across the antimeridian, keep the wider side.
  if (west > east) {
    if (180.0 - west >= east + 180.0) {
      east = 180.0;
    } else {
      west = -180.0;
    }
  }
  view_ = {std::min(south, north), west, std::max(south, north), east};
  has_view_ = true;
  if (!enabled_ || shutdown_) return;
  // Wait until the chart stops moving before subscribing to a new area.
  if (NeedsNewArea() && !timer_.IsRunning()) Schedule(1500);
}

bool OnlineChartsAis::NeedsNewArea() const {
  if (!stream_id_) return true;
  // The view's centre has left the middle of the box, or the view is much larger or smaller.
  const double c_lat = (view_[0] + view_[2]) / 2, c_lon = (view_[1] + view_[3]) / 2;
  const double h_lat = (box_[2] - box_[0]) * 0.3, h_lon = (box_[3] - box_[1]) * 0.3;
  const double m_lat = (box_[0] + box_[2]) / 2, m_lon = (box_[1] + box_[3]) / 2;
  if (std::fabs(c_lat - m_lat) > h_lat || std::fabs(c_lon - m_lon) > h_lon) return true;
  const double ratio = Area(view_) / std::max(1e-9, Area(box_view_));
  return ratio > 2.5 || ratio < 1.0 / 6.0;
}

void OnlineChartsAis::Schedule(int milliseconds) {
  timer_.StartOnce(std::max(50, milliseconds));
}

void OnlineChartsAis::OnTimer(wxTimerEvent&) {
  if (!enabled_ || shutdown_ || !has_view_) return;
  if (stream_id_ && !NeedsNewArea()) return;
  const std::time_t now = std::time(nullptr);
  if (now - last_connect_ < kMinConnectInterval) {
    Schedule(static_cast<int>((kMinConnectInterval - (now - last_connect_)) * 1000));
    return;
  }
  Connect();
}

void OnlineChartsAis::Connect() {
  const double lat_margin = (view_[2] - view_[0]) * kMargin;
  const double lon_margin = (view_[3] - view_[1]) * kMargin;
  std::array<double, 4> box{};
  if (!online_charts::AisBox(view_[0] - lat_margin, view_[1] - lon_margin, view_[2] + lat_margin,
                             view_[3] + lon_margin, kMaxArea, &box)) {
    return;
  }
  if (stream_id_) http_->Cancel(stream_id_);
  stream_id_ = 0;
  parser_.Reset();
  welcomed_ = false;
  box_ = box;
  box_view_ = view_;
  last_connect_ = std::time(nullptr);
  // Each number is written alone, with any comma decimal (from OpenCPN's
  // locale) turned into a point.
  wxString url = wxString(kStreamUrl) + "?bbox=";
  for (int i = 0; i < 4; ++i) {
    wxString part = wxString::Format("%.4f", box[i]);
    part.Replace(",", ".");
    url += (i ? "," : "") + part;
  }
  url += "&snapshot=1";
  state_ = "Connecting";
  stream_id_ = http_->Stream(
      url, [this](const std::string& chunk) { OnData(chunk); },
      [this](bool ok, long status, std::string) { OnDone(ok, status); });
  if (!stream_id_) {
    last_error_ = "Could not start the HTTPS request";
    OnDone(false, 0);
    return;
  }
  wxLogMessage("Online Charts: AIS demo subscribed to %.3f,%.3f,%.3f,%.3f", box[0], box[1], box[2],
               box[3]);
}

void OnlineChartsAis::OnData(const std::string& chunk) {
  for (const auto& data : parser_.Feed(chunk)) HandleEvent(data);
}

void OnlineChartsAis::HandleEvent(const std::string& data) {
  online_charts::AisEvent event;
  if (!online_charts::ParseAisEvent(data, &event, &sequence_)) return;
  if (event.type == "welcome") {
    welcomed_ = true;
    attempt_ = 0;
    state_ = "Live";
    last_error_.clear();
    return;
  }
  if (event.type == "error") {
    last_error_ = wxString::FromUTF8(event.error.c_str());
    wxLogWarning("Online Charts: AIS demo: Open Waters error: %s", last_error_);
    return;
  }
  if (event.type != "event") return;
  ++events_;
  // The snapshot starts with each vessel's last report, some hours old.
  // OpenCPN dates a target from when it hears it, so old positions are skipped;
  // static data (names, dimensions) is kept.
  if (event.position && event.time > 0 &&
      static_cast<double>(std::time(nullptr)) - event.time > kMaxPositionAge) {
    ++stale_;
    return;
  }
  for (const auto& part : online_charts::AttributionParts(event.attribution)) credits_.insert(part);
  for (const auto& sentence : event.sentences) {
    sink_(wxString::FromUTF8(sentence.c_str()));
    ++sentences_;
  }
}

void OnlineChartsAis::OnDone(bool ok, long status) {
  (void)ok;
  stream_id_ = 0;
  if (!enabled_ || shutdown_) return;
  if (status == 429) {
    last_error_ = "Open Waters refused the connection (HTTP 429, too many requests)";
  } else if (status >= 400) {
    last_error_ = wxString::Format("Open Waters answered HTTP %ld", status);
  } else if (!welcomed_ && last_error_.empty()) {
    last_error_ = "No connection to Open Waters AIS";
  }
  const int delay = kRetryMs[std::min<int>(attempt_, static_cast<int>(sizeof(kRetryMs) / sizeof(kRetryMs[0])) - 1)];
  ++attempt_;
  state_ = wxString::Format("Reconnecting in %d s", delay / 1000);
  wxLogMessage("Online Charts: AIS demo stream ended (HTTP %ld); retrying in %d s", status, delay / 1000);
  Schedule(delay);
}

std::vector<wxString> OnlineChartsAis::Credits() const {
  std::vector<wxString> lines;
  if (!enabled_) return lines;
  wxString line = "AIS demo (not for navigation): ";
  if (credits_.empty()) {
    line += "Open Waters AIS (https://openwaters.io/ais/)";
  } else {
    bool first = true;
    for (const auto& credit : credits_) {
      if (!first) line += L" · ";
      line += wxString::FromUTF8(credit.c_str());
      first = false;
    }
  }
  lines.push_back(line);
  return lines;
}

wxString OnlineChartsAis::StatusText() const {
  if (!enabled_) return "AIS demo: off.";
  wxString text = "AIS demo: " + state_;
  text += wxString::Format(L" · %llu events received, %llu sentences sent to OpenCPN, %llu old positions skipped",
                           events_, sentences_, stale_);
  if (stream_id_) {
    text += wxString::Format(L"\nArea: %.2f–%.2f° lat, %.2f–%.2f° lon", box_[0], box_[2],
                             box_[1], box_[3]);
  }
  if (!last_error_.empty()) text += "\nLast issue: " + last_error_;
  return text;
}
