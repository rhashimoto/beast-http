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
  template<typename Stream>
  class Response : public boost::beast::http::response<detail::ResponseBody> {
    std::shared_ptr<Stream> stream_;
    boost::beast::http::response_serializer<body_type, fields_type> serializer_;

  public:
    Response(const std::shared_ptr<Stream>& stream)
      : boost::beast::http::response<detail::ResponseBody>()
      , stream_(stream)
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
        [=, handler = std::forward<WriteHandler>(handler)](boost::system::error_code ec, std::size_t size) mutable {
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
        [this, handler](boost::system::error_code ec, std::size_t size) mutable {
          BOOST_LOG_TRIVIAL(info) << "async_finish handler " << size;
          handler(ec);
        });
    
      return result.get();
    }

    template<typename Token>
    friend auto async_finish(Response& response, Token&& token) {
      return response.async_finish(std::forward<Token>(token));
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
      BOOST_LOG_TRIVIAL(info) << "doAccept";
      auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_);
      acceptor_.async_accept(
        *socket,
        [this, socket](const boost::system::error_code& ec) mutable {
          BOOST_LOG_TRIVIAL(info) << "doAccept handler";
          if (ec)
            return fail(ec, "async_accept");
          handleAccept(socket);
        });
    }

    void handleAccept(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) {
      doAccept();

      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << "Accepting connection from " << endpoint.address().to_string();
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
      BOOST_LOG_TRIVIAL(info) << "doRead";
      auto request = std::make_shared<Request>();
      boost::beast::http::async_read(
        *stream, *buffer, *request,
        [this, stream, buffer, request](const boost::system::error_code& ec, size_t) {
          BOOST_LOG_TRIVIAL(info) << "async_read handler " << ec.message();
          if (ec) {
            if (ec != boost::beast::http::error::end_of_stream)
              fail(ec, "async_read");
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
      BOOST_LOG_TRIVIAL(info) << "handleRead";
      doRead(stream, buffer);

      auto response = std::make_shared<Response<Stream>>(stream);
      response->version(request->version());
      response->set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response->keep_alive(request->keep_alive());
      response->prepare_payload();

      doResponse(
        *request, *response,
        [this, request, response](const boost::system::error_code& ec) {
          BOOST_LOG_TRIVIAL(info) << "doResponse handler";
          if (ec)
            fail(ec, "doResponse");
          handleResponse(response);
        });
    }

    template<typename Stream>
    void handleResponse(const std::shared_ptr<Response<Stream>>& response) const {
      BOOST_LOG_TRIVIAL(info) << "handleResponse";
      response->async_finish(
        [this, response](const boost::system::error_code& ec) {
          BOOST_LOG_TRIVIAL(info) << "async_finish handler " << ec.message();
          if (ec)
            fail(ec, "async_finish");
        });
    }

    void doClose(const std::shared_ptr<boost::asio::ip::tcp::socket>& socket) const {
      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << "Closing connection from " << endpoint.address().to_string();
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
      WebServer::Response<boost::asio::ip::tcp::socket>& response,
      const std::function<void(const boost::system::error_code& ec)>& handler) const
    {
      BOOST_LOG_TRIVIAL(info) << "doResponse";
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

}

#endif // WebServer_H_
  
