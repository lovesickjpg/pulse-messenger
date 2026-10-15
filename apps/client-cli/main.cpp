#include <pulse/connection.hpp>

#include "../arguments.hpp"
#include "console_input.hpp"

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <unordered_map>

namespace asio = boost::asio;
using asio::ip::tcp;
using pulse::protocol::Message;

namespace {

const char *const usage = "Usage: pulse-client [port] [--host name] [--ca file]";

const char *const help = "Commands:\n"
                         "  /register <name> <password>   create an account\n"
                         "  /login <name> <password>      log in\n"
                         "  /logout                       log out\n"
                         "  /help                         show this\n"
                         "  /quit                         wait for replies, then exit\n"
                         "Anything else goes to the lobby once you're logged in.\n";

struct Options {
    unsigned short port = 5555;
    std::string host = "localhost";
    std::optional<std::filesystem::path> ca;
};

Options parse(int argc, char **argv) {
    Options options;
    int argument = 1;
    if (argument < argc && std::string_view(argv[argument]).substr(0, 2) != "--") {
        options.port = static_cast<unsigned short>(pulse::parse_number(argv[argument++], 65535));
    }
    while (argument < argc) {
        const std::string_view option = argv[argument];
        if (argument + 1 >= argc) {
            throw std::invalid_argument(usage);
        }
        if (option == "--host") {
            options.host = argv[argument + 1];
        } else if (option == "--ca") {
            options.ca = argv[argument + 1];
        } else {
            throw std::invalid_argument(usage);
        }
        argument += 2;
    }
    return options;
}

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

// "/login alice hunter2 hunter3" -> {"alice", "hunter2 hunter3"}; passwords may have spaces
std::optional<std::pair<std::string, std::string>> name_and_password(std::string_view rest) {
    const auto space = rest.find(' ');
    if (space == std::string_view::npos || space == 0 || space + 1 == rest.size()) {
        return std::nullopt;
    }
    return std::pair(std::string(rest.substr(0, space)), std::string(rest.substr(space + 1)));
}

const std::string &field(const Message &message, const char *name) {
    return message.payload.at(name).get_ref<const std::string &>();
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage
                      << "\n\nConnects with TLS to localhost:5555 by default and checks the\n"
                         "server's certificate against --ca (or the system store) and --host.\n\n"
                      << help;
            return 0;
        }
        const auto options = parse(argc, argv);
        auto tls = pulse::tls::client_context(options.ca);
#ifdef _WIN32
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
#endif
        asio::io_context context;
        pulse::tls::Stream stream(context, tls);
        pulse::tls::expect_server_name(stream, options.host);
        tcp::resolver resolver(context);
        asio::steady_timer deadline(context);
        asio::signal_set signals(context, SIGINT, SIGTERM);
        std::shared_ptr<pulse::Connection> connection;
        // request_id -> the result type we're waiting for
        std::unordered_map<std::string, std::string> pending;
        // Only ever in memory. Reconnects will use it later; closing the client means a new login.
        std::optional<std::string> session_token;
        const auto prefix = client_id();
        std::uint64_t sequence = 0;
        bool input_done = false;
        int exit_code = 0;

        const auto request = [&](std::string type, nlohmann::json payload) {
            const auto id = prefix + "-" + std::to_string(++sequence);
            try {
                connection->send(Message{1, type, id, std::move(payload)});
                pending.emplace(id, type + ".result");
            } catch (const pulse::protocol::ProtocolError &error) {
                std::cerr << "Skipped: " << error.what() << '\n';
                exit_code = 1;
            }
        };
        const auto close_when_idle = [&] {
            if (input_done && pending.empty()) {
                connection->close();
            }
        };

        ConsoleInput input(
            context,
            [&](std::string line) {
                if (line.empty()) {
                    return;
                }
                if (line == "/quit") {
                    input.finish();
                } else if (line == "/help") {
                    std::cout << help;
                } else if (line == "/logout") {
                    request("auth.logout", nlohmann::json::object());
                } else if (line.starts_with("/register ") || line.starts_with("/login ")) {
                    const auto space = line.find(' ');
                    const auto credentials =
                        name_and_password(std::string_view(line).substr(space + 1));
                    if (!credentials) {
                        std::cerr << "Usage: " << line.substr(0, space) << " <name> <password>\n";
                        return;
                    }
                    request(line.starts_with("/login") ? "auth.login" : "auth.register",
                            {{"username", credentials->first}, {"password", credentials->second}});
                } else if (line.starts_with('/')) {
                    std::cerr << "Unknown command. Try /help\n";
                } else {
                    const auto id = prefix + "-m" + std::to_string(++sequence);
                    request("message.send", {{"conversation_id", "lobby"},
                                             {"client_message_id", id},
                                             {"text", std::move(line)}});
                }
            },
            [&] {
                input_done = true;
                deadline.expires_after(std::chrono::seconds(10));
                deadline.async_wait([&](boost::system::error_code error) {
                    if (!error) {
                        connection->close("Timed out waiting for replies");
                    }
                });
                // closing cancels the deadline, so arm it first
                close_when_idle();
            });

        const auto on_message = [&](const Message &message) {
            if (message.type == "message.received") {
                std::cout << '[' << field(message, "message_id") << "] " << field(message, "sender")
                          << ": " << field(message, "text") << '\n';
                return;
            }
            const auto it = message.request_id ? pending.find(*message.request_id) : pending.end();
            if (it == pending.end() || (message.type != it->second && message.type != "error")) {
                connection->close("Unexpected server message");
                return;
            }
            pending.erase(it);
            if (message.type == "error") {
                std::cerr << field(message, "code") << ": " << field(message, "message") << '\n';
                exit_code = 1;
            } else if (message.type == "auth.register.result") {
                std::cout << "Registered " << field(message, "username") << ". Now /login.\n";
            } else if (message.type == "auth.login.result") {
                session_token = field(message, "session_token");
                std::cout << "Logged in as " << field(message, "username") << '\n';
            } else if (message.type == "auth.logout.result") {
                session_token.reset();
                std::cout << "Logged out\n";
            }
            close_when_idle();
        };

        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) {
                input.stop();
                deadline.cancel();
                if (connection) {
                    connection->close();
                } else {
                    resolver.cancel();
                    stream.lowest_layer().close();
                }
            }
        });
        deadline.expires_after(std::chrono::seconds(5));
        deadline.async_wait([&](boost::system::error_code error) {
            if (!error) {
                std::cerr << "Connect timeout\n";
                exit_code = 1;
                resolver.cancel();
                stream.lowest_layer().close();
            }
        });

        const auto fail_connect = [&](const boost::system::error_code &error) {
            deadline.cancel();
            std::cerr << "Connect: " << error.message() << '\n';
            exit_code = 1;
            signals.cancel();
        };
        resolver.async_resolve(
            options.host, std::to_string(options.port),
            [&](boost::system::error_code error, const tcp::resolver::results_type &endpoints) {
                if (error) {
                    fail_connect(error);
                    return;
                }
                // The server only listens on IPv4 so far, and Windows takes ~2 s to give up
                // on a refused ::1, so try IPv4 addresses first
                std::vector<tcp::endpoint> ordered(endpoints.begin(), endpoints.end());
                std::stable_partition(
                    ordered.begin(), ordered.end(),
                    [](const tcp::endpoint &item) { return item.address().is_v4(); });
                asio::async_connect(
                    stream.lowest_layer(), ordered,
                    [&](boost::system::error_code error, const tcp::endpoint &) {
                        if (error) {
                            fail_connect(error);
                            return;
                        }
                        // the handshake has its own timeout inside Connection
                        deadline.cancel();
                        connection = std::make_shared<pulse::Connection>(std::move(stream));
                        connection->start(
                            pulse::Connection::Role::client,
                            [&] {
                                std::cout << std::unitbuf << "Connected to " << options.host << ':'
                                          << options.port << " (TLS, certificate verified). "
                                          << "Type /help for commands.\n";
                                input.start();
                            },
                            on_message,
                            [&](const std::string &reason) {
                                input.stop();
                                deadline.cancel();
                                signals.cancel();
                                if (!reason.empty() || !pending.empty()) {
                                    std::cerr << "Disconnected: " << reason << " ("
                                              << pending.size() << " pending requests)\n";
                                    exit_code = 1;
                                }
                            });
                    });
            });
        context.run();
        return exit_code;
    } catch (const std::exception &error) {
        std::cerr << "Client: " << error.what() << '\n';
        return 1;
    }
}
