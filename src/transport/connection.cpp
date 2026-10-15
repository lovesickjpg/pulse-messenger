#include <pulse/connection.hpp>

#include <chrono>
#include <utility>

namespace pulse {
namespace asio = boost::asio;
using boost::system::error_code;
using namespace std::chrono_literals;

Connection::Connection(tls::Stream stream)
    : stream_(std::move(stream)), idle_timer_(stream_.get_executor()),
      write_timer_(stream_.get_executor()) {}

void Connection::start(Role role, OnReady on_ready, OnMessage on_message, OnClose on_close) {
    on_ready_ = std::move(on_ready);
    on_message_ = std::move(on_message);
    on_close_ = std::move(on_close);
    handshake(role);
}

void Connection::arm_idle_timer(std::chrono::steady_clock::duration timeout, const char *reason) {
    idle_timer_.expires_after(timeout);
    idle_timer_.async_wait([self = shared_from_this(), reason](error_code error) {
        if (!error) {
            self->close(reason);
        }
    });
}

void Connection::handshake(Role role) {
    arm_idle_timer(10s, "TLS handshake timeout");
    stream_.async_handshake(role, [self = shared_from_this(), role](error_code error) {
        if (self->closed_) {
            return;
        }
        if (error) {
            // "certificate has expired" beats "certificate verify failed"
            auto detail = role == Role::client ? tls::verify_failure(self->stream_) : "";
            self->close("TLS handshake failed: " + (detail.empty() ? error.message() : detail));
            return;
        }
        self->ready_ = true;
        self->arm_idle_timer(5min, "Read timeout");
        self->read();
        if (!self->outgoing_.empty()) {
            self->write();
        }
        if (const auto on_ready = std::move(self->on_ready_)) {
            on_ready();
        }
    });
}

void Connection::read() {
    stream_.async_read_some(asio::buffer(read_buffer_), [self = shared_from_this()](
                                                            error_code error, std::size_t size) {
        if (self->closed_) {
            return;
        }
        try {
            for (const auto &message :
                 self->decoder_.feed(std::span(self->read_buffer_).first(size))) {
                const auto on_message = self->on_message_;
                on_message(message);
                if (self->closed_) {
                    return;
                }
            }
            // a peer that just drops TCP without close_notify shows up as stream_truncated;
            // frames are length-prefixed, so the decoder still catches a cut-off message
            if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
                self->peer_finished();
            } else if (error) {
                self->close(error.message());
            } else {
                self->arm_idle_timer(5min, "Read timeout");
                self->read();
            }
        } catch (const protocol::ProtocolError &exception) {
            self->close(exception.what());
        }
    });
}

void Connection::peer_finished() {
    decoder_.finish();
    peer_ended_ = true;
    idle_timer_.cancel();
    // let replies that are already queued go out first
    if (outgoing_.empty()) {
        close();
    }
}

void Connection::send(const protocol::Message &message) {
    send(std::make_shared<const std::vector<std::uint8_t>>(protocol::encode_frame(message)));
}

void Connection::send(Frame frame) {
    if (closed_ || peer_ended_) {
        return;
    }
    constexpr std::size_t max_queue_bytes = 1024 * 1024;
    if (frame->size() > max_queue_bytes - queued_bytes_) {
        close("Write queue exceeds 1 MiB");
        return;
    }
    const bool writing = !outgoing_.empty();
    queued_bytes_ += frame->size();
    outgoing_.push_back(std::move(frame));
    if (ready_ && !writing) {
        write();
    }
}

void Connection::write() {
    write_timer_.expires_after(10s);
    write_timer_.async_wait([self = shared_from_this()](error_code error) {
        if (!error) {
            self->close("Write timeout");
        }
    });
    // asio still needs the buffer after close() clears the queue
    const auto frame = outgoing_.front();
    asio::async_write(stream_, asio::buffer(*frame),
                      [self = shared_from_this(), frame](error_code error, std::size_t) {
                          if (self->closed_) {
                              return;
                          }
                          self->write_timer_.cancel();
                          if (error) {
                              self->close(error.message());
                              return;
                          }
                          self->queued_bytes_ -= frame->size();
                          self->outgoing_.pop_front();
                          if (!self->outgoing_.empty()) {
                              self->write();
                          } else if (self->peer_ended_) {
                              self->close();
                          }
                      });
}

void Connection::close(std::string reason) {
    if (closed_) {
        return;
    }
    closed_ = true;
    idle_timer_.cancel();
    write_timer_.cancel();
    // no close_notify dance: it would race the pending read, and frames carry their own length
    error_code ignored;
    stream_.lowest_layer().close(ignored);
    outgoing_.clear();
    queued_bytes_ = 0;
    auto on_close = std::move(on_close_);
    on_message_ = {};
    on_ready_ = {};
    if (on_close) {
        on_close(reason);
    }
}

} // namespace pulse
