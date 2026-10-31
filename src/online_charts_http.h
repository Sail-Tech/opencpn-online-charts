#ifndef ONLINE_CHARTS_HTTP_H_
#define ONLINE_CHARTS_HTTP_H_

#include <functional>
#include <map>
#include <string>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/webrequest.h>
#if !wxUSE_WEBREQUEST
#include <wx/timer.h>
#endif

// Asynchronous HTTPS GET on the GUI thread: wxWebRequest where wxWidgets has
// it (macOS URLSession, Windows WinHTTP) and libcurl otherwise (Debian builds
// of wxWidgets disable wxWebRequest). Callbacks run on the GUI thread.
class OnlineChartsHttp : public wxEvtHandler {
 public:
  using Callback = std::function<void(bool ok, long status, std::string body)>;

  explicit OnlineChartsHttp(const wxString& user_agent);
  ~OnlineChartsHttp() override;

  // Returns false when the request could not be started; the callback is not
  // called in that case.
  bool Get(const wxString& url, Callback callback,
           const wxString& accept = "application/json");
  // Cancels every request without calling its callback.
  void CancelAll();
  size_t Active() const { return requests_.size(); }

 private:
  struct Pending {
    Callback callback;
    std::string body;
#if wxUSE_WEBREQUEST
    wxWebRequest request;
#else
    void* easy = nullptr;
    void* headers = nullptr;  // curl_slist with the Accept header.
#endif
  };

  void Complete(int id, bool ok, long status);
#if wxUSE_WEBREQUEST
  void OnState(wxWebRequestEvent& event);
  wxWebSession session_;
#else
  void OnTimer(wxTimerEvent& event);
  void* multi_ = nullptr;
  wxTimer timer_;
#endif
  wxString user_agent_;
  std::map<int, Pending> requests_;
  int next_id_ = 60000;
};

#endif  // ONLINE_CHARTS_HTTP_H_
