#include "server.hpp"

#include <iostream>

namespace pulse::server {
namespace asio = boost::asio;
using asio::ip::tcp;
using nlohmann::json;
using protocol::Message;

namespace {

constexpr std::size_t max_backlog = 32;

std::int64_t epoch_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

const std::string &text(const Message &message, const char *field) {
    return message.payload.at(field).get_ref<const std::string &>();
}

} // namespace

Server::Server(asio::io_context &context, asio::ssl::context &tls,
               storage::DatabaseWorker &database, auth::PasswordHasher &hasher, unsigned short port,
               std::chrono::seconds session_ttl)
    : tls_(tls), database_(database), hasher_(hasher), session_ttl_(session_ttl),
      acceptor_(context, {asio::ip::address_v4::loopback(), port}) {
    accept();
}

void Server::stop() {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    // close() also removes it from the map
    while (!clients_.empty()) {
        clients_.begin()->first->close();
    }
}

void Server::accept() {
    acceptor_.async_accept([this](boost::system::error_code error, tcp::socket socket) {
        if (!acceptor_.is_open()) {
            return;
        }
        if (error) {
            std::cerr << "Accept failed: " << error.message() << '\n';
            stop();
            throw boost::system::system_error(error);
        }
        auto peer = std::make_shared<Connection>(tls::Stream(std::move(socket), tls_));
        clients_.emplace(peer, Client{});
        std::cout << "Connected (" << clients_.size() << " clients)\n";
        peer->start(
            Connection::Role::server, {},
            [this, weak = Weak(peer)](const Message &message) {
                if (auto sender = weak.lock()) {
                    receive(sender, message);
                }
            },
            [this, weak = Weak(peer)](const std::string &reason) {
                if (auto closed = weak.lock()) {
                    clients_.erase(closed);
                }
                std::cout << "Disconnected (" << clients_.size() << " clients)";
                if (!reason.empty()) {
                    std::cout << ": " << reason;
                }
                std::cout << '\n';
            });
        accept();
    });
}

Server::Client *Server::client_of(const Weak &weak, Peer &peer) {
    peer = weak.lock();
    if (!peer) {
        return nullptr;
    }
    const auto it = clients_.find(peer);
    return it == clients_.end() ? nullptr : &it->second;
}

void Server::receive(const Peer &peer, const Message &message) {
    auto &client = clients_.at(peer);
    if (!client.busy) {
        handle(peer, message);
        return;
    }
    if (client.backlog.size() >= max_backlog) {
        peer->close("Too many requests waiting on one connection");
        return;
    }
    client.backlog.push_back(message);
}

void Server::start_async(const Peer &peer) { clients_.at(peer).busy = true; }

void Server::finish_async(const Weak &weak) {
    Peer peer;
    auto *client = client_of(weak, peer);
    if (client == nullptr) {
        return;
    }
    client->busy = false;
    // handle() may go async again, or even close the connection under us
    while (clients_.contains(peer)) {
        auto &current = clients_.at(peer);
        if (current.busy || current.backlog.empty()) {
            break;
        }
        const auto next = std::move(current.backlog.front());
        current.backlog.pop_front();
        handle(peer, next);
    }
}

void Server::handle(const Peer &peer, const Message &request) {
    if (request.type == "auth.register") {
        register_user(peer, request);
    } else if (request.type == "auth.login") {
        login(peer, request);
    } else if (request.type == "auth.resume") {
        resume(peer, request);
    } else if (request.type == "auth.logout") {
        logout(peer, request);
    } else if (request.type == "message.send") {
        send_to_lobby(peer, request);
    } else if (request.request_id) {
        fail(peer, request, "invalid_request", "Clients can't send " + request.type);
    } else {
        peer->close("Client sent a server event");
    }
}

void Server::reply(const Peer &peer, const Message &request, std::string type, json payload) {
    peer->send(Message{1, std::move(type), request.request_id, std::move(payload)});
}

void Server::fail(const Peer &peer, const Message &request, const std::string &code,
                  const std::string &message) {
    reply(peer, request, "error", {{"code", code}, {"message", message}});
}

void Server::fail(const Peer &peer, const Message &request, std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const storage::StorageError &failure) {
        if (failure.kind() == storage::ErrorKind::Overloaded) {
            fail(peer, request, "rate_limited", "Server is busy, try again in a moment");
            return;
        }
        std::cerr << "Database error during " << request.type << ": " << failure.what() << '\n';
    } catch (const auth::HasherError &failure) {
        if (failure.kind() == auth::HasherError::Kind::Overloaded) {
            fail(peer, request, "rate_limited", "Server is busy, try again in a moment");
            return;
        }
    } catch (const std::exception &failure) {
        std::cerr << "Error during " << request.type << ": " << failure.what() << '\n';
    }
    fail(peer, request, "internal_error", "Something went wrong on the server");
}

void Server::register_user(const Peer &peer, const Message &request) {
    const auto &username = text(request, "username");
    const auto &password = text(request, "password");
    if (const auto problem = auth::username_problem(username)) {
        fail(peer, request, "invalid_request", *problem);
        return;
    }
    if (const auto problem = auth::password_problem(password)) {
        fail(peer, request, "invalid_request", *problem);
        return;
    }

    start_async(peer);
    hasher_.hash(password, [this, weak = Weak(peer), request,
                            username](Outcome<std::string> hashed) mutable {
        Peer peer;
        if (client_of(weak, peer) == nullptr) {
            return;
        }
        if (!hashed.ok()) {
            fail(peer, request, hashed.error());
            finish_async(weak);
            return;
        }
        database_.submit(
            [username, hash = std::move(hashed.get())](storage::Database &db) {
                return db.create_user(username, hash, Clock::now());
            },
            [this, weak, request](Outcome<storage::User> created) {
                Peer peer;
                if (client_of(weak, peer) == nullptr) {
                    return;
                }
                try {
                    const auto &user = created.get();
                    std::cout << "Registered " << user.username << '\n';
                    reply(peer, request, "auth.register.result",
                          {{"user_id", std::to_string(user.id)}, {"username", user.username}});
                } catch (const storage::StorageError &error) {
                    if (error.kind() == storage::ErrorKind::Conflict) {
                        fail(peer, request, "conflict", "That username is taken");
                    } else {
                        fail(peer, request, std::current_exception());
                    }
                } catch (...) {
                    fail(peer, request, std::current_exception());
                }
                finish_async(weak);
            });
    });
}

void Server::login(const Peer &peer, const Message &request) {
    const auto &username = text(request, "username");
    const auto &password = text(request, "password");
    // Don't even hash a 10 MB "password"
    if (auth::username_problem(username) || auth::password_problem(password)) {
        fail(peer, request, "unauthorized", "Wrong username or password");
        return;
    }
    const auto key = auth::username_key(username);
    if (throttle_.blocked(key, auth::LoginThrottle::Clock::now())) {
        fail(peer, request, "rate_limited", "Too many failed logins, try again in a minute");
        return;
    }

    start_async(peer);
    database_.submit(
        [username](storage::Database &db) { return db.find_user(username); },
        [this, weak = Weak(peer), request, key,
         password](Outcome<std::optional<storage::User>> found) {
            Peer peer;
            if (client_of(weak, peer) == nullptr) {
                return;
            }
            if (!found.ok()) {
                fail(peer, request, found.error());
                finish_async(weak);
                return;
            }
            auto user = found.get();
            // Unknown users still pay for a full hash, so timing can't tell them apart
            hasher_.verify(
                user ? std::optional(user->password_hash) : std::nullopt, password,
                [this, weak, request, key, user](Outcome<bool> verified) {
                    Peer peer;
                    if (client_of(weak, peer) == nullptr) {
                        return;
                    }
                    if (!verified.ok()) {
                        fail(peer, request, verified.error());
                        finish_async(weak);
                        return;
                    }
                    if (!verified.get()) {
                        throttle_.failed(key, auth::LoginThrottle::Clock::now());
                        fail(peer, request, "unauthorized", "Wrong username or password");
                        finish_async(weak);
                        return;
                    }
                    throttle_.succeeded(key);
                    auto token = auth::new_token();
                    const auto now = Clock::now();
                    database_.submit(
                        [user_id = user->id, hash = token.hash, now,
                         expires = now + session_ttl_](storage::Database &db) {
                            return db.create_session(user_id, hash, now, expires);
                        },
                        [this, weak, request, user = *user,
                         token = std::move(token.text)](Outcome<storage::Session> created) {
                            Peer peer;
                            if (client_of(weak, peer) == nullptr) {
                                return;
                            }
                            if (created.ok()) {
                                sign_in(peer, request, user, created.get(), token);
                            } else {
                                fail(peer, request, created.error());
                            }
                            finish_async(weak);
                        });
                });
        });
}

void Server::resume(const Peer &peer, const Message &request) {
    const auto hash = auth::token_hash(text(request, "session_token"));
    if (!hash) {
        fail(peer, request, "unauthorized", "Session is invalid or expired");
        return;
    }
    using Found = std::optional<std::pair<storage::Session, storage::User>>;
    start_async(peer);
    database_.submit(
        [hash = *hash](storage::Database &db) -> Found {
            auto session = db.find_active_session(hash, Clock::now());
            if (!session) {
                return std::nullopt;
            }
            auto user = db.find_user(session->user_id);
            if (!user) {
                return std::nullopt;
            }
            return std::pair(*session, *user);
        },
        [this, weak = Weak(peer), request](Outcome<Found> found) {
            Peer peer;
            if (client_of(weak, peer) == nullptr) {
                return;
            }
            if (!found.ok()) {
                fail(peer, request, found.error());
            } else if (!found.get()) {
                fail(peer, request, "unauthorized", "Session is invalid or expired");
            } else {
                const auto &[session, user] = *found.get();
                sign_in(peer, request, user, session, std::nullopt);
            }
            finish_async(weak);
        });
}

void Server::sign_in(const Peer &peer, const Message &request, const storage::User &user,
                     const storage::Session &session, const std::optional<std::string> &token) {
    clients_.at(peer).identity = Identity{user.id, user.username, session.id, session.expires_at};
    json payload{{"user_id", std::to_string(user.id)},
                 {"username", user.username},
                 {"expires_at", epoch_ms(session.expires_at)}};
    if (token) {
        payload["session_token"] = *token;
        std::cout << user.username << " logged in\n";
    } else {
        std::cout << user.username << " resumed a session\n";
    }
    reply(peer, request, token ? "auth.login.result" : "auth.resume.result", std::move(payload));
}

void Server::logout(const Peer &peer, const Message &request) {
    const auto identity = clients_.at(peer).identity;
    if (!identity) {
        fail(peer, request, "unauthorized", "You're not logged in");
        return;
    }
    // Sign out every connection on this session right now, before the database even
    // hears about it, so nothing queued behind the logout slips through
    for (auto &[other, client] : clients_) {
        if (client.identity && client.identity->session_id == identity->session_id) {
            client.identity.reset();
        }
    }
    std::cout << identity->username << " logged out\n";

    start_async(peer);
    database_.submit(
        [session_id = identity->session_id](storage::Database &db) {
            return db.revoke_session(session_id, Clock::now());
        },
        [this, weak = Weak(peer), request](Outcome<bool> revoked) {
            Peer peer;
            if (client_of(weak, peer) == nullptr) {
                return;
            }
            if (revoked.ok()) {
                reply(peer, request, "auth.logout.result", json::object());
            } else {
                fail(peer, request, revoked.error());
            }
            finish_async(weak);
        });
}

void Server::send_to_lobby(const Peer &peer, const Message &request) {
    auto &identity = clients_.at(peer).identity;
    if (!identity) {
        fail(peer, request, "unauthorized", "Log in first");
        return;
    }
    if (Clock::now() >= identity->expires_at) {
        identity.reset();
        fail(peer, request, "unauthorized", "Your session expired, log in again");
        return;
    }
    if (text(request, "conversation_id") != "lobby") {
        fail(peer, request, "not_found", "Only the temporary lobby exists");
        return;
    }

    auto payload = request.payload;
    payload["message_id"] = std::to_string(++message_id_);
    payload["sender"] = identity->username;
    try {
        const auto event = std::make_shared<const std::vector<std::uint8_t>>(
            protocol::encode_frame(Message{1, "message.received", {}, payload}));
        reply(peer, request, "message.send.result",
              {{"conversation_id", "lobby"},
               {"client_message_id", payload.at("client_message_id")},
               {"message_id", payload.at("message_id")}});
        // send() can kick a slow reader out of the map, so copy the list first
        std::vector<Peer> recipients;
        for (const auto &[other, client] : clients_) {
            if (client.identity) {
                recipients.push_back(other);
            }
        }
        for (const auto &recipient : recipients) {
            recipient->send(event);
        }
    } catch (const protocol::ProtocolError &) {
        fail(peer, request, "invalid_request", "Message is too large to broadcast");
    }
}

} // namespace pulse::server
