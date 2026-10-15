#include "server.hpp"

#include "../arguments.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>

namespace asio = boost::asio;

namespace {

const char *const usage =
    "Usage: pulse-server [port] --cert file --key file [--db file] [--run-for seconds]\n"
    "                    [--session-ttl seconds] [--password-cost interactive|moderate|minimal]";

struct Options {
    unsigned short port = 5555;
    std::filesystem::path certificate;
    std::filesystem::path key;
    std::filesystem::path database = ":memory:";
    unsigned run_for = 0;
    std::chrono::seconds session_ttl = std::chrono::hours(12);
    pulse::auth::Cost password_cost = pulse::auth::Cost::Interactive;
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
        const char *value = argv[argument + 1];
        if (option == "--cert") {
            options.certificate = value;
        } else if (option == "--key") {
            options.key = value;
        } else if (option == "--db") {
            options.database = value;
        } else if (option == "--run-for") {
            options.run_for = pulse::parse_number(value, 86400);
        } else if (option == "--session-ttl") {
            options.session_ttl = std::chrono::seconds(pulse::parse_number(value, 30 * 86400));
        } else if (option == "--password-cost") {
            const auto cost = pulse::auth::parse_cost(value);
            if (!cost) {
                throw std::invalid_argument(usage);
            }
            options.password_cost = *cost;
        } else {
            throw std::invalid_argument(usage);
        }
        argument += 2;
    }
    if (options.certificate.empty() || options.key.empty()) {
        throw std::invalid_argument(
            "TLS is required: pass --cert and --key (pulse-dev-certs makes a local pair)\n" +
            std::string(usage));
    }
    return options;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage
                      << "\n\nListens with TLS on 127.0.0.1:5555 by default. Ctrl+C stops it.\n"
                         "--db keeps accounts in a SQLite file; without it they live in memory.\n"
                         "--session-ttl is how long a login lasts (default 12 hours).\n"
                         "--password-cost minimal is for tests only; it makes hashes cheap to "
                         "crack.\n";
            return 0;
        }
        const auto options = parse(argc, argv);
        auto tls = pulse::tls::server_context(options.certificate, options.key);

        std::cout << std::unitbuf;
        if (options.database == ":memory:") {
            std::cerr << "No --db given: accounts live in memory and vanish when the server "
                         "stops\n";
        }
        if (options.password_cost == pulse::auth::Cost::Minimal) {
            std::cerr << "Password hashing is set to minimal. Fine for tests, never for real "
                         "users.\n";
        }

        asio::io_context context;
        // Declared after the context so they go away before it, as both require.
        // Opening the database migrates it; a bad path or newer schema fails right here.
        pulse::storage::DatabaseWorker database(context.get_executor(), options.database);
        pulse::auth::PasswordHasher hasher(context.get_executor(), options.password_cost);
        pulse::server::Server server(context, tls, database, hasher, options.port,
                                     options.session_ttl);

        asio::signal_set signals(context, SIGINT, SIGTERM);
        asio::steady_timer deadline(context);
        const auto stop = [&] {
            server.stop();
            // queued hashes and queries fail fast instead of holding up the exit
            hasher.stop();
            database.stop();
            signals.cancel();
            deadline.cancel();
        };
        signals.async_wait([&](boost::system::error_code error, int) {
            if (!error) {
                stop();
            }
        });
        if (options.run_for != 0) {
            deadline.expires_after(std::chrono::seconds(options.run_for));
            deadline.async_wait([&](boost::system::error_code error) {
                if (!error) {
                    stop();
                }
            });
        }
        database.submit([](pulse::storage::Database &db) { return db.schema_version(); },
                        [&](pulse::Outcome<int> result) {
                            try {
                                std::cout << "Database ready: " << options.database.string()
                                          << " (schema version " << result.get() << ")\n";
                            } catch (const std::exception &error) {
                                std::cerr << "Database: " << error.what() << '\n';
                                stop();
                            }
                        });
        std::cout << "Listening on 127.0.0.1:" << options.port << " (TLS, temporary lobby)\n";
        context.run();
        std::cout << "Server stopped\n";
    } catch (const std::exception &error) {
        std::cerr << "Server: " << error.what() << '\n';
        return 1;
    }
}
