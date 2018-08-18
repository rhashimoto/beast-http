#ifndef WebServer_H_
#define WebServer_H_

#include <stdexcept>
#include <vector>

#include <boost/version.hpp>
#if BOOST_VERSION >= 106600
#include <boost/asio/executor.hpp>
#include <boost/asio/io_context.hpp>
#else
#include <boost/asio/io_service.hpp>
#endif
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
#if BOOST_VERSION >= 106600
        for (auto i = boost::asio::buffer_sequence_begin(buffers);
             i != boost::asio::buffer_sequence_end(buffers);
             ++i) {
          this->emplace_back(*i);
        }
      }
#else
        for (const auto& buffer : buffers)
          this->emplace_back(buffer);
      }
#endif
    };

    // Type-erased movable handler.
    template<typename T> class MovableHandler;
    template<typename Result, typename... Args>
    class MovableHandler<Result(Args...)> {
    private:
      struct Holder {
        virtual Result operator()(Args...) = 0;
      };

      template<typename T>
      class HolderT : public Holder {
        T t_;
      public:
        HolderT(HolderT&& other) = default;

        template<typename U>
        HolderT(U&& u) : t_{std::forward<U>(u)} {
        }
        
        Result operator()(Args... a) override {
          t_(std::forward<Args>(a)...);
        }
      };

      std::unique_ptr<Holder> h_;

    public:
      MovableHandler() = default;
      MovableHandler(MovableHandler&& other) = default;

      template<typename Functor,
               typename = typename std::enable_if<
                 std::is_convertible<std::result_of_t<Functor(Args...)>, Result>::value>::type>
      MovableHandler(Functor&& f)
        : h_{new HolderT<typename std::decay<Functor>::type>{std::forward<Functor>(f)}} {
      }
      template<typename Functor,
               typename = typename std::enable_if<
                 std::is_convertible<std::result_of_t<Functor(Args...)>, Result>::value>::type>
      MovableHandler& operator=(Functor&& f) {
        h_.reset(new HolderT<typename std::decay<Functor>::type>{std::forward<Functor>(f)});
        return *this;
      }
      
      // Copy constructor and assignment are invalid and not used at
      // runtime, but must exist to pass boost/asio static assertion
      // checks in Boost 1.62.
      MovableHandler(const MovableHandler&) {
        throw std::logic_error("MovableHandler is not copyable");
      }
      MovableHandler& operator=(const MovableHandler&) {
        throw std::logic_error("MovableHandler is not copyable");
      }

      Result operator()(Args... args) const {
        return (*h_)(std::forward<Args>(args)...);
      }
    };
    typedef MovableHandler<void(const boost::system::error_code&, std::size_t)> TransferHandler;

    // Type-erasure adapter so Response can have a stream reference
    // without being a template class. This allows user handlers to be
    // written independently of the stream providing i/o.
    struct StreamAdapter {
      typedef BufferContainer<boost::asio::const_buffer> ConstBufferContainer;
      typedef BufferContainer<boost::asio::mutable_buffer> MutableBufferContainer;

      template<typename MutableBufferSequence,
               typename ReadHandler>
      void async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler) {
        // Select type erasure wrapper (MovableHandler or
        // std::function) based on whether the handler can be move
        // constructed.
        typedef typename std::conditional<
          std::is_move_constructible<ReadHandler>::value,
          TransferHandler,
          std::function<void(const boost::system::error_code&, std::size_t)>>::type
          TypeErasedHandler;
        async_read_some_forward(
          MutableBufferContainer(buffers),
          TypeErasedHandler{std::forward<ReadHandler>(handler)});
      }

      template<typename ConstBufferSequence,
               typename WriteHandler>
      void async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler) {
        // Select type erasure wrapper (MovableHandler or
        // std::function) based on whether the handler can be move
        // constructed.
        typedef typename std::conditional<
          std::is_move_constructible<WriteHandler>::value,
          TransferHandler,
          std::function<void(const boost::system::error_code&, std::size_t)>>::type
          TypeErasedHandler;
        async_write_some_forward(
          ConstBufferContainer(buffers),
          TypeErasedHandler(std::forward<WriteHandler>(handler)));
      }

#if BOOST_VERSION >= 106600
      virtual boost::asio::executor get_executor() = 0;
#else      
      virtual boost::asio::io_service& get_io_service() = 0;
#endif
      
      // AsyncWriteStream
      virtual void async_write_some_forward(
        ConstBufferContainer&& buffers,
        TransferHandler&& handler) = 0;
      virtual void async_write_some_forward(
        ConstBufferContainer&& buffers,
        std::function<void(const boost::system::error_code&, std::size_t)>&& handler) = 0;

      // AsyncReadStream
      virtual void async_read_some_forward(
        MutableBufferContainer&& buffers,
        TransferHandler&& handler) = 0;
      virtual void async_read_some_forward(
        MutableBufferContainer&& buffers,
        std::function<void(const boost::system::error_code&, std::size_t)>&& handler) = 0;

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
      explicit
      StreamAdapterT(StreamType& stream)
        : stream_(stream)
      {
      }

#if BOOST_VERSION >= 106600
      virtual boost::asio::executor get_executor() {
        return stream_.get_executor();
      }
#else
      virtual boost::asio::io_service& get_io_service() {
        return stream_.get_io_service();
      }
#endif
      
      virtual void async_write_some_forward(
        ConstBufferContainer&& buffers,
        TransferHandler&& handler) override
      {
        stream_.async_write_some(std::move(buffers), std::move(handler));
      }
      virtual void async_write_some_forward(
        ConstBufferContainer&& buffers,
        std::function<void(const boost::system::error_code&, std::size_t)>&& handler) override
      {
        stream_.async_write_some(std::move(buffers), std::move(handler));
      }    

      virtual void async_read_some_forward(
        MutableBufferContainer&& buffers,
        TransferHandler&& handler) override
      {
        stream_.async_read_some(std::move(buffers), std::move(handler));
      }
      virtual void async_read_some_forward(
        MutableBufferContainer&& buffers,
        std::function<void(const boost::system::error_code&, std::size_t)>&& handler) override
      {
        stream_.async_read_some(std::move(buffers), std::move(handler));
      }

      virtual std::size_t write_some(
        ConstBufferContainer buffers,
        boost::system::error_code& ec) override
      {
        return stream_.write_some(buffers, ec);
      }

      virtual std::size_t read_some(
        MutableBufferContainer buffers,
        boost::system::error_code& ec) override
      {
        return stream_.read_some(buffers, ec);
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
    explicit
    Parser(Stream& stream, boost::beast::flat_buffer& buffer)
      : boost::beast::http::request_parser<detail::RequestBody>()
      , stream_(new detail::StreamAdapterT<Stream>(stream))
      , buffer_(buffer)
    {
    }

#if BOOST_VERSION >= 106600
    boost::asio::executor get_executor() {
      return stream_->get_executor();
    }
#else    
    boost::asio::io_service& get_io_service() {
      return stream_->get_io_service();
    }
#endif
    
    // AsyncReadStream
    template<typename MutableBufferSequence, typename ReadHandler>
    void async_read_some(
      const MutableBufferSequence& buffers,
      ReadHandler&& handler)
    {
#if BOOST_VERSION >= 106600
      for (auto i = boost::asio::buffer_sequence_begin(buffers);
           i != boost::asio::buffer_sequence_end(buffers);
           ++i) {
        get().body().buffers.emplace_back(*i);
      }
#else
      for (const auto& buffer : buffers)
        get().body().buffers.emplace_back(buffer);
#endif

      boost::beast::http::async_read(
        *stream_, buffer_, *this,
        std::forward<ReadHandler>(handler));
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

    template<typename Handler>
    void async_finish(Handler&& handler) {
      body().more = false;
      async_write(
        *stream_, serializer_,
        std::forward<Handler>(handler));
    }

    template<typename Stream>
    friend class Session;
  public:
    template<typename Stream>
    explicit
    Response(Stream& stream)
      : boost::beast::http::response<detail::ResponseBody>()
      , stream_(new detail::StreamAdapterT<Stream>(stream))
      , serializer_(*this) {
    }

#if BOOST_VERSION >= 106600
    boost::asio::executor get_executor() {
      return stream_->get_executor();
    }
#else    
    boost::asio::io_service&
    get_io_service() {
      return stream_->get_io_service();
    }
#endif
    
    // AsyncWriteStream
    template<typename ConstBufferSequence, typename WriteHandler>
    void async_write_some(
      const ConstBufferSequence& buffers,
      WriteHandler&& handler)
    {
      for (const auto& buffer : buffers)
        body().buffers.emplace_back(buffer);

      boost::beast::http::async_write(
        *stream_, serializer_,
        [this, handler(std::forward<WriteHandler>(handler))](
            boost::system::error_code ec, std::size_t size) mutable
         {
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

    explicit
    Session(Stream& stream)
      : stream_(stream) {
    }

    // Two handlers must be provided. The request handler is called
    // for each request in the session. The completion handler is
    // called once at the conclusion of the session.
    void start(
      detail::MovableHandler<void(Parser&, Response&, const CompletionHandler&)>&& handle,
      detail::MovableHandler<void(const boost::system::error_code&)>&& complete)
    {
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
    detail::MovableHandler<void(Parser&, Response&, const CompletionHandler&)> handle_;
    detail::MovableHandler<void(const boost::system::error_code&)> complete_;

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
        [=](const boost::system::error_code& ec, std::size_t) mutable {
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
#if BOOST_VERSION >= 106600
    boost::asio::io_context& io_;
#else    
    boost::asio::io_service& io_;
#endif
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
      auto session = std::make_unique<Session<Stream>>(*socket);
      auto sptr = session.get();
      sptr->start(
        [=](Parser& parser, Response& response, const Session<Stream>::CompletionHandler& complete) {
          handleRequest(parser, response, complete);
        },
        [=, session(std::move(session))](const boost::system::error_code& ec) {
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
#if BOOST_VERSION >= 106600
    BasicServer(boost::asio::io_context& io)
      : io_(io)
      , acceptor_(io) {
    }
#else
    BasicServer(boost::asio::io_service& io)
      : io_(io)
      , acceptor_(io) {
    }
#endif
    
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
        std::vector<char> v(std::min(MAX_BODY_SIZE - request.body().size(), CHUNK_SIZE));
        auto buffer = boost::asio::buffer(v.data(), v.size());
        async_read(
          parser, buffer,
          [=, v(std::move(v)), &parser, &request, &response](
            const boost::system::error_code& ec, std::size_t size) mutable
          {
            // Work around https://github.com/boostorg/beast/issues/1223
            size = ec == boost::beast::http::error::need_buffer ? v.size() : size;

            request.body().insert(request.body().end(), v.begin(), v.begin() + size);
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
