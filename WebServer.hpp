#ifndef WebServer_H_
#define WebServer_H_

#include <chrono>
#include <thread>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/beast.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "detail/WebServer_detail.hpp"

namespace WebServer {
  
  class Parser : public boost::beast::http::request_parser<detail::RequestBody> {
    std::unique_ptr<detail::StreamAdapter> stream_;
    boost::beast::flat_buffer& buffer_;
    
  public:
    template<typename Stream>
    Parser(Stream& stream, boost::beast::flat_buffer& buffer)
      : boost::beast::http::request_parser<detail::RequestBody>()
      , stream_(new detail::StreamAdapterT<Stream>(stream))
      , buffer_(buffer)
    {
    }

    boost::asio::io_service&
    get_io_service() {
      return stream_->get_io_service();
    }

    template<typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(
      const MutableBufferSequence& buffers,
      ReadHandler&& handler)
    {
      const auto buffersSize = boost::asio::buffer_size(buffers);
      BOOST_LOG_TRIVIAL(info) << "async_read_some " << buffersSize;
      for (const auto& buffer : buffers)
        get().body().buffers.emplace_back(buffer);

      async_read(
        *stream_, buffer_, *this,
        [=](const boost::system::error_code& ec, std::size_t size) mutable {
          BOOST_LOG_TRIVIAL(info) << "async_read handler " << ec.message() << " " << size;
          handler(ec, size);
        });
    }

    template<typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers) {
      boost::system::error_code ec;
      std::size_t result = read_some(buffers, ec);
      if (ec)
        throw boost::system::system_error(ec);
      return result;
    }
    template<typename MutableBufferSequence>
    std::size_t read_some(
      const MutableBufferSequence& buffers,
      boost::system::error_code& ec) {
      const auto buffersSize = boost::asio::buffer_size(buffers);
      BOOST_LOG_TRIVIAL(info) << "read_some " << buffersSize;
      for (const auto& buffer : buffers)
        get().body().buffers.emplace_back(buffer);

      std::size_t result = read(*stream_, buffer_, *this, ec);
      if (!ec && result == 0)
        ec = boost::asio::error::eof;
      return result;
    } 
  };

  typedef Parser::value_type Request;
  
  // Response with AsyncWriteStream and SyncWriteStream support.
  class Response : public boost::beast::http::response<detail::ResponseBody> {
    std::unique_ptr<detail::StreamAdapter> stream_;
    boost::beast::http::response_serializer<body_type, fields_type> serializer_;

  public:
    template<typename Stream>
    Response(Stream& stream)
      : boost::beast::http::response<detail::ResponseBody>()
      , stream_(new detail::StreamAdapterT<Stream>(stream))
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
  
  class BasicServer {
    static const std::size_t FLUSH_BLOCK_SIZE = 16384;
    
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
      auto parser = std::make_shared<Parser>(*stream, *buffer);
      boost::beast::http::async_read_header(
        *stream, *buffer, *parser,
        [=](const boost::system::error_code& ec, size_t) {
          if (ec) {
            if (ec != boost::beast::http::error::end_of_stream)
              fail(ec, "parse header");
            return doClose(stream);
          }
          handleRead(stream, buffer, parser);
        });
    }

    template<typename Stream>
    void handleRead(
      const std::shared_ptr<Stream>& stream,
      const std::shared_ptr<boost::beast::flat_buffer>& buffer,
      const std::shared_ptr<Parser>& parser) const
    {
      BOOST_LOG_TRIVIAL(info) << boost::format("%x %s %s")
        % stream.get()
        % parser->get().method_string()
        % parser->get().target();
      auto response = std::make_shared<Response>(*stream);
      response->version(parser->get().version());
      response->set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response->keep_alive(parser->get().keep_alive());
      response->prepare_payload();

      doResponse(
        *parser, *response,
        [=](const boost::system::error_code& ec) {
          if (ec)
            fail(ec, "user handler");
          handleResponse(stream, buffer, parser, response);
        });
    }

    template<typename Stream>
    void handleResponse(
      const std::shared_ptr<Stream>& stream,
      const std::shared_ptr<boost::beast::flat_buffer>& buffer,
      const std::shared_ptr<Parser>& parser,
      const std::shared_ptr<Response>& response) const {

      if (!parser->is_done()) {
        BOOST_LOG_TRIVIAL(debug) << "parser not done";
        static std::vector<char> flushBuffer(FLUSH_BLOCK_SIZE);
        async_read(
          *parser, boost::asio::buffer(flushBuffer),
          [=](const boost::system::error_code& ec, std::size_t size) {
            if (!ec ||
                ec == boost::beast::http::error::need_buffer ||
                ec == boost::asio::error::eof) {
              handleResponse(stream, buffer, parser, response);
            }
            else {
              fail(ec, "parser flush");
            }
          });
        return;
      }
      
      response->async_finish(
        [=](const boost::system::error_code& ec) {
          boost::ignore_unused(response);
          BOOST_LOG_TRIVIAL(info) << "async_finish handler " << ec.message();
          if (ec)
            fail(ec, "response completion");
        });

      doRead(stream, buffer);
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

    // Override this function to control reading the request body.
    // This default implementation reads the entire body and attaches
    // it to the request as a string.
    virtual void doResponse(
      Parser& parser,
      Response& response,
      const std::function<void(const boost::system::error_code& ec)>& handler) const
    {
      static const size_t CHUNK_SIZE = 8192;
      static const size_t MAX_BODY_SIZE = 1048576;
      
      Request& request = parser.get();
      if (!parser.is_done()) {
        auto v = std::make_shared<std::vector<char>>(
          std::min(MAX_BODY_SIZE - request.body().size(), CHUNK_SIZE));
        async_read(
          parser, boost::asio::buffer(*v),
          [=, &parser, &request, &response](const boost::system::error_code& ec, std::size_t size) mutable {
            // Work around https://github.com/boostorg/beast/issues/1223
            size = ec == boost::beast::http::error::need_buffer ? v->size() : size;

            request.body().insert(request.body().end(), v->begin(), v->begin() + size);
            if (request.body().size() < MAX_BODY_SIZE) {
              doResponse(parser, response, handler);
            }
            else {
              if (!parser.is_done()) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("Body truncated to %d bytes")
                  % request.body().size();
              }
              doResponse(request, response, handler);
            }
          });
        return;
      }

      doResponse(request, response, handler);
    }

    // Override this function to get the request body as a string.
    virtual void doResponse(
      const Request& request,
      Response& response,
      const std::function<void(const boost::system::error_code& ec)>& handler) const
    {
      BOOST_LOG_TRIVIAL(info) << request.body();
      
      response.result(boost::beast::http::status::ok);

      auto body = std::make_shared<std::string>("Hello, world!");
      async_write(
        response, boost::asio::buffer(*body),
        [=](const boost::system::error_code& ec, std::size_t) {
          boost::ignore_unused(body);
          handler(ec);
        });
    }
  };

}

#endif // WebServer_H_
  
