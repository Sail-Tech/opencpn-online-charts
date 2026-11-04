#ifndef ONLINE_CHARTS_HTTP_H_
#define ONLINE_CHARTS_HTTP_H_

#include <functional>
#include <map>
#include <set>
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
  using DataCallback = std::function<void(const std::string& chunk)>;

  explicit OnlineChartsHttp(const wxString& user_agent);
  ~OnlineChartsHttp() override;

  // Returns false when the request could not be started; the callback is not
  // called in that case.
  bool Get(const wxString& url, Callback callback,
           const wxString& accept = "application/json");
  // A long-lived GET (server-sent events): on_data receives the body as it
  // arrives, then on_done runs once when the stream ends or fails (its body is
  // empty). Returns the request id, or 0 when it could not be started.
  int Stream(const wxString& url, DataCallback on_data, Callback on_done,
             const wxString& accept = "text/event-stream");
  // Cancels one request without calling its callbacks.
  void Cancel(int id);
  // Cancels every request without calling its callback.
  void CancelAll();
  size_t Active() const { return requests_.size(); }

 private:
  struct Pending {
    Callback callback;
    DataCallback on_data;  // Set for streams.
    std::string body;
#if wxUSE_WEBREQUEST
    wxWebRequest request;
#else
    void* easy = nullptr;
    void* headers = nullptr;  // curl_slist with the Accept header.
#endif
  };

  void Complete(int id, bool ok, long status);
  int Start(const wxString& url, Callback callback, DataCallback on_data, const wxString& accept);
#if wxUSE_WEBREQUEST
  void OnState(wxWebRequestEvent& event);
  void OnData(wxWebRequestEvent& event);
  void CancelRequest(int id, wxWebRequest& request);
  wxWebSession session_;
  // Cancelled requests whose end wxWidgets has not reported yet. URLSession
  // and WinHTTP report it from another thread, through this handler, so the
  // handler must outlive them.
  std::set<int> cancelling_;
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
