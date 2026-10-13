#include <pulse/storage.hpp>

#include "migrations.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace pulse::storage {
namespace {

using std::chrono::duration_cast;
using std::chrono::milliseconds;

// Plain numbers from sqlite3.h so we don't depend on how SQLiteCpp exposes them
constexpr int sqlite_busy = 5;
constexpr int sqlite_locked = 6;
constexpr int sqlite_constraint = 19;
constexpr int constraint_check = 275;
constexpr int constraint_foreign_key = 787;
constexpr int constraint_not_null = 1299;
constexpr int constraint_primary_key = 1555;
constexpr int constraint_unique = 2067;

constexpr int busy_timeout_ms = 5000;

std::int64_t to_ms(Timestamp time) {
    return duration_cast<milliseconds>(time.time_since_epoch()).count();
}

Timestamp from_ms(std::int64_t value) { return Timestamp(milliseconds(value)); }

std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

StorageError translate(const SQLite::Exception &error) {
    ErrorKind kind = ErrorKind::Internal;
    switch (error.getExtendedErrorCode()) {
    case constraint_unique:
    case constraint_primary_key:
        kind = ErrorKind::Conflict;
        break;
    case constraint_foreign_key:
        kind = ErrorKind::NotFound;
        break;
    case constraint_check:
    case constraint_not_null:
        kind = ErrorKind::Invalid;
        break;
    default:
        if (error.getErrorCode() == sqlite_busy || error.getErrorCode() == sqlite_locked) {
            kind = ErrorKind::Busy;
        } else if (error.getErrorCode() == sqlite_constraint) {
            kind = ErrorKind::Invalid;
        }
    }
    return {kind, error.what()};
}

// Every public call goes through this so callers only ever see StorageError
template <class F> decltype(auto) guarded(F &&work) {
    try {
        return std::forward<F>(work)();
    } catch (const SQLite::Exception &error) {
        throw translate(error);
    }
}

int clamp_limit(int limit) {
    if (limit < 1) {
        throw StorageError(ErrorKind::Invalid, "Page size must be at least 1");
    }
    return std::min(limit, max_page_size);
}

const char *role_text(Role role) { return role == Role::Owner ? "owner" : "member"; }

Role role_from(const std::string &text) { return text == "owner" ? Role::Owner : Role::Member; }

User read_user(SQLite::Statement &row) {
    return {row.getColumn(0).getInt64(), row.getColumn(1).getString(), row.getColumn(2).getString(),
            from_ms(row.getColumn(3).getInt64())};
}

Conversation read_conversation(SQLite::Statement &row) {
    Conversation conversation;
    conversation.id = row.getColumn(0).getInt64();
    conversation.kind = row.getColumn(1).getString() == "direct" ? ConversationKind::Direct
                                                                 : ConversationKind::Room;
    if (!row.getColumn(2).isNull()) {
        conversation.name = row.getColumn(2).getString();
    }
    conversation.last_sequence = row.getColumn(3).getInt64();
    conversation.created_at = from_ms(row.getColumn(4).getInt64());
    return conversation;
}

Message read_message(SQLite::Statement &row) {
    return {row.getColumn(0).getInt64(),         row.getColumn(1).getInt64(),
            row.getColumn(2).getInt64(),         row.getColumn(3).getString(),
            row.getColumn(4).getInt64(),         row.getColumn(5).getString(),
            from_ms(row.getColumn(6).getInt64())};
}

constexpr const char *message_columns =
    "id, conversation_id, sender_id, client_message_id, sequence, body, created_at";
constexpr const char *conversation_columns = "id, kind, name, last_sequence, created_at";

} // namespace

StorageError::StorageError(ErrorKind kind, const std::string &message)
    : std::runtime_error(message), kind_(kind) {}

struct Database::Impl {
    explicit Impl(const std::string &path)
        : db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE, busy_timeout_ms) {}

    SQLite::Database db;
    int depth = 0;
    bool rollback_only = false;
};

namespace {

void configure(SQLite::Database &db) {
    db.exec("PRAGMA foreign_keys = ON");
    // Some builds silently ignore this, and then none of our REFERENCES would mean anything
    if (db.execAndGet("PRAGMA foreign_keys").getInt() != 1) {
        throw StorageError(ErrorKind::Internal, "SQLite foreign key support is unavailable");
    }
    // WAL lets readers keep going during a write. FULL means a commit is on disk
    // before we tell anyone the message was saved.
    db.exec("PRAGMA journal_mode = WAL");
    db.exec("PRAGMA synchronous = FULL");
}

int applied_version(SQLite::Database &db) {
    return db.execAndGet("SELECT COALESCE(MAX(version), 0) FROM schema_migrations").getInt();
}

void migrate(SQLite::Database &db) {
    const auto known = migrations();
    for (std::size_t index = 0; index < known.size(); ++index) {
        // Gaps or reordering would mean a broken build, so fail loudly
        if (known[index].version != static_cast<int>(index) + 1) {
            throw StorageError(ErrorKind::Migration, "Migrations must be numbered 1..N");
        }
    }

    db.exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
            "version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at INTEGER NOT NULL) STRICT");

    const int current = applied_version(db);
    const int newest = known.empty() ? 0 : known.back().version;
    if (current > newest) {
        throw StorageError(ErrorKind::Migration, "Database schema version " +
                                                     std::to_string(current) +
                                                     " is newer than this build supports (" +
                                                     std::to_string(newest) + ")");
    }

    for (const auto &migration : known) {
        if (migration.version <= current) {
            continue;
        }
        try {
            SQLite::Transaction transaction(db, SQLite::TransactionBehavior::IMMEDIATE);
            // Another process might have won the race while we waited for the lock
            if (applied_version(db) >= migration.version) {
                continue;
            }
            db.exec(std::string(migration.sql));
            SQLite::Statement record(
                db, "INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, ?, ?)");
            record.bind(1, migration.version);
            record.bind(2, std::string(migration.name));
            record.bind(3, to_ms(std::chrono::system_clock::now()));
            record.exec();
            transaction.commit();
        } catch (const SQLite::Exception &error) {
            throw StorageError(ErrorKind::Migration, "Migration " + std::string(migration.name) +
                                                         " failed: " + error.what());
        }
    }
}

} // namespace

Database::Database(const std::filesystem::path &file) {
    guarded([&] {
        const bool in_memory = file == ":memory:";
        if (!in_memory && file.has_parent_path()) {
            std::error_code ignored;
            std::filesystem::create_directories(file.parent_path(), ignored);
        }
        impl_ = std::make_unique<Impl>(in_memory ? std::string(":memory:") : utf8(file));
        configure(impl_->db);
        migrate(impl_->db);
    });
}

Database::~Database() = default;

int Database::schema_version() const {
    return guarded([&] { return applied_version(impl_->db); });
}

void Database::begin() {
    guarded([&] {
        if (impl_->depth == 0) {
            impl_->db.exec("BEGIN IMMEDIATE");
        }
        ++impl_->depth;
    });
}

void Database::commit() {
    auto &state = *impl_;
    if (state.depth > 1) {
        --state.depth;
        return;
    }
    state.depth = 0;
    const bool doomed = std::exchange(state.rollback_only, false);
    try {
        guarded([&] { state.db.exec(doomed ? "ROLLBACK" : "COMMIT"); });
    } catch (...) {
        try {
            state.db.exec("ROLLBACK");
        } catch (...) {
            // already failed once; SQLite rolls back whatever is left when the connection closes
        }
        throw;
    }
    if (doomed) {
        throw StorageError(ErrorKind::Internal,
                           "A nested transaction failed, so the outer one was rolled back");
    }
}

void Database::rollback() noexcept {
    auto &state = *impl_;
    if (state.depth == 0) {
        // commit() already closed the transaction before it threw
        return;
    }
    state.rollback_only = true;
    if (--state.depth > 0) {
        return;
    }
    state.rollback_only = false;
    try {
        state.db.exec("ROLLBACK");
    } catch (...) {
        // the caller is already unwinding with the real error
    }
}

User Database::create_user(std::string_view username, std::string_view password_hash,
                           Timestamp now) {
    return guarded([&] {
        auto &db = impl_->db;
        SQLite::Statement insert(
            db, "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)");
        insert.bind(1, std::string(username));
        insert.bind(2, std::string(password_hash));
        insert.bind(3, to_ms(now));
        insert.exec();
        return User{db.getLastInsertRowid(), std::string(username), std::string(password_hash),
                    from_ms(to_ms(now))};
    });
}

std::optional<User> Database::find_user(std::string_view username) const {
    return guarded([&]() -> std::optional<User> {
        SQLite::Statement query(
            impl_->db,
            "SELECT id, username, password_hash, created_at FROM users WHERE username = ?");
        query.bind(1, std::string(username));
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return read_user(query);
    });
}

std::optional<User> Database::find_user(Id id) const {
    return guarded([&]() -> std::optional<User> {
        SQLite::Statement query(
            impl_->db, "SELECT id, username, password_hash, created_at FROM users WHERE id = ?");
        query.bind(1, id);
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return read_user(query);
    });
}

Session Database::create_session(Id user_id, std::span<const std::uint8_t> token_hash,
                                 Timestamp now, Timestamp expires_at) {
    return guarded([&] {
        auto &db = impl_->db;
        SQLite::Statement insert(db, "INSERT INTO sessions (user_id, token_hash, created_at, "
                                     "expires_at) VALUES (?, ?, ?, ?)");
        insert.bind(1, user_id);
        insert.bind(2, token_hash.data(), static_cast<int>(token_hash.size()));
        insert.bind(3, to_ms(now));
        insert.bind(4, to_ms(expires_at));
        insert.exec();
        return Session{db.getLastInsertRowid(), user_id, from_ms(to_ms(now)),
                       from_ms(to_ms(expires_at)), std::nullopt};
    });
}

std::optional<Session> Database::find_active_session(std::span<const std::uint8_t> token_hash,
                                                     Timestamp now) const {
    return guarded([&]() -> std::optional<Session> {
        SQLite::Statement query(impl_->db,
                                "SELECT id, user_id, created_at, expires_at FROM sessions "
                                "WHERE token_hash = ? AND revoked_at IS NULL AND expires_at > ?");
        query.bind(1, token_hash.data(), static_cast<int>(token_hash.size()));
        query.bind(2, to_ms(now));
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return Session{query.getColumn(0).getInt64(), query.getColumn(1).getInt64(),
                       from_ms(query.getColumn(2).getInt64()),
                       from_ms(query.getColumn(3).getInt64()), std::nullopt};
    });
}

bool Database::revoke_session(Id session_id, Timestamp now) {
    return guarded([&] {
        SQLite::Statement update(
            impl_->db, "UPDATE sessions SET revoked_at = ? WHERE id = ? AND revoked_at IS NULL");
        update.bind(1, to_ms(now));
        update.bind(2, session_id);
        return update.exec() > 0;
    });
}

Conversation Database::create_room(std::string_view name, Id owner, Timestamp now) {
    return in_transaction([&] {
        return guarded([&] {
            auto &db = impl_->db;
            SQLite::Statement insert(
                db, "INSERT INTO conversations (kind, name, created_at) VALUES ('room', ?, ?)");
            insert.bind(1, std::string(name));
            insert.bind(2, to_ms(now));
            insert.exec();
            Conversation room;
            room.id = db.getLastInsertRowid();
            room.kind = ConversationKind::Room;
            room.name = std::string(name);
            room.created_at = from_ms(to_ms(now));
            add_member(room.id, owner, Role::Owner, now);
            return room;
        });
    });
}

Conversation Database::open_direct(Id first_user, Id second_user, Timestamp now) {
    if (first_user == second_user) {
        throw StorageError(ErrorKind::Invalid, "A direct dialog needs two different users");
    }
    const std::string key = std::to_string(std::min(first_user, second_user)) + ":" +
                            std::to_string(std::max(first_user, second_user));
    return in_transaction([&] {
        return guarded([&] {
            auto &db = impl_->db;
            {
                SQLite::Statement existing(db, std::string("SELECT ") + conversation_columns +
                                                   " FROM conversations WHERE direct_key = ?");
                existing.bind(1, key);
                if (existing.executeStep()) {
                    return read_conversation(existing);
                }
            }
            SQLite::Statement insert(db, "INSERT INTO conversations (kind, direct_key, created_at) "
                                         "VALUES ('direct', ?, ?)");
            insert.bind(1, key);
            insert.bind(2, to_ms(now));
            insert.exec();
            Conversation dialog;
            dialog.id = db.getLastInsertRowid();
            dialog.kind = ConversationKind::Direct;
            dialog.created_at = from_ms(to_ms(now));
            add_member(dialog.id, first_user, Role::Member, now);
            add_member(dialog.id, second_user, Role::Member, now);
            return dialog;
        });
    });
}

std::optional<Conversation> Database::find_conversation(Id id) const {
    return guarded([&]() -> std::optional<Conversation> {
        SQLite::Statement query(impl_->db, std::string("SELECT ") + conversation_columns +
                                               " FROM conversations WHERE id = ?");
        query.bind(1, id);
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return read_conversation(query);
    });
}

std::optional<Conversation> Database::find_room(std::string_view name) const {
    return guarded([&]() -> std::optional<Conversation> {
        SQLite::Statement query(impl_->db,
                                std::string("SELECT ") + conversation_columns +
                                    " FROM conversations WHERE kind = 'room' AND name = ?");
        query.bind(1, std::string(name));
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return read_conversation(query);
    });
}

void Database::add_member(Id conversation, Id user, Role role, Timestamp now) {
    guarded([&] {
        SQLite::Statement insert(impl_->db, "INSERT INTO conversation_members "
                                            "(conversation_id, user_id, role, joined_at) "
                                            "VALUES (?, ?, ?, ?)");
        insert.bind(1, conversation);
        insert.bind(2, user);
        insert.bind(3, std::string(role_text(role)));
        insert.bind(4, to_ms(now));
        insert.exec();
    });
}

bool Database::remove_member(Id conversation, Id user) {
    return guarded([&] {
        SQLite::Statement remove(
            impl_->db,
            "DELETE FROM conversation_members WHERE conversation_id = ? AND user_id = ?");
        remove.bind(1, conversation);
        remove.bind(2, user);
        return remove.exec() > 0;
    });
}

std::optional<Role> Database::role_of(Id conversation, Id user) const {
    return guarded([&]() -> std::optional<Role> {
        SQLite::Statement query(
            impl_->db,
            "SELECT role FROM conversation_members WHERE conversation_id = ? AND user_id = ?");
        query.bind(1, conversation);
        query.bind(2, user);
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return role_from(query.getColumn(0).getString());
    });
}

std::vector<Member> Database::members(Id conversation) const {
    return guarded([&] {
        SQLite::Statement query(impl_->db,
                                "SELECT user_id, role, joined_at FROM conversation_members "
                                "WHERE conversation_id = ? ORDER BY joined_at, user_id");
        query.bind(1, conversation);
        std::vector<Member> result;
        while (query.executeStep()) {
            result.push_back({query.getColumn(0).getInt64(),
                              role_from(query.getColumn(1).getString()),
                              from_ms(query.getColumn(2).getInt64())});
        }
        return result;
    });
}

Message Database::append_message(Id conversation, Id sender, std::string_view client_message_id,
                                 std::string_view body, Timestamp now) {
    return in_transaction([&] {
        return guarded([&] {
            auto &db = impl_->db;
            std::int64_t sequence = 0;
            {
                SQLite::Statement bump(db,
                                       "UPDATE conversations SET last_sequence = "
                                       "last_sequence + 1 WHERE id = ? RETURNING last_sequence");
                bump.bind(1, conversation);
                if (!bump.executeStep()) {
                    throw StorageError(ErrorKind::NotFound, "Conversation does not exist");
                }
                sequence = bump.getColumn(0).getInt64();
            }
            SQLite::Statement insert(
                db, "INSERT INTO messages (conversation_id, sender_id, client_message_id, "
                    "sequence, body, created_at) VALUES (?, ?, ?, ?, ?, ?)");
            insert.bind(1, conversation);
            insert.bind(2, sender);
            insert.bind(3, std::string(client_message_id));
            insert.bind(4, sequence);
            insert.bind(5, std::string(body));
            insert.bind(6, to_ms(now));
            insert.exec();
            return Message{db.getLastInsertRowid(),
                           conversation,
                           sender,
                           std::string(client_message_id),
                           sequence,
                           std::string(body),
                           from_ms(to_ms(now))};
        });
    });
}

std::optional<Message> Database::find_message(Id conversation, Id sender,
                                              std::string_view client_message_id) const {
    return guarded([&]() -> std::optional<Message> {
        SQLite::Statement query(impl_->db,
                                std::string("SELECT ") + message_columns +
                                    " FROM messages WHERE conversation_id = ? AND sender_id = ? "
                                    "AND client_message_id = ?");
        query.bind(1, conversation);
        query.bind(2, sender);
        query.bind(3, std::string(client_message_id));
        if (!query.executeStep()) {
            return std::nullopt;
        }
        return read_message(query);
    });
}

std::vector<Message> Database::history_before(Id conversation,
                                              std::optional<std::int64_t> before_sequence,
                                              int limit) const {
    const int count = clamp_limit(limit);
    return guarded([&] {
        SQLite::Statement query(impl_->db,
                                std::string("SELECT ") + message_columns +
                                    " FROM messages WHERE conversation_id = ? AND sequence < ? "
                                    "ORDER BY sequence DESC LIMIT ?");
        query.bind(1, conversation);
        query.bind(2, before_sequence.value_or(std::numeric_limits<std::int64_t>::max()));
        query.bind(3, count);
        std::vector<Message> result;
        while (query.executeStep()) {
            result.push_back(read_message(query));
        }
        // We read newest-first to get the right page, but callers want chronological order
        std::reverse(result.begin(), result.end());
        return result;
    });
}

std::vector<Message> Database::messages_after(Id conversation, std::int64_t after_sequence,
                                              int limit) const {
    const int count = clamp_limit(limit);
    return guarded([&] {
        SQLite::Statement query(impl_->db,
                                std::string("SELECT ") + message_columns +
                                    " FROM messages WHERE conversation_id = ? AND sequence > ? "
                                    "ORDER BY sequence ASC LIMIT ?");
        query.bind(1, conversation);
        query.bind(2, after_sequence);
        query.bind(3, count);
        std::vector<Message> result;
        while (query.executeStep()) {
            result.push_back(read_message(query));
        }
        return result;
    });
}

void Database::backup_to(const std::filesystem::path &target) {
    if (impl_->depth != 0) {
        throw StorageError(ErrorKind::Invalid, "Can't back up inside a transaction");
    }
    if (std::filesystem::exists(target)) {
        throw StorageError(ErrorKind::Conflict, "Backup target already exists");
    }
    guarded([&] {
        SQLite::Statement backup(impl_->db, "VACUUM INTO ?");
        backup.bind(1, utf8(target));
        backup.exec();
    });
}

} // namespace pulse::storage
