#ifndef ONLINE_CHARTS_SERVER_H_
#define ONLINE_CHARTS_SERVER_H_

#include <mutex>
#include <set>
#include <string>

struct mg_context;
struct mg_connection;

class OnlineChartsServer {
 public:
  explicit OnlineChartsServer(std::string document_root);
  ~OnlineChartsServer();

  OnlineChartsServer(const OnlineChartsServer&) = delete;
  OnlineChartsServer& operator=(const OnlineChartsServer&) = delete;

  bool Start(std::string* error = nullptr);
  void Stop();
  void Broadcast(const std::string& json);

  bool running() const;
  int port() const { return port_; }
  std::string Url() const;
  std::size_t ClientCount();

 private:
  static int BeginRequest(mg_connection* connection);
  static int WebSocketConnect(const mg_connection* connection, void* user_data);
  static void WebSocketReady(mg_connection* connection, void* user_data);
  static int WebSocketData(mg_connection* connection, int bits, char* data,
                           size_t length, void* user_data);
  static void WebSocketClose(const mg_connection* connection, void* user_data);

  bool AllowsLocalHost(const mg_connection* connection) const;
  bool AllowsLocalOrigin(const mg_connection* connection) const;
  int SendSimpleResponse(mg_connection* connection, int status,
                         const std::string& content_type,
                         const std::string& body) const;

  std::string document_root_;
  mg_context* context_ = nullptr;
  int port_ = 0;
  mutable std::mutex mutex_;
  std::set<mg_connection*> clients_;
  std::string latest_fix_;
  bool stopping_ = false;
};

#endif  // ONLINE_CHARTS_SERVER_H_
