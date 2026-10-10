#include <pulse/connection.hpp>

#include <chrono>
#include <utility>

namespace pulse {
namespace asio = boost::asio;
using boost::system::error_code;
using namespace std::chrono_literals;

Connection::Connection(asio::ip::tcp::socket socket)
    : socket_(std::move(socket)), idle_timer_(socket_.get_executor()),
      write_timer_(socket_.get_executor()) {}

void Connection::start(OnMessage on_message, OnClose on_close) {
    on_message_ = std::move(on_message);
    on_close_ = std::move(on_close);
    idle_timer();
    read();
}

void Connection::idle_timer() {
    idle_timer_.expires_after(5min);
    idle_timer_.async_wait([self = shared_from_this()](error_code error) {
        if (!error) {
            self->close("Read timeout");
        }
    });
}

void Connection::read() {
    socket_.async_read_some(asio::buffer(read_buffer_),
                            [self = shared_from_this()](error_code error, std::size_t size) {
                                if (self->closed_) {
                                    return;
                                }
                                try {
                                    for (const auto &message : self->decoder_.feed(
                                             std::span(self->read_buffer_).first(size))) {
                                        const auto on_message = self->on_message_;
                                        on_message(message);
                                        if (self->closed_) {
                                            return;
                                        }
                                    }
                                    if (error == asio::error::eof) {
                                        self->decoder_.finish();
                                        self->peer_ended_ = true;
                                        self->idle_timer_.cancel();
                                        if (self->outgoing_.empty()) {
                                            self->close();
                                        }
                                    } else if (error) {
                                        self->close(error.message());
                                    } else {
                                        self->idle_timer();
                                        self->read();
                                    }
                                } catch (const protocol::ProtocolError &exception) {
                                    self->close(exception.what());
                                }
                            });
}

void Connection::send(const protocol::Message &message) {
    send(std::make_shared<const std::vector<std::uint8_t>>(protocol::encode_frame(message)));
}

void Connection::send(Frame frame) {
    if (closed_ || input_ended_ || peer_ended_) {
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
    if (!writing) {
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
    asio::async_write(socket_, asio::buffer(*frame),
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
                          } else {
                              self->finish_write();
                          }
                      });
}

void Connection::end_input() {
    if (closed_ || input_ended_) {
        return;
    }
    input_ended_ = true;
    if (outgoing_.empty()) {
        finish_write();
    }
}

void Connection::finish_write() {
    if (peer_ended_) {
        close();
    } else if (input_ended_) {
        error_code error;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send, error);
        if (error) {
            close(error.message());
        }
    }
}

void Connection::close(std::string reason) {
    if (closed_) {
        return;
    }
    closed_ = true;
    idle_timer_.cancel();
    write_timer_.cancel();
    error_code ignored;
    socket_.close(ignored);
    outgoing_.clear();
    queued_bytes_ = 0;
    auto on_close = std::move(on_close_);
    on_message_ = {};
    if (on_close) {
        on_close(reason);
    }
}

} // namespace pulse
