
namespace WebServer {
  class Response;
  
  namespace detail {
    struct ResponseBody {
      class reader;
      class value_type {
        std::vector<boost::asio::const_buffer> buffers;
        bool more;

      public:
        value_type()
          : more(true) {
        }

        friend class WebServer::Response;
        friend class ResponseBody::reader;
      };
  
      class reader {
        const value_type& value_;
        bool toggle_;
      public:
        typedef std::vector<boost::asio::const_buffer> const_buffers_type;
    
        template<bool isRequest, class Fields>
        explicit
        reader(boost::beast::http::message<isRequest, ResponseBody, Fields>& msg)
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
    
    struct StreamFacade {
      virtual boost::asio::io_service& get_io_service() = 0;
      virtual void async_write_some(
        ConstBufferContainer buffers,
        std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;
    };

    template<typename StreamType>
    class StreamFacadeT : public StreamFacade {
      StreamType& stream_;
    public:
      StreamFacadeT(StreamType& stream)
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
    };
  }
}
