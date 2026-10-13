#include <pulse/database_worker.hpp>
#include <pulse/storage.hpp>

#include <SQLiteCpp/SQLiteCpp.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace pulse::storage;
using namespace std::chrono_literals;

namespace {

// Every test gets its own directory so databases never leak between cases
class TempDir {
  public:
    TempDir() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("pulse-storage-test-" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::filesystem::path file(const std::string &name = "pulse.db") const {
        return path_ / name;
    }

  private:
    std::filesystem::path path_;
};

const Timestamp t0 = Timestamp(std::chrono::milliseconds(1'700'000'000'000));

Timestamp at(int seconds) { return t0 + std::chrono::seconds(seconds); }

std::vector<std::uint8_t> token(std::uint8_t seed) { return std::vector<std::uint8_t>(32, seed); }

void expect_kind(const std::function<void()> &action, ErrorKind kind) {
    try {
        action();
    } catch (const StorageError &error) {
        REQUIRE(error.kind() == kind);
        return;
    }
    FAIL("Expected a StorageError");
}

std::int64_t count(SQLite::Database &db, const std::string &sql) {
    return db.execAndGet(sql).getInt64();
}

std::vector<std::int64_t> sequences(const std::vector<Message> &messages) {
    std::vector<std::int64_t> result;
    for (const auto &message : messages) {
        result.push_back(message.sequence);
    }
    return result;
}

} // namespace

TEST_CASE("A fresh database is created from the migrations") {
    TempDir dir;
    {
        Database db(dir.file());
        REQUIRE(db.schema_version() == 1);
    }
    SQLite::Database raw(dir.file().string());
    for (const char *table : {"users", "sessions", "conversations", "conversation_members",
                              "messages", "schema_migrations"}) {
        INFO(table);
        REQUIRE(count(raw, std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                                       "AND name = '") +
                               table + "'") == 1);
    }
    REQUIRE(raw.execAndGet("SELECT name FROM schema_migrations").getString() == "001_initial");
}

TEST_CASE("Opening creates missing parent directories") {
    TempDir dir;
    const auto nested = dir.file("a") / "b" / "pulse.db";
    Database db(nested);
    REQUIRE(std::filesystem::exists(nested));
}

TEST_CASE("Reopening does not apply a migration twice") {
    TempDir dir;
    std::int64_t first_applied = 0;
    {
        Database db(dir.file());
    }
    {
        SQLite::Database raw(dir.file().string());
        first_applied = raw.execAndGet("SELECT applied_at FROM schema_migrations").getInt64();
    }
    {
        Database db(dir.file());
        REQUIRE(db.schema_version() == 1);
    }
    SQLite::Database raw(dir.file().string());
    REQUIRE(count(raw, "SELECT COUNT(*) FROM schema_migrations") == 1);
    REQUIRE(raw.execAndGet("SELECT applied_at FROM schema_migrations").getInt64() == first_applied);
}

TEST_CASE("A database from a newer build is refused") {
    TempDir dir;
    {
        Database db(dir.file());
    }
    {
        SQLite::Database raw(dir.file().string(), SQLite::OPEN_READWRITE);
        raw.exec("INSERT INTO schema_migrations (version, name, applied_at) "
                 "VALUES (99, '099_from_the_future', 0)");
    }
    expect_kind([&] { Database db(dir.file()); }, ErrorKind::Migration);
}

TEST_CASE("Connections enforce foreign keys and use WAL") {
    TempDir dir;
    Database db(dir.file());
    // the pragmas are per connection, so we check through the API instead of a raw handle
    expect_kind([&] { db.add_member(999, 999, Role::Member, t0); }, ErrorKind::NotFound);
    SQLite::Database raw(dir.file().string());
    REQUIRE(raw.execAndGet("PRAGMA journal_mode").getString() == "wal");
}

TEST_CASE("Data survives closing and reopening the database") {
    TempDir dir;
    Id alice = 0;
    Id room = 0;
    {
        Database db(dir.file());
        alice = db.create_user("alice", "hash-a", t0).id;
        const auto bob = db.create_user("bob", "hash-b", t0).id;
        room = db.create_room("general", alice, t0).id;
        db.add_member(room, bob, Role::Member, t0);
        db.append_message(room, alice, "m-1", "Привет!", at(1));
        db.append_message(room, bob, "m-2", "hey", at(2));
    }
    Database db(dir.file());
    const auto user = db.find_user("ALICE");
    REQUIRE(user);
    REQUIRE(user->id == alice);
    REQUIRE(user->password_hash == "hash-a");
    REQUIRE(user->created_at == t0);

    const auto found = db.find_room("General");
    REQUIRE(found);
    REQUIRE(found->id == room);
    REQUIRE(found->last_sequence == 2);
    REQUIRE(db.members(room).size() == 2);

    const auto history = db.history_before(room, std::nullopt, 10);
    REQUIRE(history.size() == 2);
    REQUIRE(history[0].body == "Привет!");
    REQUIRE(history[1].client_message_id == "m-2");
}

TEST_CASE("A failed transaction leaves nothing behind") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;

    SECTION("an exception rolls back every statement") {
        REQUIRE_THROWS_AS(db.in_transaction([&] {
            db.create_user("bob", "hash", t0);
            db.create_room("lost", alice, t0);
            throw std::runtime_error("boom");
        }),
                          std::runtime_error);
        REQUIRE_FALSE(db.find_user("bob"));
        REQUIRE_FALSE(db.find_room("lost"));
    }

    SECTION("a constraint error halfway through undoes the earlier writes") {
        expect_kind(
            [&] {
                db.in_transaction([&] {
                    db.create_user("carol", "hash", t0);
                    db.create_user("alice", "hash", t0);
                });
            },
            ErrorKind::Conflict);
        REQUIRE_FALSE(db.find_user("carol"));
    }

    SECTION("swallowing a nested failure can't sneak a partial commit through") {
        expect_kind(
            [&] {
                db.in_transaction([&] {
                    db.create_user("dave", "hash", t0);
                    try {
                        db.in_transaction([&] { throw std::runtime_error("inner"); });
                    } catch (const std::runtime_error &) {
                    }
                });
            },
            ErrorKind::Internal);
        REQUIRE_FALSE(db.find_user("dave"));
    }

    SECTION("the connection is usable afterwards") {
        REQUIRE_THROWS(db.in_transaction([] { throw std::runtime_error("boom"); }));
        REQUIRE(db.create_user("erin", "hash", t0).id > 0);
    }

    SECTION("committed work stays when nothing fails") {
        const auto id = db.in_transaction([&] {
            return db.in_transaction([&] { return db.create_user("frank", "hash", t0).id; });
        });
        REQUIRE(db.find_user(id));
    }
}

TEST_CASE("Unique constraints fire") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto bob = db.create_user("bob", "hash", t0).id;

    SECTION("usernames, ignoring ASCII case") {
        expect_kind([&] { db.create_user("Alice", "other", t0); }, ErrorKind::Conflict);
    }
    SECTION("room names") {
        db.create_room("general", alice, t0);
        expect_kind([&] { db.create_room("GENERAL", bob, t0); }, ErrorKind::Conflict);
        // and the failed attempt must not leave a half-built room behind
        REQUIRE(db.find_room("general")->id > 0);
    }
    SECTION("membership") {
        const auto room = db.create_room("general", alice, t0).id;
        expect_kind([&] { db.add_member(room, alice, Role::Member, t0); }, ErrorKind::Conflict);
    }
    SECTION("session token hashes") {
        db.create_session(alice, token(1), t0, at(60));
        expect_kind([&] { db.create_session(bob, token(1), t0, at(60)); }, ErrorKind::Conflict);
    }
    SECTION("a client message id per conversation and sender") {
        const auto room = db.create_room("general", alice, t0).id;
        db.add_member(room, bob, Role::Member, t0);
        db.append_message(room, alice, "same", "first", t0);
        expect_kind([&] { db.append_message(room, alice, "same", "again", t0); },
                    ErrorKind::Conflict);
        // another sender may reuse the id
        REQUIRE(db.append_message(room, bob, "same", "mine", t0).sequence == 2);
        // the rejected attempt must not have burned a sequence number
        REQUIRE(db.append_message(room, alice, "next", "second", t0).sequence == 3);
    }
}

TEST_CASE("Foreign keys and CHECK constraints are enforced") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto room = db.create_room("general", alice, t0).id;

    expect_kind([&] { db.add_member(room, 12345, Role::Member, t0); }, ErrorKind::NotFound);
    expect_kind([&] { db.add_member(12345, alice, Role::Member, t0); }, ErrorKind::NotFound);
    expect_kind([&] { db.append_message(12345, alice, "m", "text", t0); }, ErrorKind::NotFound);
    expect_kind([&] { db.append_message(room, 12345, "m", "text", t0); }, ErrorKind::NotFound);
    expect_kind([&] { db.create_session(12345, token(1), t0, at(1)); }, ErrorKind::NotFound);
    expect_kind([&] { db.open_direct(alice, 12345, t0); }, ErrorKind::NotFound);
    expect_kind([&] { db.create_user("", "hash", t0); }, ErrorKind::Invalid);
    expect_kind([&] { db.create_room("", alice, t0); }, ErrorKind::Invalid);
    expect_kind([&] { db.append_message(room, alice, "m", "", t0); }, ErrorKind::Invalid);

    // none of the failures above may have changed the conversation
    REQUIRE(db.find_conversation(room)->last_sequence == 0);
    REQUIRE(db.members(room).size() == 1);
}

TEST_CASE("Sequences are assigned per conversation without gaps") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto first = db.create_room("first", alice, t0).id;
    const auto second = db.create_room("second", alice, t0).id;

    for (int index = 1; index <= 5; ++index) {
        const auto message =
            db.append_message(first, alice, "a-" + std::to_string(index), "text", at(index));
        REQUIRE(message.sequence == index);
        REQUIRE(message.created_at == at(index));
    }
    REQUIRE(db.append_message(second, alice, "b-1", "text", t0).sequence == 1);
    REQUIRE(db.find_conversation(first)->last_sequence == 5);

    const auto existing = db.find_message(first, alice, "a-3");
    REQUIRE(existing);
    REQUIRE(existing->sequence == 3);
    REQUIRE_FALSE(db.find_message(first, alice, "nope"));
}

TEST_CASE("History pages walk back and forward without overlap") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto room = db.create_room("general", alice, t0).id;
    for (int index = 1; index <= 7; ++index) {
        db.append_message(room, alice, "m-" + std::to_string(index), "text", at(index));
    }

    const auto newest = db.history_before(room, std::nullopt, 3);
    REQUIRE(sequences(newest) == std::vector<std::int64_t>{5, 6, 7});
    const auto older = db.history_before(room, newest.front().sequence, 3);
    REQUIRE(sequences(older) == std::vector<std::int64_t>{2, 3, 4});
    const auto oldest = db.history_before(room, older.front().sequence, 3);
    REQUIRE(sequences(oldest) == std::vector<std::int64_t>{1});
    REQUIRE(db.history_before(room, 1, 3).empty());

    REQUIRE(sequences(db.messages_after(room, 4, 10)) == std::vector<std::int64_t>{5, 6, 7});
    REQUIRE(sequences(db.messages_after(room, 0, 2)) == std::vector<std::int64_t>{1, 2});
    REQUIRE(db.messages_after(room, 7, 10).empty());

    expect_kind([&] { (void)db.history_before(room, std::nullopt, 0); }, ErrorKind::Invalid);
    expect_kind([&] { (void)db.messages_after(room, 0, -1); }, ErrorKind::Invalid);
}

TEST_CASE("History page size is capped") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto room = db.create_room("general", alice, t0).id;
    db.in_transaction([&] {
        for (int index = 1; index <= max_page_size + 5; ++index) {
            db.append_message(room, alice, "m-" + std::to_string(index), "text", t0);
        }
    });
    REQUIRE(db.history_before(room, std::nullopt, 100000).size() ==
            static_cast<std::size_t>(max_page_size));
}

TEST_CASE("A pair of users has exactly one direct dialog") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto bob = db.create_user("bob", "hash", t0).id;
    const auto carol = db.create_user("carol", "hash", t0).id;

    const auto dialog = db.open_direct(alice, bob, t0);
    REQUIRE(dialog.kind == ConversationKind::Direct);
    REQUIRE_FALSE(dialog.name);
    REQUIRE(db.open_direct(bob, alice, at(5)).id == dialog.id);
    REQUIRE(db.open_direct(alice, bob, at(9)).id == dialog.id);
    REQUIRE(db.open_direct(alice, carol, t0).id != dialog.id);

    REQUIRE(db.members(dialog.id).size() == 2);
    REQUIRE(db.role_of(dialog.id, alice) == Role::Member);
    REQUIRE_FALSE(db.role_of(dialog.id, carol));
    expect_kind([&] { db.open_direct(alice, alice, t0); }, ErrorKind::Invalid);
}

TEST_CASE("Rooms and membership") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;
    const auto bob = db.create_user("bob", "hash", t0).id;

    const auto room = db.create_room("general", alice, t0);
    REQUIRE(room.kind == ConversationKind::Room);
    REQUIRE(room.name == "general");
    REQUIRE(db.role_of(room.id, alice) == Role::Owner);
    REQUIRE_FALSE(db.role_of(room.id, bob));

    db.add_member(room.id, bob, Role::Member, at(1));
    REQUIRE(db.role_of(room.id, bob) == Role::Member);
    REQUIRE(db.remove_member(room.id, bob));
    REQUIRE_FALSE(db.remove_member(room.id, bob));
    REQUIRE_FALSE(db.role_of(room.id, bob));
}

TEST_CASE("Sessions expire and can be revoked") {
    TempDir dir;
    Database db(dir.file());
    const auto alice = db.create_user("alice", "hash", t0).id;

    const auto session = db.create_session(alice, token(7), t0, at(60));
    REQUIRE(session.user_id == alice);

    const auto active = db.find_active_session(token(7), at(30));
    REQUIRE(active);
    REQUIRE(active->id == session.id);

    REQUIRE_FALSE(db.find_active_session(token(7), at(60)));
    REQUIRE_FALSE(db.find_active_session(token(8), at(30)));

    REQUIRE(db.revoke_session(session.id, at(40)));
    REQUIRE_FALSE(db.revoke_session(session.id, at(41)));
    REQUIRE_FALSE(db.find_active_session(token(7), at(30)));
}

TEST_CASE("Backups are consistent copies and never overwrite") {
    TempDir dir;
    const auto copy = dir.file("backup.db");
    {
        Database db(dir.file());
        const auto alice = db.create_user("alice", "hash", t0).id;
        const auto room = db.create_room("general", alice, t0).id;
        db.append_message(room, alice, "m-1", "saved", t0);
        db.backup_to(copy);
        expect_kind([&] { db.backup_to(copy); }, ErrorKind::Conflict);
        expect_kind([&] { db.in_transaction([&] { db.backup_to(dir.file("x.db")); }); },
                    ErrorKind::Invalid);
    }
    Database restored(copy);
    const auto room = restored.find_room("general");
    REQUIRE(room);
    REQUIRE(restored.history_before(room->id, std::nullopt, 10).at(0).body == "saved");
}

TEST_CASE("The worker runs jobs on its own thread and answers on the executor") {
    TempDir dir;
    boost::asio::io_context context;
    DatabaseWorker worker(context.get_executor(), dir.file());

    std::thread::id job_thread;
    std::thread::id handler_thread;
    std::optional<Outcome<User>> created;
    worker.submit(
        [&](Database &db) {
            job_thread = std::this_thread::get_id();
            return db.create_user("alice", "hash", t0);
        },
        [&](Outcome<User> outcome) {
            handler_thread = std::this_thread::get_id();
            created = std::move(outcome);
        });

    // the pending job must keep run() from returning early
    context.run();
    REQUIRE(created);
    REQUIRE(created->get().username == "alice");
    REQUIRE(job_thread != std::this_thread::get_id());
    REQUIRE(handler_thread == std::this_thread::get_id());
}

TEST_CASE("The worker keeps jobs in order and reports failures") {
    TempDir dir;
    boost::asio::io_context context;
    DatabaseWorker worker(context.get_executor(), dir.file());

    std::vector<int> order;
    std::optional<ErrorKind> failure;
    std::optional<std::string> found;

    worker.submit([](Database &db) { db.create_user("alice", "hash", t0); },
                  [&](Outcome<Unit>) { order.push_back(1); });
    worker.submit([](Database &db) { db.create_user("alice", "again", t0); },
                  [&](Outcome<Unit> outcome) {
                      order.push_back(2);
                      try {
                          outcome.get();
                      } catch (const StorageError &error) {
                          failure = error.kind();
                      }
                  });
    worker.submit([](Database &db) { return db.find_user("alice"); },
                  [&](Outcome<std::optional<User>> outcome) {
                      order.push_back(3);
                      found = outcome.get()->password_hash;
                  });

    context.run();
    REQUIRE(order == std::vector<int>{1, 2, 3});
    REQUIRE(failure == ErrorKind::Conflict);
    REQUIRE(found == "hash");
}

TEST_CASE("The worker refuses work when the queue is full or after stop") {
    TempDir dir;
    boost::asio::io_context context;
    DatabaseWorker worker(context.get_executor(), dir.file(), 1);

    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::vector<std::optional<ErrorKind>> results(4);
    const auto record = [&](std::size_t slot) {
        return [&results, slot](Outcome<Unit> outcome) {
            try {
                outcome.get();
            } catch (const StorageError &error) {
                results[slot] = error.kind();
            }
        };
    };

    // parks the worker thread so the queue can fill up deterministically
    worker.submit(
        [&](Database &) {
            started.set_value();
            released.wait();
        },
        record(0));
    started.get_future().wait();

    worker.submit([](Database &) {}, record(1)); // fills the one queue slot
    worker.submit([](Database &) {}, record(2)); // no room
    release.set_value();
    context.run();

    REQUIRE_FALSE(results[0]);
    REQUIRE_FALSE(results[1]);
    REQUIRE(results[2] == ErrorKind::Overloaded);

    context.restart();
    worker.stop();
    worker.submit([](Database &) {}, record(3));
    context.run();
    REQUIRE(results[3] == ErrorKind::Shutdown);
    worker.stop(); // second stop is a no-op
}

TEST_CASE("The worker fails to start on a bad database") {
    TempDir dir;
    boost::asio::io_context context;

    SECTION("path is a directory") {
        REQUIRE_THROWS_AS(DatabaseWorker(context.get_executor(), dir.file("")), StorageError);
    }
    SECTION("newer schema") {
        {
            Database db(dir.file());
        }
        {
            SQLite::Database raw(dir.file().string(), SQLite::OPEN_READWRITE);
            raw.exec("INSERT INTO schema_migrations VALUES (42, '042_future', 0)");
        }
        REQUIRE_THROWS_AS(DatabaseWorker(context.get_executor(), dir.file()), StorageError);
    }
}
