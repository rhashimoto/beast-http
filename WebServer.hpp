#ifndef WebServer_H_
#define WebServer_H_

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/beast.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace WebServer {
  class Parser;
  class Response;
  namespace detail {
    // BodyReader/BodyWriter were reversed @ Boost 1.66.
#if BOOST_VERSION >= 106600
#define READER reader
#define WRITER writer
#else
#define READER writer
#define WRITER reader
#endif
    
    struct RequestBody {
      class READER;
      class value_type : public std::string {
        std::vector<boost::asio::mutable_buffer> buffers;
        
        friend class RequestBody::READER;
        friend class WebServer::Parser;
      };

      class READER {
        value_type& value_;
      public:
        // Deprecated in Boost 1.66.
        template<bool isRequest, class Fields>
        explicit
        READER(boost::beast::http::message<isRequest, RequestBody, Fields>& msg)
          : value_(msg.body())
        {
        }
        
        template<bool isRequest, class Fields>
        explicit
        READER(boost::beast::http::header<isRequest, Fields>&, value_type& value)
          : value_(value) {
        }
        
        void init(boost::optional<std::uint64_t> const&, boost::system::error_code& ec) {
          ec.assign(0, ec.category());
        }

        template<class ConstBufferSequence>
        std::size_t put(ConstBufferSequence const& buffers, boost::system::error_code& ec) {
          auto size = boost::asio::buffer_copy(value_.buffers, buffers);
          if (size == boost::asio::buffer_size(buffers))
            ec.assign(0, ec.category());
          else
            ec = boost::beast::http::error::need_buffer;

          value_.buffers.clear();
          return size;
        }

        void
        finish(boost::system::error_code& ec) {
          ec.assign(0, ec.category());
        }
      };
    };
    
    struct ResponseBody {
      class WRITER;
      class value_type : public std::string {
        std::vector<boost::asio::const_buffer> buffers;
        bool more;

      public:
        value_type()
          : more(true) {
        }

        value_type& operator=(const std::string& s) {
          std::string::operator=(s);
          return *this;
        }
        
        friend class ResponseBody::WRITER;
        friend class WebServer::Response;
      };
  
      class WRITER {
        const value_type& value_;
        bool toggle_;
      public:
        typedef std::vector<boost::asio::const_buffer> const_buffers_type;
    
        // Deprecated in Boost 1.66.
        template<bool isRequest, class Fields>
        explicit
        WRITER(boost::beast::http::message<isRequest, ResponseBody, Fields>& msg)
          : value_(msg.body())
          , toggle_(false) {
        }

        template<bool isRequest, class Fields>
        explicit
        WRITER(boost::beast::http::header<isRequest, Fields>&, value_type& value)
          : value_(value)
          , toggle_(false) {
        }

        void init(boost::system::error_code& ec) {
          ec.assign(0, ec.category());
        }
    
        boost::optional<std::pair<const_buffers_type, bool>>
        get(boost::system::error_code& ec) {
          ec.assign(0, ec.category());
          
          const auto size = boost::asio::buffer_size(value_.buffers);
          if (toggle_ || (value_.empty() && size == 0)) {
            if (value_.more) {
              toggle_ = false;
              ec = boost::beast::http::error::need_buffer;
            }
            return boost::none;
          }

          // When data is returned with more=true, the serializer will
          // call again without executing the handler. Arrange to return
          // need_buffer on that second call to avoid an infinite loop (as
          // buffer_body does).
          toggle_ = true;
          if (!value_.empty()) {
            return std::pair<const_buffers_type, bool>(
              const_buffers_type(1, boost::asio::buffer(value_)),
              value_.more);
          }
          return std::pair<const_buffers_type, bool>(value_.buffers, value_.more);
        }
      };
    };
#undef READER
#undef WRITER
    
    struct ConstBufferContainer : public std::vector<boost::asio::const_buffer> {
      ConstBufferContainer() = default;

      template<typename T>
      ConstBufferContainer(const T& buffers) {
        for (const auto& buffer : buffers)
          emplace_back(buffer);
      }
    };
    
    struct MutableBufferContainer : public std::vector<boost::asio::mutable_buffer> {
      MutableBufferContainer() = default;

      template<typename T>
      MutableBufferContainer(const T& buffers) {
        for (const auto& buffer : buffers)
          emplace_back(buffer);
      }
    };
    
    struct StreamAdapter {
      virtual boost::asio::io_service& get_io_service() = 0;

      // AsyncWriteStream
      virtual void async_write_some(
        ConstBufferContainer buffers,
        std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;

      // AsyncReadStream
      virtual void async_read_some(
        MutableBufferContainer buffers,
        std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;

      // SyncWriteStream
      virtual std::size_t write_some(
        ConstBufferContainer buffers,
        boost::system::error_code& ec) = 0;
      virtual std::size_t write_some(ConstBufferContainer buffers) {
        boost::system::error_code ec;
        auto size = write_some(buffers, ec);
        if (ec)
          throw boost::system::system_error(ec);
        return size;
      }

      // SyncReadStream
      virtual std::size_t read_some(
        MutableBufferContainer buffers,
        boost::system::error_code& ec) = 0;
      virtual std::size_t read_some(MutableBufferContainer buffers) {
        boost::system::error_code ec;
        auto size = read_some(buffers, ec);
        if (ec)
          throw boost::system::system_error(ec);
        return size;
      }
    };

    template<typename StreamType>
    class StreamAdapterT : public StreamAdapter {
      StreamType& stream_;
    public:
      StreamAdapterT(StreamType& stream)
        : stream_(stream)
      {
      }

      virtual boost::asio::io_service& get_io_service() {
        return stream_.get_io_service();
      }

      virtual void async_write_some(
        ConstBufferContainer buffers,
        std::function<void(const boost::system::error_code&, std::size_t)> handler)
      {
        stream_.async_write_some(std::move(buffers), std::move(handler));
      }

      virtual void async_read_some(
        MutableBufferContainer buffers,
        std::function<void(const boost::system::error_code&, std::size_t)> handler)
      {
        stream_.async_read_some(std::move(buffers), std::move(handler));
      }

      virtual std::size_t write_some(
        ConstBufferContainer buffers,
        boost::system::error_code& ec)
      {
        return stream_.write_some(std::move(buffers), ec);
      }

      virtual std::size_t read_some(
        MutableBufferContainer buffers,
        boost::system::error_code& ec)
      {
        return stream_.read_some(std::move(buffers), ec);
      }
    };
  } // namespace detail

    // RequestParser with AsyncReadStream and SyncReadStream support.
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

    // AsyncReadStream
    template<typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(
      const MutableBufferSequence& buffers,
      ReadHandler handler)
    {
      const auto buffersSize = boost::asio::buffer_size(buffers);
      for (const auto& buffer : buffers)
        get().body().buffers.emplace_back(buffer);

      async_read(*stream_, buffer_, *this,
        [=](const boost::system::error_code& ec, std::size_t size) mutable {
          handler(ec, size);
        });
    }

    // SyncReadStream
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
      for (const auto& buffer : buffers)
        body().buffers.emplace_back(buffer);
    
      async_write(
        *stream_,
        serializer_,
        [=](boost::system::error_code ec, std::size_t size) mutable {
          if (!ec || ec == boost::beast::http::error::need_buffer) {
            body().buffers.clear();
            ec = {};
          }
          handler(ec, size);
        });
    }

    template<typename Token>
    auto async_finish(Token&& token) {
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

  template<typename Stream>
  class Session {
  public:
    typedef std::function<void (const boost::system::error_code&)> CompletionHandler;
    typedef std::function<void (Parser&, Response&, const CompletionHandler&)> RequestHandler;

    Session(Stream& stream)
      : stream_(stream)
      , flushBufferBytes_(DEFAULT_FLUSH_BUFFER_BYTES) {
    }

    void start(RequestHandler handle, CompletionHandler complete) {
      handle_ = std::move(handle);
      complete_ = std::move(complete);
      beginTransaction();
    }

  private:
    static const std::size_t DEFAULT_FLUSH_BUFFER_BYTES = 8192;
    
    Stream& stream_;
    boost::beast::flat_buffer parseBuffer_;
    
    RequestHandler handle_;
    CompletionHandler complete_;

    std::size_t flushBufferBytes_;

    // Create the request parser and parse the header.
    void beginTransaction() {
      auto parser = std::make_shared<Parser>(stream_, parseBuffer_);
      boost::beast::http::async_read_header(
        stream_, parseBuffer_, *parser,
        [=](const boost::system::error_code& ec, size_t) mutable {
          if (ec) {
            if (ec != boost::beast::http::error::end_of_stream)
              fail(ec, "parse header");
            return close(ec);
          }
          invokeHandler(parser);
        });
    }

    // Create the response and invoke the handler callback.
    void invokeHandler(const std::shared_ptr<Parser>& parser) {
      BOOST_LOG_TRIVIAL(info) << boost::format("%x %s %s")
        % &stream_
        % parser->get().method_string()
        % parser->get().target();
      auto response = std::make_shared<Response>(stream_);
      response->version(parser->get().version());
      response->set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response->keep_alive(parser->get().keep_alive());
      response->prepare_payload();

      handle_(
        *parser, *response,
        [=](const boost::system::error_code& ec) mutable {
          if (ec) {
            fail(ec, "user handler");
            return close(ec);
          }
          endTransaction(parser, response);
        });
    }

    // Complete request input and response output.
    void endTransaction(
      const std::shared_ptr<Parser>& parser,
      const std::shared_ptr<Response>& response)
    {
      if (!parser->is_done()) {
        static std::vector<char> flushBuffer;
        flushBuffer.resize(flushBufferBytes_);
        
        return async_read(
          *parser, boost::asio::buffer(flushBuffer),
          [=](const boost::system::error_code& ec, std::size_t size) mutable {
            if (!ec ||
                ec == boost::beast::http::error::need_buffer ||
                ec == boost::asio::error::eof) {
              endTransaction(parser, response);
            }
            else {
              fail(ec, "parser flush");
              close(ec);
            }
          });
      }
      
      response->async_finish(
        [=](const boost::system::error_code& ec) mutable {
          boost::ignore_unused(response);
          if (ec)
            fail(ec, "response completion");
          beginTransaction();
        });
    }

    // Invoke the completion callback.
    void close(const boost::system::error_code& ec) const {
      complete_(ec);
    }

    void fail(const boost::system::error_code& ec, const std::string& context) const {
      BOOST_LOG_TRIVIAL(error) << context << ": " << ec.message();
    }
    
  };
      
  class BasicServer {
    static const std::size_t FLUSH_BUFFER_SIZE = 16384;
    
    boost::asio::io_service& io_;
    boost::asio::ip::tcp::acceptor acceptor_;

    typedef boost::asio::ip::tcp::socket Stream;
    
    void beginConnection() {
      auto socket = std::make_shared<Stream>(io_);
      acceptor_.async_accept(
        *socket,
        [=](const boost::system::error_code& ec) mutable {
          if (ec) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%x Socket error: %s ")
              % socket.get()
              % ec.message();
            return;
          }
          createSession(socket);
        });
    }

    void createSession(const std::shared_ptr<Stream>& socket) {
      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << boost::format("%x Accepting %s")
          % socket.get()
          % endpoint.address().to_string();
      }
      else {
        return endConnection(socket);
      }

      beginConnection();
      auto session = std::make_shared<Session<Stream>>(*socket);
      session->start(
        [=](Parser& parser, Response& response, const Session<Stream>::CompletionHandler& complete) {
          handleRequest(parser, response, complete);
        },
        [=](const boost::system::error_code& ec) {
          // Force capture to extend the lifetime of the Session.
          boost::ignore_unused(session);
          endConnection(socket);
        });
    }

    void endConnection(const std::shared_ptr<Stream>& socket) const {
      boost::system::error_code ec;
      auto endpoint = socket->remote_endpoint(ec);
      if (!ec) {
        BOOST_LOG_TRIVIAL(info) << boost::format("%x Closing %s ")
          % socket.get()
          % endpoint.address().to_string();
      }
      else {
        BOOST_LOG_TRIVIAL(error) << boost::format("%x Invalid endpoint: %s")
          % socket.get()
          % ec.message();
      }
        
      socket->shutdown(Stream::shutdown_send, ec);
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

      beginConnection();
    }

    virtual void stop() {
    }

    // Override this function to control reading the request body.
    // This default implementation reads the entire body and attaches
    // it to the request as a string.
    virtual void handleRequest(
      Parser& parser,
      Response& response,
      const std::function<void(const boost::system::error_code& ec)>& complete) const
    {
      static const size_t CHUNK_SIZE = 8192;
      static const size_t MAX_BODY_SIZE = 1 << 20;
      
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
              handleRequest(parser, response, complete);
            }
            else {
              if (!parser.is_done()) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("Body truncated to %d bytes")
                  % request.body().size();
              }
              handleRequest(request, response, complete);
            }
          });
        return;
      }

      handleRequest(request, response, complete);
    }

    // Override this function to get the request body as a string.
    virtual void handleRequest(
      const Request& request,
      Response& response,
      const std::function<void(const boost::system::error_code& ec)>& complete) const
    {
      BOOST_LOG_TRIVIAL(info) << request.body();
      
      response.result(boost::beast::http::status::ok);
      response.body() = "Hello, world!";
      complete(boost::system::error_code());
    }
  };

}

#endif // WebServer_H_
  
