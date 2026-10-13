#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulse::storage {

using Id = std::int64_t;
// Stored with millisecond precision
using Timestamp = std::chrono::system_clock::time_point;

inline constexpr int max_page_size = 200;

enum class ErrorKind {
    Conflict,   // unique constraint hit
    NotFound,   // missing row, or a foreign key pointing at nothing
    Invalid,    // CHECK / NOT NULL, or bad arguments
    Busy,       // somebody else held the write lock past the timeout
    Migration,  // schema is broken or newer than this build
    Overloaded, // worker queue is full
    Shutdown,   // worker is stopping
    Internal,
};

class StorageError : public std::runtime_error {
  public:
    StorageError(ErrorKind kind, const std::string &message);
    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }

  private:
    ErrorKind kind_;
};

struct User {
    Id id = 0;
    std::string username;
    std::string password_hash;
    Timestamp created_at;
};

struct Session {
    Id id = 0;
    Id user_id = 0;
    Timestamp created_at;
    Timestamp expires_at;
    std::optional<Timestamp> revoked_at;
};

enum class ConversationKind { Direct, Room };
enum class Role { Owner, Member };

struct Conversation {
    Id id = 0;
    ConversationKind kind = ConversationKind::Room;
    std::optional<std::string> name;
    std::int64_t last_sequence = 0;
    Timestamp created_at;
};

struct Member {
    Id user_id = 0;
    Role role = Role::Member;
    Timestamp joined_at;
};

struct Message {
    Id id = 0;
    Id conversation_id = 0;
    Id sender_id = 0;
    std::string client_message_id;
    std::int64_t sequence = 0;
    std::string body;
    Timestamp created_at;
};

// One SQLite connection. Not thread-safe: keep it on one thread, or put it behind
// DatabaseWorker. Opening runs pending migrations.
//
// Membership and permission checks are not done here. This layer only guarantees
// the data stays consistent; the domain layer decides who may do what.
class Database {
  public:
    // ":memory:" works too. Parent directories are created for file databases.
    explicit Database(const std::filesystem::path &file);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    [[nodiscard]] int schema_version() const;

    // Runs work() in one IMMEDIATE transaction and commits when it returns. Any
    // exception rolls everything back. Nested calls join the outer transaction; if
    // an inner one fails, the outer commit is refused.
    template <class F> auto in_transaction(F &&work) -> std::invoke_result_t<F> {
        begin();
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                work();
                commit();
            } else {
                auto result = work();
                commit();
                return result;
            }
        } catch (...) {
            rollback();
            throw;
        }
    }

    User create_user(std::string_view username, std::string_view password_hash, Timestamp now);
    [[nodiscard]] std::optional<User> find_user(std::string_view username) const;
    [[nodiscard]] std::optional<User> find_user(Id id) const;

    Session create_session(Id user_id, std::span<const std::uint8_t> token_hash, Timestamp now,
                           Timestamp expires_at);
    // Skips revoked and expired sessions
    [[nodiscard]] std::optional<Session>
    find_active_session(std::span<const std::uint8_t> token_hash, Timestamp now) const;
    // False when it was already revoked or doesn't exist
    bool revoke_session(Id session_id, Timestamp now);

    // Creates the room and makes `owner` its first member
    Conversation create_room(std::string_view name, Id owner, Timestamp now);
    // Returns the existing dialog when the pair already has one
    Conversation open_direct(Id first_user, Id second_user, Timestamp now);
    [[nodiscard]] std::optional<Conversation> find_conversation(Id id) const;
    [[nodiscard]] std::optional<Conversation> find_room(std::string_view name) const;

    void add_member(Id conversation, Id user, Role role, Timestamp now);
    bool remove_member(Id conversation, Id user);
    [[nodiscard]] std::optional<Role> role_of(Id conversation, Id user) const;
    [[nodiscard]] std::vector<Member> members(Id conversation) const;

    // Assigns the next sequence and inserts, atomically. A repeated
    // (conversation, sender, client_message_id) throws Conflict and burns no sequence.
    Message append_message(Id conversation, Id sender, std::string_view client_message_id,
                           std::string_view body, Timestamp now);
    [[nodiscard]] std::optional<Message> find_message(Id conversation, Id sender,
                                                      std::string_view client_message_id) const;
    // Newest `limit` messages below `before_sequence` (or the newest overall), oldest first
    [[nodiscard]] std::vector<Message>
    history_before(Id conversation, std::optional<std::int64_t> before_sequence, int limit) const;
    // Up to `limit` messages above `after_sequence`, oldest first
    [[nodiscard]] std::vector<Message> messages_after(Id conversation, std::int64_t after_sequence,
                                                      int limit) const;

    // Consistent snapshot into a new file. Refuses to overwrite and can't run
    // inside a transaction.
    void backup_to(const std::filesystem::path &target);

  private:
    void begin();
    void commit();
    void rollback() noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::storage
