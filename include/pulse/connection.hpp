#pragma once

#include <pulse/protocol.hpp>
#include <pulse/tls.hpp>

#include <boost/asio.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>

namespace pulse {

// One framed TLS connection. io_context thread only, no locks here.
class Connection : public std::enable_shared_from_this<Connection> {
  public:
    using Frame = std::shared_ptr<const std::vector<std::uint8_t>>;
    using Role = boost::asio::ssl::stream_base::handshake_type;
    using OnReady = std::function<void()>;
    using OnMessage = std::function<void(const protocol::Message &)>;
    using OnClose = std::function<void(const std::string &)>;

    explicit Connection(tls::Stream stream);

    // Runs the TLS handshake first. Nothing is read or written until it succeeds,
    // so a client can't leak a password to a server it hasn't verified.
    void start(Role role, OnReady on_ready, OnMessage on_message, OnClose on_close);
    // Safe to call before the handshake finishes; frames just wait in the queue
    void send(Frame frame);
    void send(const protocol::Message &message);
    void close(std::string reason = {});

  private:
    void handshake(Role role);
    void read();
    void write();
    void arm_idle_timer(std::chrono::steady_clock::duration timeout, const char *reason);
    void peer_finished();

    tls::Stream stream_;
    boost::asio::steady_timer idle_timer_;
    boost::asio::steady_timer write_timer_;
    protocol::FrameDecoder decoder_;
    std::array<std::uint8_t, 8192> read_buffer_{};
    std::deque<Frame> outgoing_;
    std::size_t queued_bytes_ = 0;
    OnReady on_ready_;
    OnMessage on_message_;
    OnClose on_close_;
    bool ready_ = false;
    bool closed_ = false;
    bool peer_ended_ = false;
};

} // namespace pulse
