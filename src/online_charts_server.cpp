#include "online_charts_server.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "civetweb.h"

namespace {
constexpr int kFirstPort = 8766;
constexpr int kLastPort = 8775;

const char* Header(const mg_connection* connection, const char* name) {
  const char* value = mg_get_header(connection, name);
  return value ? value : "";
}

std::string PortString(int port) {
  return std::to_string(port);
}
}  // namespace

OnlineChartsServer::OnlineChartsServer(std::string document_root)
    : document_root_(std::move(document_root)) {}

OnlineChartsServer::~OnlineChartsServer() { Stop(); }

bool OnlineChartsServer::Start(std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (context_) return true;
  if (document_root_.empty()) {
    if (error) *error = "The dashboard data directory was not found.";
    return false;
  }

  stopping_ = false;
  for (int candidate = kFirstPort; candidate <= kLastPort; ++candidate) {
    const std::string listening = "127.0.0.1:" + PortString(candidate);
    const char* options[] = {
        "document_root", document_root_.c_str(),
        "listening_ports", listening.c_str(),
        "num_threads", "4",
        "enable_keep_alive", "yes",
        "keep_alive_timeout_ms", "5000",
        "request_timeout_ms", "10000",
        "access_control_allow_origin", "",
        nullptr, nullptr};

    mg_callbacks callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = &OnlineChartsServer::BeginRequest;
    context_ = mg_start(&callbacks, this, options);
    if (context_) {
      port_ = candidate;
      mg_set_websocket_handler(context_, "/gps",
                               &OnlineChartsServer::WebSocketConnect,
                               &OnlineChartsServer::WebSocketReady,
                               &OnlineChartsServer::WebSocketData,
                               &OnlineChartsServer::WebSocketClose, this);
      return true;
    }
  }
  if (error) {
    *error = "Could not bind a loopback port in the range 8766-8775. "
             "Close another local service using these ports and try again.";
  }
  return false;
}

void OnlineChartsServer::Stop() {
  mg_context* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_) return;
    stopping_ = true;
    context = context_;
    context_ = nullptr;
  }
  mg_stop(context);
  std::lock_guard<std::mutex> lock(mutex_);
  clients_.clear();
  port_ = 0;
  stopping_ = false;
}

bool OnlineChartsServer::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_ != nullptr;
}

std::string OnlineChartsServer::Url() const {
  return port_ ? "http://127.0.0.1:" + PortString(port_) : std::string();
}

std::size_t OnlineChartsServer::ClientCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return clients_.size();
}

void OnlineChartsServer::Broadcast(const std::string& json) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_fix_ = json;
  if (!context_ || stopping_) return;
  for (auto it = clients_.begin(); it != clients_.end();) {
    const int written = mg_websocket_write(*it, MG_WEBSOCKET_OPCODE_TEXT,
                                           json.data(), json.size());
    if (written < 0) {
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

bool OnlineChartsServer::AllowsLocalHost(const mg_connection* connection) const {
  const std::string host = Header(connection, "Host");
  const std::string suffix = ":" + PortString(port_);
  return host == "127.0.0.1" + suffix || host == "localhost" + suffix;
}

bool OnlineChartsServer::AllowsLocalOrigin(
    const mg_connection* connection) const {
  const std::string origin = Header(connection, "Origin");
  if (origin.empty()) return true;
  const std::string suffix = ":" + PortString(port_);
  return origin == "http://127.0.0.1" + suffix ||
         origin == "http://localhost" + suffix;
}

int OnlineChartsServer::SendSimpleResponse(mg_connection* connection, int status,
                                           const std::string& content_type,
                                           const std::string& body) const {
  mg_printf(connection,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "Connection: close\r\n\r\n%s",
            status, status == 200 ? "OK" : (status == 302 ? "Found" : "Forbidden"),
            content_type.c_str(), body.size(), body.c_str());
  return status;
}

int OnlineChartsServer::BeginRequest(mg_connection* connection) {
  auto* server = static_cast<OnlineChartsServer*>(
      mg_get_user_context_data(connection));
  if (!server) return 0;
  if (!server->AllowsLocalHost(connection)) {
    return server->SendSimpleResponse(connection, 403, "text/plain",
                                      "Forbidden: loopback Host required\n");
  }

  const mg_request_info* request = mg_get_request_info(connection);
  const std::string uri = request && request->request_uri ? request->request_uri : "";
  if (uri == "/") {
    const std::string location = "Location: /dashboard.html\r\n";
    mg_printf(connection,
              "HTTP/1.1 302 Found\r\n%sContent-Length: 0\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
              location.c_str());
    return 302;
  }
  if (uri == "/api/status") {
    const std::string body =
        "{\"name\":\"online_charts_pi\",\"port\":" +
        std::to_string(server->port_) +
        ",\"websocket\":\"/gps\",\"clients\":" +
        std::to_string(server->ClientCount()) + "}\n";
    return server->SendSimpleResponse(connection, 200, "application/json", body);
  }
  return 0;
}

int OnlineChartsServer::WebSocketConnect(const mg_connection* connection,
                                         void* user_data) {
  auto* server = static_cast<OnlineChartsServer*>(user_data);
  return server && server->AllowsLocalOrigin(connection) ? 0 : 1;
}

void OnlineChartsServer::WebSocketReady(mg_connection* connection,
                                        void* user_data) {
  auto* server = static_cast<OnlineChartsServer*>(user_data);
  if (!server) return;
  std::lock_guard<std::mutex> lock(server->mutex_);
  if (server->stopping_) return;
  server->clients_.insert(connection);
  if (!server->latest_fix_.empty()) {
    mg_websocket_write(connection, MG_WEBSOCKET_OPCODE_TEXT,
                       server->latest_fix_.data(), server->latest_fix_.size());
  }
}

int OnlineChartsServer::WebSocketData(mg_connection*, int, char*, size_t,
                                      void*) {
  // The dashboard is receive-only; ignore browser messages and keep the socket.
  return 1;
}

void OnlineChartsServer::WebSocketClose(const mg_connection* connection,
                                        void* user_data) {
  auto* server = static_cast<OnlineChartsServer*>(user_data);
  if (!server) return;
  std::lock_guard<std::mutex> lock(server->mutex_);
  server->clients_.erase(const_cast<mg_connection*>(connection));
}
