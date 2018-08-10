#include "WebServer.hpp"

int main() {
  boost::asio::io_service io;
  WebServer::BasicServer<WebServer::StringRequest> server(io);

  server.start("0.0.0.0", 8080);
  io.run();
  
  return 0;
}
