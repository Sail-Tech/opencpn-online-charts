#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "online_charts_server.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: server_smoke DASHBOARD_DIRECTORY\n";
    return 2;
  }
  OnlineChartsServer server(argv[1]);
  std::string error;
  if (!server.Start(&error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "READY " << server.Url() << '\n' << std::flush;
  for (int i = 0; i < 30; ++i) {
    server.Broadcast(
        "{\"lat\":50.775,\"lon\":-1.3,\"sog\":5.2,\"cog\":45,"
        "\"source\":\"OpenCPN\",\"quality\":\"GPS\"}");
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  server.Stop();
  return 0;
}
