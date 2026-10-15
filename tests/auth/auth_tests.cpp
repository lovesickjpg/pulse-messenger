#include <pulse/auth.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <thread>

using namespace pulse::auth;
using namespace std::chrono_literals;
using pulse::Outcome;

TEST_CASE("Usernames follow the documented rules") {
    for (const auto *good :
         {"bob", "alice_99", "a.b-c", "Z12", "abcdefghijklmnopqrstuvwxyz012345"}) {
        CAPTURE(good);
        REQUIRE_FALSE(username_problem(good));
    }
    for (const auto *bad : {"", "ab", "abcdefghijklmnopqrstuvwxyz0123456", "_bob", ".bob", "bo b",
                            "bob!", "ПРИВЕТ", "bob\n"}) {
        CAPTURE(bad);
        REQUIRE(username_problem(bad));
    }
    REQUIRE(username_key("Alice.B") == "alice.b");
}

TEST_CASE("Passwords are bounded by bytes") {
    REQUIRE(password_problem("short"));
    REQUIRE_FALSE(password_problem("12345678"));
    REQUIRE_FALSE(password_problem(std::string(max_password, 'x')));
    REQUIRE(password_problem(std::string(max_password + 1, 'x')));
}

TEST_CASE("Tokens are random and only their hash is stable") {
    std::set<std::string> seen;
    for (int index = 0; index < 50; ++index) {
        const auto token = new_token();
        REQUIRE(token.text.size() == 64);
        REQUIRE(token.hash.size() == 32);
        REQUIRE(token_hash(token.text) == token.hash);
        REQUIRE(seen.insert(token.text).second);
    }
}

TEST_CASE("Junk tokens never produce a hash") {
    const auto good = new_token().text;
    REQUIRE_FALSE(token_hash(""));
    REQUIRE_FALSE(token_hash(good.substr(1)));
    REQUIRE_FALSE(token_hash(good + "00"));
    REQUIRE_FALSE(token_hash(std::string(64, 'z')));
}

TEST_CASE("Password costs parse by name") {
    REQUIRE(parse_cost("minimal") == Cost::Minimal);
    REQUIRE(parse_cost("interactive") == Cost::Interactive);
    REQUIRE(parse_cost("moderate") == Cost::Moderate);
    REQUIRE_FALSE(parse_cost("fast"));
}

TEST_CASE("The login throttle blocks after repeated failures and forgets them later") {
    LoginThrottle throttle(3, 60s);
    const auto start = LoginThrottle::Clock::time_point{} + 1h;

    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE_FALSE(throttle.blocked("alice", start));
        throttle.failed("alice", start + std::chrono::seconds(attempt));
    }
    REQUIRE(throttle.blocked("alice", start + 10s));
    // other users aren't affected
    REQUIRE_FALSE(throttle.blocked("bob", start + 10s));
    // the window slides: once the first failure ages out, one more try is allowed
    REQUIRE_FALSE(throttle.blocked("alice", start + 60s));
    throttle.failed("alice", start + 60s);
    REQUIRE(throttle.blocked("alice", start + 60s));
    // and a good login wipes the slate
    throttle.succeeded("alice");
    REQUIRE_FALSE(throttle.blocked("alice", start + 60s));
}

TEST_CASE("The login throttle keeps a bounded number of keys") {
    LoginThrottle throttle(2, 60s, 3);
    const auto start = LoginThrottle::Clock::time_point{} + 1h;
    int second = 0;
    for (const auto *user : {"a", "b", "c", "d", "e"}) {
        const auto now = start + std::chrono::seconds(++second);
        throttle.failed(user, now);
        throttle.failed(user, now);
    }
    const auto later = start + 10s;
    // the cap is three, so the two quietest keys had to go
    REQUIRE_FALSE(throttle.blocked("a", later));
    REQUIRE_FALSE(throttle.blocked("b", later));
    for (const auto *user : {"c", "d", "e"}) {
        REQUIRE(throttle.blocked(user, later));
    }
}

namespace {

template <class T> T wait_for(boost::asio::io_context &context, std::optional<Outcome<T>> &slot) {
    context.run();
    context.restart();
    REQUIRE(slot);
    return slot->get();
}

} // namespace

TEST_CASE("The hasher hashes off-thread and verifies on the executor") {
    boost::asio::io_context context;
    PasswordHasher hasher(context.get_executor(), Cost::Minimal);

    std::optional<Outcome<std::string>> hashed;
    std::thread::id handler_thread;
    hasher.hash("correct horse", [&](Outcome<std::string> outcome) {
        handler_thread = std::this_thread::get_id();
        hashed = std::move(outcome);
    });
    const auto hash = wait_for(context, hashed);
    REQUIRE(hash.starts_with("$argon2id$"));
    REQUIRE(hash.find("correct horse") == std::string::npos);
    REQUIRE(handler_thread == std::this_thread::get_id());

    std::optional<Outcome<bool>> right;
    std::optional<Outcome<bool>> wrong;
    std::optional<Outcome<bool>> nobody;
    hasher.verify(hash, "correct horse", [&](Outcome<bool> outcome) { right = outcome; });
    hasher.verify(hash, "Correct horse", [&](Outcome<bool> outcome) { wrong = outcome; });
    // no stored hash at all still runs a full check and says no
    hasher.verify(std::nullopt, "correct horse", [&](Outcome<bool> outcome) { nobody = outcome; });
    context.run();
    REQUIRE(right->get());
    REQUIRE_FALSE(wrong->get());
    REQUIRE_FALSE(nobody->get());
}

TEST_CASE("Two hashes of the same password differ") {
    boost::asio::io_context context;
    PasswordHasher hasher(context.get_executor(), Cost::Minimal);
    std::optional<Outcome<std::string>> first;
    std::optional<Outcome<std::string>> second;
    hasher.hash("same password", [&](Outcome<std::string> outcome) { first = outcome; });
    hasher.hash("same password", [&](Outcome<std::string> outcome) { second = outcome; });
    context.run();
    REQUIRE(first->get() != second->get());
}

TEST_CASE("The hasher refuses work past its backlog and after stop") {
    boost::asio::io_context context;
    // one thread and one queue slot; real-cost hashes are slow enough that a burst
    // of forty can't drain through it
    PasswordHasher hasher(context.get_executor(), Cost::Interactive, 1, 1);

    std::vector<std::optional<HasherError::Kind>> failures(40);
    for (std::size_t index = 0; index < failures.size(); ++index) {
        hasher.hash("password" + std::to_string(index),
                    [&failures, index](Outcome<std::string> outcome) {
                        try {
                            outcome.get();
                        } catch (const HasherError &error) {
                            failures[index] = error.kind();
                        }
                    });
    }
    context.run();
    REQUIRE(std::any_of(failures.begin(), failures.end(), [](const auto &failure) {
        return failure == HasherError::Kind::Overloaded;
    }));
    REQUIRE_FALSE(failures.front());

    context.restart();
    hasher.stop();
    std::optional<HasherError::Kind> after_stop;
    hasher.verify(std::nullopt, "password", [&](Outcome<bool> outcome) {
        try {
            outcome.get();
        } catch (const HasherError &error) {
            after_stop = error.kind();
        }
    });
    context.run();
    REQUIRE(after_stop == HasherError::Kind::Shutdown);
}
