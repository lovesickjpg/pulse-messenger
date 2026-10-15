#pragma once

#include <pulse/auth.hpp>
#include <pulse/connection.hpp>
#include <pulse/database_worker.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace pulse::server {

// Accepts TLS connections on loopback, handles auth, and runs the temporary lobby.
// Lives on the io_context thread; the database and the hasher call back onto it.
class Server {
  public:
    Server(boost::asio::io_context &context, boost::asio::ssl::context &tls,
           storage::DatabaseWorker &database, auth::PasswordHasher &hasher, unsigned short port,
           std::chrono::seconds session_ttl);

    void stop();

  private:
    using Peer = std::shared_ptr<Connection>;
    using Weak = std::weak_ptr<Connection>;
    using Clock = std::chrono::system_clock;

    // Who the server decided this connection is. Never taken from the client's JSON.
    struct Identity {
        storage::Id user_id = 0;
        std::string username;
        storage::Id session_id = 0;
        Clock::time_point expires_at;
    };

    struct Client {
        std::optional<Identity> identity;
        // Requests wait here while an earlier one is still hashing or in the database,
        // so "/login, then send" on one connection always happens in that order
        std::deque<protocol::Message> backlog;
        bool busy = false;
    };

    void accept();
    void receive(const Peer &peer, const protocol::Message &message);
    void handle(const Peer &peer, const protocol::Message &request);
    void start_async(const Peer &peer);
    void finish_async(const Weak &weak);
    Client *client_of(const Weak &weak, Peer &peer);

    void register_user(const Peer &peer, const protocol::Message &request);
    void login(const Peer &peer, const protocol::Message &request);
    void resume(const Peer &peer, const protocol::Message &request);
    void logout(const Peer &peer, const protocol::Message &request);
    void send_to_lobby(const Peer &peer, const protocol::Message &request);
    void sign_in(const Peer &peer, const protocol::Message &request, const storage::User &user,
                 const storage::Session &session, const std::optional<std::string> &token);

    void reply(const Peer &peer, const protocol::Message &request, std::string type,
               nlohmann::json payload);
    void fail(const Peer &peer, const protocol::Message &request, const std::string &code,
              const std::string &message);
    // Turns a failed background job into the right protocol error
    void fail(const Peer &peer, const protocol::Message &request, std::exception_ptr error);

    boost::asio::ssl::context &tls_;
    storage::DatabaseWorker &database_;
    auth::PasswordHasher &hasher_;
    std::chrono::seconds session_ttl_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::unordered_map<Peer, Client> clients_;
    auth::LoginThrottle throttle_;
    std::uint64_t message_id_ = 0;
};

} // namespace pulse::server
