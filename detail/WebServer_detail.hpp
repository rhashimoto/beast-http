
namespace WebServer {
  class Parser;
  class Response;
  
  namespace detail {
    struct RequestBody {
#if BOOST_VERSION >= 106600
      class reader;
#else
      class writer;
#endif
      class value_type : public std::string {
        std::vector<boost::asio::mutable_buffer> buffers;

        // BodyReader/BodyWriter were reversed @ Boost 1.66.
#if BOOST_VERSION >= 106600
        friend class RequestBody::reader;
#else
        friend class RequestBody::writer;
#endif
        friend class WebServer::Parser;
      };

      // BodyReader/BodyWriter were reversed @ Boost 1.66.
#if BOOST_VERSION >= 106600
      class reader {
#else
      class writer {
#endif
        value_type& value_;
      public:
        template<bool isRequest, class Fields>
        explicit
        writer(boost::beast::http::message<isRequest, RequestBody, Fields>& msg)
          : value_(msg.body())
        {
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
      // BodyReader/BodyWriter were reversed for Boost release.
#if BOOST_VERSION >= 106600
      class writer;
#else
      class reader;
#endif
      class value_type {
        std::vector<boost::asio::const_buffer> buffers;
        bool more;

      public:
        value_type()
          : more(true) {
        }

        // BodyReader/BodyWriter were reversed for Boost release.
#if BOOST_VERSION >= 106600
        friend class ResponseBody::writer;
#else
        friend class ResponseBody::reader;
#endif
        friend class WebServer::Response;
      };
  
      // BodyReader/BodyWriter were reversed @ Boost 1.66.
#if BOOST_VERSION >= 106600
      class writer {
#else
      class reader {
#endif
        const value_type& value_;
        bool toggle_;
      public:
        typedef std::vector<boost::asio::const_buffer> const_buffers_type;
    
        template<bool isRequest, class Fields>
        explicit
      // BodyReader/BodyWriter were reversed @ Boost 1.66.
#if BOOST_VERSION >= 106600
        writer(boost::beast::http::message<isRequest, ResponseBody, Fields>& msg)
#else
        reader(boost::beast::http::message<isRequest, ResponseBody, Fields>& msg)
#endif
          : value_(msg.body())
          , toggle_(false) {
        }

        void init(boost::system::error_code& ec) {
          ec.assign(0, ec.category());
        }
    
        boost::optional<std::pair<const_buffers_type, bool>>
        get(boost::system::error_code& ec) {
          ec.assign(0, ec.category());
          const auto size = boost::asio::buffer_size(value_.buffers);
          if (toggle_ || size == 0) {
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
          return std::pair<const_buffers_type, bool>(value_.buffers, value_.more);
        }
      };
    };

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
  }
}
