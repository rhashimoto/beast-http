#include "WebServer.hpp"

struct MyWebServer : public WebServer::BasicServer {
#if BOOST_VERSION >= 106600
  MyWebServer(boost::asio::io_context& io)
    : WebServer::BasicServer(io) {}
#else
  MyWebServer(boost::asio::io_service& io)
    : WebServer::BasicServer(io) {}
#endif
  
  virtual void handleRequest(
    const WebServer::Request& request,
    WebServer::Response& response,
    const std::function<void(const boost::system::error_code& ec)>& complete) const
    {
      if (!request.body().empty())
        BOOST_LOG_TRIVIAL(info) << "request payload: " << request.body();
      
      response.result(boost::beast::http::status::ok);
      response.set(boost::beast::http::field::content_type, "text/plain");
      async_write(
        response,
        boost::asio::buffer("how now brown cow", 17),
        [=](const boost::system::error_code& ec, size_t size) {
          complete(ec);
        });
    }
};

int main() {
#if BOOST_VERSION >= 106600
  boost::asio::io_context io;
#else
  boost::asio::io_service io;
#endif
  MyWebServer server(io);

  server.start("0.0.0.0", 8080);
  io.run();
  
  return 0;
}
