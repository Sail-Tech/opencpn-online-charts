#include "online_charts_http.h"

#include <utility>

#include <wx/log.h>

#if !wxUSE_WEBREQUEST
#include <curl/curl.h>
#endif

namespace {

constexpr size_t kMaxBodyBytes = 8 * 1024 * 1024;

#if wxUSE_WEBREQUEST
bool ReadBody(wxInputStream* input, std::string* body) {
  if (!input) return false;
  char buffer[16384];
  while (input->IsOk()) {
    input->Read(buffer, sizeof(buffer));
    const size_t count = input->LastRead();
    if (count == 0) break;
    if (body->size() + count > kMaxBodyBytes) return false;
    body->append(buffer, count);
  }
  return input->Eof() || input->IsOk();
}
#else
size_t CurlWrite(char* data, size_t size, size_t count, void* context) {
  const size_t length = size * count;
  auto* body = static_cast<std::string*>(context);
  if (body->size() + length > kMaxBodyBytes) return 0;
  body->append(data, length);
  return length;
}
#endif

}  // namespace

OnlineChartsHttp::OnlineChartsHttp(const wxString& user_agent)
#if wxUSE_WEBREQUEST
    : session_(wxWebSession::New()),
      user_agent_(user_agent)
#else
    : user_agent_(user_agent)
#endif
{
#if wxUSE_WEBREQUEST
  Bind(wxEVT_WEBREQUEST_STATE, &OnlineChartsHttp::OnState, this);
#else
  curl_global_init(CURL_GLOBAL_DEFAULT);
  multi_ = curl_multi_init();
  timer_.SetOwner(this);
  Bind(wxEVT_TIMER, &OnlineChartsHttp::OnTimer, this);
#endif
}

OnlineChartsHttp::~OnlineChartsHttp() {
  CancelAll();
#if wxUSE_WEBREQUEST
  Unbind(wxEVT_WEBREQUEST_STATE, &OnlineChartsHttp::OnState, this);
#else
  Unbind(wxEVT_TIMER, &OnlineChartsHttp::OnTimer, this);
  if (multi_) curl_multi_cleanup(static_cast<CURLM*>(multi_));
  multi_ = nullptr;
#endif
}

bool OnlineChartsHttp::Get(const wxString& url, Callback callback, const wxString& accept) {
  const int id = next_id_++;
  if (next_id_ > 1000000) next_id_ = 60000;
#if wxUSE_WEBREQUEST
  if (!session_.IsOpened()) return false;
  wxWebRequest request = session_.CreateRequest(this, url, id);
  if (!request.IsOk()) return false;
  request.SetHeader("Accept", accept);
  request.SetHeader("User-Agent", user_agent_);
  Pending pending;
  pending.callback = std::move(callback);
  pending.request = request;
  auto inserted = requests_.emplace(id, std::move(pending));
  inserted.first->second.request.Start();
  return true;
#else
  if (!multi_) return false;
  CURL* easy = curl_easy_init();
  if (!easy) return false;
  Pending pending;
  pending.callback = std::move(callback);
  pending.easy = easy;
  auto inserted = requests_.emplace(id, std::move(pending));
  Pending& stored = inserted.first->second;
  const std::string url_utf8 = url.ToUTF8().data();
  const std::string agent = user_agent_.ToUTF8().data();
  curl_easy_setopt(easy, CURLOPT_URL, url_utf8.c_str());
  curl_easy_setopt(easy, CURLOPT_USERAGENT, agent.c_str());
  const std::string accept_header = "Accept: " + std::string(accept.ToUTF8().data());
  stored.headers = curl_slist_append(nullptr, accept_header.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, static_cast<curl_slist*>(stored.headers));
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &CurlWrite);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &stored.body);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
  curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
  if (curl_multi_add_handle(static_cast<CURLM*>(multi_), easy) != CURLM_OK) {
    curl_easy_cleanup(easy);
    requests_.erase(inserted.first);
    return false;
  }
  if (!timer_.IsRunning()) timer_.Start(50);
  return true;
#endif
}

void OnlineChartsHttp::CancelAll() {
  auto pending = std::move(requests_);
  requests_.clear();
#if wxUSE_WEBREQUEST
  // Cancel() may emit an event synchronously; the map is already empty, so no
  // callback runs and nothing is erased while iterating.
  for (auto& entry : pending) entry.second.request.Cancel();
#else
  timer_.Stop();
  for (auto& entry : pending) {
    CURL* easy = static_cast<CURL*>(entry.second.easy);
    if (multi_ && easy) curl_multi_remove_handle(static_cast<CURLM*>(multi_), easy);
    if (easy) curl_easy_cleanup(easy);
    if (entry.second.headers) curl_slist_free_all(static_cast<curl_slist*>(entry.second.headers));
  }
#endif
}

void OnlineChartsHttp::Complete(int id, bool ok, long status) {
  auto found = requests_.find(id);
  if (found == requests_.end()) return;
  Pending pending = std::move(found->second);
  requests_.erase(found);
#if !wxUSE_WEBREQUEST
  CURL* easy = static_cast<CURL*>(pending.easy);
  if (multi_ && easy) curl_multi_remove_handle(static_cast<CURLM*>(multi_), easy);
  if (easy) curl_easy_cleanup(easy);
  if (pending.headers) curl_slist_free_all(static_cast<curl_slist*>(pending.headers));
#endif
  if (pending.callback) pending.callback(ok && status == 200, status, std::move(pending.body));
}

#if wxUSE_WEBREQUEST
void OnlineChartsHttp::OnState(wxWebRequestEvent& event) {
  auto found = requests_.find(event.GetId());
  if (found == requests_.end()) return;
  const wxWebRequest::State state = event.GetState();
  if (state != wxWebRequest::State_Completed && state != wxWebRequest::State_Failed &&
      state != wxWebRequest::State_Cancelled) {
    return;
  }
  long status = 0;
  bool ok = false;
  wxWebResponse response = event.GetResponse();
  if (response.IsOk()) status = response.GetStatus();
  // Read error responses too: a 429 body says which Open-Meteo limit was hit.
  if (response.IsOk() && state != wxWebRequest::State_Cancelled) {
    const bool read = ReadBody(response.GetStream(), &found->second.body);
    ok = state == wxWebRequest::State_Completed && read;
  }
  Complete(event.GetId(), ok, status);
}
#else
void OnlineChartsHttp::OnTimer(wxTimerEvent&) {
  if (!multi_) return;
  int running = 0;
  curl_multi_perform(static_cast<CURLM*>(multi_), &running);
  CURLMsg* message = nullptr;
  int remaining = 0;
  while ((message = curl_multi_info_read(static_cast<CURLM*>(multi_), &remaining))) {
    if (message->msg != CURLMSG_DONE) continue;
    void* data = nullptr;
    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &data);
    long status = 0;
    curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status);
    Complete(static_cast<int>(reinterpret_cast<intptr_t>(data)),
             message->data.result == CURLE_OK, status);
  }
  if (requests_.empty()) timer_.Stop();
}
#endif
