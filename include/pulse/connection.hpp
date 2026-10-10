#pragma once

#include <pulse/protocol.hpp>

#include <boost/asio.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>

namespace pulse {

// io_context thread only, no locks here
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    using Frame = std::shared_ptr<const std::vector<std::uint8_t>>;
    using OnMessage = std::function<void(const protocol::Message &)>;
    using OnClose = std::function<void(const std::string &)>;

    explicit Connection(boost::asio::ip::tcp::socket socket);
    void start(OnMessage on_message, OnClose on_close);
    void send(Frame frame);
    void send(const protocol::Message &message);
    // flush the queue, then wait for replies
    void end_input();
    void close(std::string reason = {});

  private:
    void read();
    void write();
    void idle_timer();
    void finish_write();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer idle_timer_;
    boost::asio::steady_timer write_timer_;
    protocol::FrameDecoder decoder_;
    std::array<std::uint8_t, 8192> read_buffer_{};
    std::deque<Frame> outgoing_;
    std::size_t queued_bytes_ = 0;
    OnMessage on_message_;
    OnClose on_close_;
    bool closed_ = false;
    bool input_ended_ = false;
    bool peer_ended_ = false;
};

} // namespace pulse
