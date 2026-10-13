#include <pulse/connection.hpp>
#include <pulse/database_worker.hpp>

#include "../arguments.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <unordered_set>

namespace asio = boost::asio;
using asio::ip::tcp;
using pulse::Connection;
using pulse::protocol::Message;

namespace {

class Server {
  public:
    Server(asio::io_context &context, unsigned short port)
        : acceptor_(context, {asio::ip::address_v4::loopback(), port}) {
        accept();
    }

    void stop() {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
        // close() also removes it from the set
        while (!sessions_.empty()) {
            (*sessions_.begin())->close();
        }
    }

  private:
    void accept() {
        acceptor_.async_accept([this](boost::system::error_code error, tcp::socket socket) {
            if (!acceptor_.is_open()) {
                return;
            }
            if (error) {
                std::cerr << "Accept failed: " << error.message() << '\n';
                stop();
                throw boost::system::system_error(error);
            }
            auto session = std::make_shared<Connection>(std::move(socket));
            sessions_.insert(session);
            std::cout << "Connected (" << sessions_.size() << " clients)\n";
            session->start(
                [this, weak = std::weak_ptr(session)](const Message &message) {
                    if (auto sender = weak.lock()) {
                        receive(sender, message);
                    }
                },
                [this, weak = std::weak_ptr(session)](const std::string &reason) {
                    if (auto closed = weak.lock()) {
                        sessions_.erase(closed);
                    }
                    std::cout << "Disconnected (" << sessions_.size() << " clients)";
                    if (!reason.empty()) {
                        std::cout << ": " << reason;
                    }
                    std::cout << '\n';
                });
            accept();
        });
    }

    void receive(const std::shared_ptr<Connection> &sender, const Message &message) {
        if (message.type != "message.send") {
            if (message.request_id) {
                sender->send(
                    Message{1,
                            "error",
                            message.request_id,
                            {{"code", "invalid_request"}, {"message", "Expected message.send"}}});
            } else {
                sender->close("Client sent a server event");
            }
            return;
        }
        if (message.payload.at("conversation_id") != "lobby") {
            sender->send(
                Message{1,
                        "error",
                        message.request_id,
                        {{"code", "not_found"}, {"message", "Only the temporary lobby exists"}}});
            return;
        }

        auto payload = message.payload;
        payload["message_id"] = std::to_string(++message_id_);
        try {
            const auto event = std::make_shared<const std::vector<std::uint8_t>>(
                pulse::protocol::encode_frame(Message{1, "message.received", {}, payload}));
            sender->send(Message{1,
                                 "message.send.result",
                                 message.request_id,
                                 {{"conversation_id", "lobby"},
                                  {"client_message_id", payload.at("client_message_id")},
                                  {"message_id", payload.at("message_id")}}});
            // send() can kick a slow reader out of the set
            const std::vector recipients(sessions_.begin(), sessions_.end());
            for (const auto &recipient : recipients) {
                recipient->send(event);
            }
        } catch (const pulse::protocol::ProtocolError &) {
            sender->send(Message{
                1,
                "error",
                message.request_id,
                {{"code", "invalid_request"}, {"message", "Message is too large to broadcast"}}});
        }
    }

    tcp::acceptor acceptor_;
    std::unordered_set<std::shared_ptr<Connection>> sessions_;
    std::uint64_t message_id_ = 0;
};

} // namespace

int main(int argc, char **argv) {
    try {
        const std::string usage = "Usage: pulse-server [port] [--run-for seconds] [--db file]";
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage
                      << "\nListens on 127.0.0.1:5555 by default. Ctrl+C stops the server.\n"
                         "--db opens (and migrates) a SQLite database file at startup.\n";
            return 0;
        }
        unsigned short port = 5555;
        unsigned run_for = 0;
        std::filesystem::path database_path;
        int argument = 1;
        if (argument < argc && std::string_view(argv[argument]).substr(0, 2) != "--") {
            port = static_cast<unsigned short>(pulse::parse_number(argv[argument++], 65535));
        }
        while (argument < argc) {
            const std::string_view option = argv[argument];
            if (argument + 1 >= argc) {
                throw std::invalid_argument(usage);
            }
            if (option == "--run-for") {
                run_for = pulse::parse_number(argv[argument + 1], 86400);
            } else if (option == "--db") {
                database_path = argv[argument + 1];
            } else {
                throw std::invalid_argument(usage);
            }
            argument += 2;
        }

        asio::io_context context;
        // declared after the context so it goes away first, as the worker requires
        std::optional<pulse::storage::DatabaseWorker> database;
        Server server(context, port);
        asio::signal_set signals(context, SIGINT, SIGTERM);
        asio::steady_timer deadline(context);
        const auto stop = [&] {
            server.stop();
            signals.cancel();
            deadline.cancel();
        };
        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) {
                stop();
            }
        });
        if (run_for != 0) {
            deadline.expires_after(std::chrono::seconds(run_for));
            deadline.async_wait([&](boost::system::error_code error) {
                if (!error) {
                    stop();
                }
            });
        }
        std::cout << std::unitbuf;
        if (!database_path.empty()) {
            // opening migrates the schema; a bad path or newer schema fails right here
            database.emplace(context.get_executor(), database_path);
            database->submit([](pulse::storage::Database &db) { return db.schema_version(); },
                             [&, database_path](pulse::storage::Outcome<int> result) {
                                 try {
                                     std::cout << "Database ready: " << database_path.string()
                                               << " (schema version " << result.get() << ")\n";
                                 } catch (const std::exception &error) {
                                     std::cerr << "Database: " << error.what() << '\n';
                                     stop();
                                 }
                             });
        }
        std::cout << "Listening on 127.0.0.1:" << port << " (temporary lobby)\n";
        context.run();
        std::cout << "Server stopped\n";
    } catch (const std::exception &error) {
        std::cerr << "Server: " << error.what() << '\n';
        return 1;
    }
}
