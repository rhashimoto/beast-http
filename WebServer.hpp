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
// BodyReader/BodyWriter were swapped @ Boost Beast 132.
// https://github.com/boostorg/beast/commit/895c9fa7ed79df27dca119d0395941045289e2a3    
#if BOOST_BEAST_VERSION >= 132
#define READER reader
#define WRITER writer
#else
#define READER writer
#define WRITER reader
#endif

    // Implementation of boost::beast Body concept:
    // https://www.boost.org/doc/libs/1_68_0/libs/beast/doc/html/beast/concepts/Body.html
    struct RequestBody {
      class READER;

      // Subclass value_type from std::string to allow both a simple
      // handler that can access pre-parsed body data or a handler
      // that reads its own boday.data.
      class value_type : public std::string {
        std::vector<boost::asio::mutable_buffer> buffers;
        
        friend class READER;
        friend class WebServer::Parser;
      };

      // Implementation of boost::beast BodyReader concept:
      // https://www.boost.org/doc/libs/1_68_0/libs/beast/doc/html/beast/concepts/BodyReader.html
      class READER {
        value_type& value_;
      public:
#if BOOST_BEAST_VERSION < 150
        template<bool isRequest, class Fields>
        explicit
        READER(boost::beast::http::message<isRequest, RequestBody, Fields>& msg)
          : READER(msg.base(), msg.body())
        {
        }
#endif
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
    
    // Implementation of boost::beast Body concept:
    // https://www.boost.org/doc/libs/1_68_0/libs/beast/doc/html/beast/concepts/Body.html
    struct ResponseBody {
      class WRITER;

      // Subclass from std::string so the handler can choose between
      // setting the entire body as a string or writing body data to
      // the Response.
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
        
        friend class WRITER;
        friend class WebServer::Response;
      };
  
      class WRITER {
        const value_type& value_;
        bool toggle_;
      public:
        typedef std::vector<boost::asio::const_buffer> const_buffers_type;
    
#if BOOST_BEAST_VERSION < 150
        template<bool isRequest, class Fields>
        explicit
        WRITER(boost::beast::http::message<isRequest, ResponseBody, Fields>& msg)
          : WRITER(msg.base(), msg.body()) {
        }
#endif
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

    // This implements boost::asio ConstBufferSequence so StreamAdapter can
    // have virtual non-template methods.
    template<typename BufferType>
    struct BufferContainer : public std::vector<BufferType> {
      BufferContainer() = default;

      template<typename T>
      BufferContainer(const T& buffers) {
        for (const auto& buffer : buffers)
          this->emplace_back(buffer);
      }
    };

    // Type-erasure adapter so Response can have a stream reference
    // without being a template class. This allows user handlers to be
    // written independently of the stream providing i/o.
    struct StreamAdapter {
      typedef BufferContainer<boost::asio::const_buffer> ConstBufferContainer;
      typedef BufferContainer<boost::asio::mutable_buffer> MutableBufferContainer;
      
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
  // User handlers can read request body data from the Parser.
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
  
  // Response with AsyncWriteStream and SyncWriteStream support. User
  // handlers can write response body data to the Response.
  template<typename Stream> class Session;
  class Response : public boost::beast::http::response<detail::ResponseBody> {
    std::unique_ptr<detail::StreamAdapter> stream_;
    boost::beast::http::response_serializer<body_type, fields_type> serializer_;

    template<typename T> friend class Session;
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
        *stream_, serializer_, 
        [=](boost::system::error_code ec, std::size_t size) mutable {
          handler(ec);
        });
    
      return result.get();
    }

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

    // AsyncWriteStream
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

    // SyncWriteStream
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

  // This class encapsulates a session, i.e. a stream connection that
  // may include multiple requests.
  template<typename Stream>
  class Session {
  public:
    typedef std::function<void (const boost::system::error_code&)> CompletionHandler;
    typedef std::function<void (Parser&, Response&, const CompletionHandler&)> RequestHandler;

    Session(Stream& stream)
      : stream_(stream) {
    }

    // Two handlers must be provided. The request handler is called
    // for each request in the session. The completion handler is
    // called once at the conclusion of the session.
    void start(RequestHandler handle, CompletionHandler complete) {
      handle_ = std::move(handle);
      complete_ = std::move(complete);
      beginTransaction();
    }

  private:
    // After the user handler is done, any unread request body data is
    // read and discarded. This parameter defines the buffer size for
    // these discarded reads.
    static const std::size_t DEFAULT_FLUSH_BUFFER_BYTES = 8192;
    
    Stream& stream_;
    RequestHandler handle_;
    CompletionHandler complete_;

    // Parsing requires a single buffer for all requests on the
    // stream.
    boost::beast::flat_buffer parseBuffer_;

    // Per-request state.
    boost::optional<Parser> parser_;
    boost::optional<Response> response_;
    
    // Create the request parser and parse the header.
    void beginTransaction() {
      parser_.emplace(stream_, parseBuffer_);
      boost::beast::http::async_read_header(
        stream_, parseBuffer_, parser_.get(),
        [=](const boost::system::error_code& ec, size_t) mutable {
          if (ec) {
            if (ec != boost::beast::http::error::end_of_stream)
              fail(ec, "parse header");
            return close(ec);
          }
          invokeHandler();
        });
    }

    // Create the response and invoke the handler callback.
    void invokeHandler() {
      BOOST_LOG_TRIVIAL(info) << boost::format("%x %s %s")
        % &stream_
        % parser_.get().get().method_string()
        % parser_.get().get().target();
      response_.emplace(stream_);
      response_.get().version(parser_.get().get().version());
      response_.get().set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
      response_.get().keep_alive(parser_.get().get().keep_alive());
      response_.get().prepare_payload();

      handle_(
        parser_.get(), response_.get(),
        [=](const boost::system::error_code& ec) mutable {
          if (ec) {
            fail(ec, "user handler");
            return close(ec);
          }
          endTransaction();
        });
    }

    // Complete request input and response output.
    void endTransaction()
    {
      if (!parser_.get().is_done()) {
        // Flush the unread request body data.
        static std::vector<char> flushBuffer(DEFAULT_FLUSH_BUFFER_BYTES);
        
        return async_read(
          parser_.get(), boost::asio::buffer(flushBuffer),
          [=](const boost::system::error_code& ec, std::size_t size) mutable {
            if (!ec ||
                ec == boost::beast::http::error::need_buffer ||
                ec == boost::asio::error::eof) {
              endTransaction();
            }
            else {
              fail(ec, "parser flush");
              close(ec);
            }
          });
      }
      parser_ = boost::none;
      
      response_.get().async_finish(
        [=](const boost::system::error_code& ec) mutable {
          response_ = boost::none;
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

  // Simple unencrypted example server.
  class BasicServer {
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
          // This capture extends the lifetime of the Session to this
          // point.
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
