#include <pulse/auth.hpp>

#include "sodium_init.hpp"

#include <sodium.h>

#include <algorithm>

namespace pulse::auth {
namespace {

constexpr std::size_t token_bytes = 32;

bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

} // namespace

std::optional<std::string> username_problem(std::string_view username) {
    if (username.size() < min_username || username.size() > max_username) {
        return "Username must be " + std::to_string(min_username) + " to " +
               std::to_string(max_username) + " characters";
    }
    // ASCII only for now: it keeps "case-insensitive" honest and dodges lookalike letters
    const bool allowed = std::all_of(username.begin(), username.end(), [](char c) {
        return is_ascii_alnum(c) || c == '_' || c == '.' || c == '-';
    });
    if (!allowed || !is_ascii_alnum(username.front())) {
        return "Username may use letters, digits, '_', '.', '-' and must start with a letter "
               "or digit";
    }
    return std::nullopt;
}

std::optional<std::string> password_problem(std::string_view password) {
    if (password.size() < min_password || password.size() > max_password) {
        return "Password must be " + std::to_string(min_password) + " to " +
               std::to_string(max_password) + " bytes";
    }
    return std::nullopt;
}

std::string username_key(std::string_view username) {
    std::string key(username);
    std::transform(key.begin(), key.end(), key.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return key;
}

Token new_token() {
    detail::init_sodium();
    std::vector<std::uint8_t> raw(token_bytes);
    randombytes_buf(raw.data(), raw.size());

    Token token;
    token.text.resize(raw.size() * 2 + 1);
    sodium_bin2hex(token.text.data(), token.text.size(), raw.data(), raw.size());
    token.text.pop_back(); // bin2hex writes a trailing NUL
    token.hash = *token_hash(token.text);
    sodium_memzero(raw.data(), raw.size());
    return token;
}

std::optional<std::vector<std::uint8_t>> token_hash(std::string_view text) {
    detail::init_sodium();
    if (text.size() != token_bytes * 2) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> raw(token_bytes);
    std::size_t decoded = 0;
    if (sodium_hex2bin(raw.data(), raw.size(), text.data(), text.size(), nullptr, &decoded,
                       nullptr) != 0 ||
        decoded != token_bytes) {
        return std::nullopt;
    }
    // Tokens are already 256 random bits, so a plain fast hash is enough here
    std::vector<std::uint8_t> hash(crypto_generichash_BYTES);
    crypto_generichash(hash.data(), hash.size(), raw.data(), raw.size(), nullptr, 0);
    sodium_memzero(raw.data(), raw.size());
    return hash;
}

std::optional<Cost> parse_cost(std::string_view text) {
    if (text == "minimal") {
        return Cost::Minimal;
    }
    if (text == "interactive") {
        return Cost::Interactive;
    }
    if (text == "moderate") {
        return Cost::Moderate;
    }
    return std::nullopt;
}

LoginThrottle::LoginThrottle(std::size_t max_failures, Clock::duration window, std::size_t max_keys)
    : max_failures_(max_failures), window_(window), max_keys_(max_keys) {}

bool LoginThrottle::blocked(const std::string &key, Clock::time_point now) {
    const auto it = failures_.find(key);
    if (it == failures_.end()) {
        return false;
    }
    auto &times = it->second;
    while (!times.empty() && now - times.front() >= window_) {
        times.pop_front();
    }
    if (times.empty()) {
        failures_.erase(it);
        return false;
    }
    return times.size() >= max_failures_;
}

void LoginThrottle::failed(const std::string &key, Clock::time_point now) {
    if (failures_.size() >= max_keys_ && !failures_.contains(key)) {
        forget_stale(now);
    }
    if (failures_.size() >= max_keys_ && !failures_.contains(key)) {
        // Still full of live entries: drop whoever has been quiet the longest
        const auto quietest =
            std::min_element(failures_.begin(), failures_.end(), [](const auto &a, const auto &b) {
                return a.second.back() < b.second.back();
            });
        failures_.erase(quietest);
    }
    auto &times = failures_[key];
    times.push_back(now);
    // anything past the limit is just memory; blocked() only cares that we hit it
    while (times.size() > max_failures_) {
        times.pop_front();
    }
}

void LoginThrottle::succeeded(const std::string &key) { failures_.erase(key); }

void LoginThrottle::forget_stale(Clock::time_point now) {
    std::erase_if(failures_,
                  [&](const auto &entry) { return now - entry.second.back() >= window_; });
}

} // namespace pulse::auth
