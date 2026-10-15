#pragma once

#include <pulse/outcome.hpp>

#include <boost/asio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::auth {

inline constexpr std::size_t min_username = 3;
inline constexpr std::size_t max_username = 32;
inline constexpr std::size_t min_password = 8;
inline constexpr std::size_t max_password = 256;

// Each returns nothing when the value is fine, or a message we can show the user
std::optional<std::string> username_problem(std::string_view username);
std::optional<std::string> password_problem(std::string_view password);

// Usernames are unique ignoring ASCII case, so throttling has to be too
std::string username_key(std::string_view username);

// 32 random bytes. The client gets `text` (hex); the database only ever sees `hash`.
struct Token {
    std::string text;
    std::vector<std::uint8_t> hash;
};

Token new_token();
// Nothing if `text` can't be a token we issued, so junk never reaches the database
std::optional<std::vector<std::uint8_t>> token_hash(std::string_view text);

// How hard Argon2id works per hash. Minimal exists for tests and nothing else.
enum class Cost { Minimal, Interactive, Moderate };
std::optional<Cost> parse_cost(std::string_view text);

// Counts failed logins per key in a sliding window. Not thread-safe.
class LoginThrottle {
  public:
    using Clock = std::chrono::steady_clock;

    explicit LoginThrottle(std::size_t max_failures = 5,
                           Clock::duration window = std::chrono::minutes(1),
                           std::size_t max_keys = 10'000);

    [[nodiscard]] bool blocked(const std::string &key, Clock::time_point now);
    void failed(const std::string &key, Clock::time_point now);
    void succeeded(const std::string &key);

  private:
    void forget_stale(Clock::time_point now);

    std::size_t max_failures_;
    Clock::duration window_;
    std::size_t max_keys_;
    std::unordered_map<std::string, std::deque<Clock::time_point>> failures_;
};

class HasherError : public std::runtime_error {
  public:
    enum class Kind { Overloaded, Shutdown };
    HasherError(Kind kind, const std::string &message) : std::runtime_error(message), kind_(kind) {}
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

  private:
    Kind kind_;
};

// Argon2id on a few background threads. Hashing is slow on purpose, so it must
// never run on the network thread, and the backlog is capped so a login flood
// can't eat all the memory. Results land on the `results` executor.
//
// Destroy it before the io_context, same as DatabaseWorker.
class PasswordHasher {
  public:
    PasswordHasher(boost::asio::any_io_executor results, Cost cost, std::size_t threads = 2,
                   std::size_t max_pending = 32);
    ~PasswordHasher();
    PasswordHasher(const PasswordHasher &) = delete;
    PasswordHasher &operator=(const PasswordHasher &) = delete;

    void hash(std::string password, std::function<void(Outcome<std::string>)> done);
    // With no stored hash it checks a throwaway one and says no, so a missing
    // user takes as long as a wrong password
    void verify(std::optional<std::string> stored, std::string password,
                std::function<void(Outcome<bool>)> done);
    // Finishes running hashes, fails queued ones with Shutdown, joins the threads
    void stop();

  private:
    using Job = std::function<void(std::exception_ptr rejected)>;

    template <class T, class Work> void submit(Work work, std::function<void(Outcome<T>)> done);
    void enqueue(Job job);
    void run();

    boost::asio::any_io_executor results_;
    unsigned long long opslimit_ = 0;
    std::size_t memlimit_ = 0;
    std::string dummy_hash_;
    std::size_t max_pending_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

} // namespace pulse::auth
