// Smoke test for the native shell's session client: connects to a host core,
// sends hello, and prints the stats it collects for a few seconds.
#include <windows.h>

#include <cstdio>
#include <string>

#include "session_client.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::printf("usage: ws_smoke <host> <port> <code>\n");
    return 2;
  }
  molink::ui::session_stats stats;
  molink::net::session_endpoint endpoint;
  endpoint.host = argv[1];
  endpoint.port = std::atoi(argv[2]);
  endpoint.token = argv[3];
  endpoint.direct_session = true;
  molink::net::session_client client;
  std::string error;
  if (!client.start(endpoint.host, endpoint.port, endpoint, &stats, &error)) {
    std::printf("connect failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("connected, hello sent\n");
  for (int i = 0; i < 6; ++i) {
    Sleep(1000);
    client.tick(GetTickCount64());
    std::wprintf(L"t=%d codec=%ls res=%ls kbps=%d rtt=%d\n", i + 1,
                 stats.codec.c_str(), stats.resolution.c_str(), stats.kbps,
                 stats.latency_ms);
  }
  client.stop();
  return 0;
}
