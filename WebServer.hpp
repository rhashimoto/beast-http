#ifndef WebServer_H_
#define WebServer_H_

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/beast.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "detail/WebServer_detail.hpp"

namespace WebServer {
  
  typedef boost::beast::http::request<boost::beast::http::string_body> StringRequest;

  // Response with AsyncWriteStream and SyncWriteStream support.
  class Response : public boost::beast::http::response<detail::ResponseBody> {
    std::unique_ptr<detail::StreamFacade> stream_;
    boost::beast::http::response_serializer<body_type, fields_type> serializer_;

  public:
    template<typename Stream>
    Response(Stream& stream)
      : boost::beast::http::response<detail::ResponseBody>()
      , stream_(new detail::StreamFacadeT<Stream>(stream))
      , serializer_(*this) {
    }

    boost::asio::io_service&
    get_io_service() {
      return stream_->get_io_service();
    }

    template<typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(
      const ConstBufferSequence& buffers,
      WriteHandler&& handler)
    {
      const auto buffersSize = boost::asio::buffer_size(buffers);
      BOOST_LOG_TRIVIAL(info) << "async_write_some " << buffersSize;
      for (const auto& buffer : buffers)
        body().buffers.emplace_back(buffer);
    
      async_write(
        *stream_,
        serializer_,
        [=](boost::system::error_code ec, std::size_t size) mutable {
          BOOST_LOG_TRIVIAL(info) << "async_write_some handler " << ec.message() << " " << size;
          if (!ec || ec == boost::beast::http::error::need_buffer) {
            body().buffers.clear();
            ec = {};
          }
          handler(ec, size);
        });
    }

    template<typename Token>
    auto async_finish(Token&& token) {
      BOOST_LOG_TRIVIAL(info) << "async_finish";
#if BOOST_VERSION >= 106600
      using result_type = typename boost::asio::async_result<std::decay_t<Token>, void(boost::system::error_code)>;
      typename result_type::completion_handler_type handler(std::forward<Token>(token));

      result_type result(handler);
#else
      typename boost::asio::handler_type<Token, void(boost::system::error_code)>::type
        handler(std::forward<Token>(token));

      boost::asio::async_result<decltype (handler)> result (handler);
#endif

      body().more = false;
      async_write(
        *stream_,
        serializer_, 
        [=](boost::system::error_code ec, std::size_t size) mutable {
          BOOST_LOG_TRIVIAL(info) << "async_finish handler " << size;
          handler(ec);
        });
    
      return result.get();
    }

    template<typename ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers) {
      boost::system::error_code ec;
      std::size_t result = write_some(buffers, ec);
      if (ec)
        throw boost::system::system_error(ec);
      return result;
    }
 
    template<typename ConstBufferSequence>
    std::size_t write_some(
      const ConstBufferSequence& buffers,
      boost::system::error_code & ec)
    {
      const auto buffersSize = boost::asio::buffer_size(buffers);
      BOOST_LOG_TRIVIAL(info) << "write_some " << buffersSize;
      for (const auto& buffer : buffers)
        body().buffers.emplace_back(buffer);
    

      const auto size = write(*stream_, serializer_, ec);
      if (!ec || ec == boost::beast::http::error::need_buffer) {
        body().buffers.clear();
        ec = {};
      }
      return buffersSize;
    }
  };
  
  template<typename Request>
  class BasicServer {
    boost::asio::io_service& io_;
    boost::asio::ip::tcp::acceptor acceptor_;

    void fail(const boost::system::error_code& ec, const std::string& context) const {
      BOOST_LOG_TRIVIAL(error) << context << ": " << ec.message();
    }
    
    void doAccept() {
      auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
      acceptor_.async_accept(
        *socket,
        [=](const boost::system::error_code& ec) mutable {
          if (ec)
            return fail(ec, "socket accept");
          handleAccept(socket);
        });
    }

    void handleAccept(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) {
      doAccept();

      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << boost::format("%x Accepting %s")
          % socket.get()
          % endpoint.address().to_string();
      }
      else
        fail(ec, "remote endpoint");
      
      doRead(socket, std::make_shared<boost::beast::flat_buffer>());
    }

    template<typename Stream>
    void doRead(
      const std::shared_ptr<Stream>& stream,
      const std::shared_ptr<boost::beast::flat_buffer>& buffer) const
    {
      auto request = std::make_shared<Request>();
      boost::beast::http::async_read(
        *stream, *buffer, *request,
        [=](const boost::system::error_code& ec, size_t) {
          if (ec) {
            if (ec != boost::beast::http::error::end_of_stream)
              fail(ec, "request");
            return doClose(stream);
          }
          handleRead(stream, buffer, request);
        });
    }

    template<typename Stream>
    void handleRead(
      const std::shared_ptr<Stream>& stream,
      const std::shared_ptr<boost::beast::flat_buffer>& buffer,
      const std::shared_ptr<Request>& request) const
    {
      doRead(stream, buffer);

      BOOST_LOG_TRIVIAL(info) << boost::format("%x %s %s")
        % stream.get()
        % request->method_string()
        % request->target();
      auto response = std::make_shared<Response>(*stream);
      response->version(request->version());
      response->set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response->keep_alive(request->keep_alive());
      response->prepare_payload();

      doResponse(
        *request, *response,
        [=](const boost::system::error_code& ec) {
          boost::ignore_unused(request);
          if (ec)
            fail(ec, "user handler");
          handleResponse(response);
        });
    }

    void handleResponse(const std::shared_ptr<Response>& response) const {
      response->async_finish(
        [=](const boost::system::error_code& ec) {
          boost::ignore_unused(response);
          BOOST_LOG_TRIVIAL(info) << "async_finish handler " << ec.message();
          if (ec)
            fail(ec, "response completion");
        });
    }

    void doClose(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) const {
      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << boost::format("%x Closing %s ")
          % socket.get()
          % endpoint.address().to_string();
      }
      else {
        fail(ec, "remote endpoint");
      }
        
      socket->shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
    }
    
  public:
    BasicServer(boost::asio::io_service& io)
      : io_(io)
      , acceptor_(io) {
    }

    virtual ~BasicServer() = default;
    
    virtual void start(const std::string& address, unsigned short port = 8080) {
      boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::address::from_string(address),
        port);
      acceptor_.open(endpoint.protocol());
      acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
      acceptor_.bind(endpoint);
      acceptor_.listen(boost::asio::socket_base::max_connections);
      
      BOOST_LOG_TRIVIAL(info) << boost::format("Listening on %s:%d...")
        % address
        % port;

      doAccept();
    }

    virtual void stop() {
    }

    virtual void doResponse(
      const Request& request,
      WebServer::Response& response,
      const std::function<void(const boost::system::error_code& ec)>& handler) const
    {
      response.result(boost::beast::http::status::not_found);
      handler(boost::system::error_code());
    }
  };

}

#endif // WebServer_H_
  
