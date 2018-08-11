#include "WebServer.hpp"

struct MyWebServer : public WebServer::BasicServer {
  MyWebServer(boost::asio::io_service& io)
    : WebServer::BasicServer(io) {}
  
  virtual void doResponse(
    WebServer::Parser& parser,
    WebServer::Response& response,
    const std::function<void(const boost::system::error_code& ec)>& handler) const
    {
      response.result(boost::beast::http::status::ok);
      response.set(boost::beast::http::field::content_type, "text/plain");
      async_write(
        response,
        boost::asio::buffer("how now brown cow", 17),
        [=](const boost::system::error_code& ec, size_t size) {
          BOOST_LOG_TRIVIAL(info) << "async_write handler " << ec.message() << " " << size;
          handler(ec);
        });
    }
};

int main() {
  boost::asio::io_service io;
  MyWebServer server(io);

  server.start("0.0.0.0", 8080);
  io.run();
  
  return 0;
}
