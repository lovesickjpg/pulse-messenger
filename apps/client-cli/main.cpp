#include <pulse/connection.hpp>

#include "../arguments.hpp"
#include "console_input.hpp"

#include <csignal>
#include <iostream>
#include <random>
#include <unordered_set>

namespace asio = boost::asio;
using asio::ip::tcp;
using pulse::protocol::Message;

namespace {

std::string client_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string id;
    for (int index = 0; index < 16; ++index) {
        const auto byte = static_cast<unsigned char>(random());
        id += hex[byte >> 4];
        id += hex[byte & 15];
    }
    return id;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: pulse-client [port]\n"
                         "Connects to 127.0.0.1:5555 by default. Type a line to send it.\n"
                         "/quit or EOF flushes pending requests and exits.\n";
            return 0;
        }
        if (argc > 2) {
            throw std::invalid_argument("Usage: pulse-client [port]");
        }
        const auto port =
            static_cast<unsigned short>(argc == 2 ? pulse::parse_number(argv[1], 65535) : 5555);
#ifdef _WIN32
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
#endif
        asio::io_context context;
        tcp::socket socket(context);
        asio::steady_timer deadline(context);
        asio::signal_set signals(context, SIGINT, SIGTERM);
        std::shared_ptr<pulse::Connection> connection;
        std::unordered_set<std::string> pending;
        const auto prefix = client_id();
        std::uint64_t sequence = 0;
        int exit_code = 0;

        ConsoleInput input(
            context,
            [&](std::string line) {
                if (line == "/quit") {
                    input.finish();
                    return;
                }
                if (line.empty()) {
                    return;
                }
                const auto id = prefix + "-" + std::to_string(++sequence);
                try {
                    pending.insert(id);
                    connection->send(Message{1,
                                             "message.send",
                                             id,
                                             {{"conversation_id", "lobby"},
                                              {"client_message_id", id},
                                              {"text", std::move(line)}}});
                } catch (const pulse::protocol::ProtocolError &error) {
                    pending.erase(id);
                    std::cerr << "Skipped: " << error.what() << '\n';
                    exit_code = 1;
                }
            },
            [&] {
                connection->end_input();
                deadline.expires_after(std::chrono::seconds(10));
                deadline.async_wait([&](boost::system::error_code error) {
                    if (!error) {
                        connection->close("Timed out waiting for replies");
                    }
                });
            });

        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) {
                input.stop();
                deadline.cancel();
                if (connection) {
                    connection->close();
                } else {
                    socket.close();
                }
            }
        });
        deadline.expires_after(std::chrono::seconds(5));
        deadline.async_wait([&](boost::system::error_code error) {
            if (!error) {
                std::cerr << "Connect timeout\n";
                exit_code = 1;
                socket.close();
            }
        });
        socket.async_connect(
            {asio::ip::address_v4::loopback(), port}, [&](boost::system::error_code error) {
                deadline.cancel();
                if (error) {
                    std::cerr << "Connect: " << error.message() << '\n';
                    exit_code = 1;
                    signals.cancel();
                    return;
                }
                connection = std::make_shared<pulse::Connection>(std::move(socket));
                connection->start(
                    [&](const Message &message) {
                        if (message.type == "message.received") {
                            std::cout
                                << '['
                                << message.payload.at("message_id").get_ref<const std::string &>()
                                << "] " << message.payload.at("text").get_ref<const std::string &>()
                                << '\n';
                        } else if ((message.type == "message.send.result" ||
                                    message.type == "error") &&
                                   message.request_id && pending.erase(*message.request_id) != 0) {
                            if (message.type == "error") {
                                std::cerr << message.payload.at("code") << ": "
                                          << message.payload.at("message") << '\n';
                                exit_code = 1;
                            }
                        } else {
                            connection->close("Unexpected server message");
                        }
                    },
                    [&](const std::string &reason) {
                        input.stop();
                        deadline.cancel();
                        signals.cancel();
                        if (!reason.empty() || !pending.empty()) {
                            std::cerr << "Disconnected: " << reason << " (" << pending.size()
                                      << " pending requests)\n";
                            exit_code = 1;
                        }
                    });
                std::cout << std::unitbuf << "Connected to 127.0.0.1:" << port
                          << " (lobby). /quit to exit.\n";
                input.start();
            });
        context.run();
        return exit_code;
    } catch (const std::exception &error) {
        std::cerr << "Client: " << error.what() << '\n';
        return 1;
    }
}
