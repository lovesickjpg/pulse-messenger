#include <pulse/auth.hpp>

#include "sodium_init.hpp"

#include <sodium.h>

#include <memory>

namespace pulse::auth {
namespace {

std::string hash_now(std::string_view password, unsigned long long opslimit, std::size_t memlimit) {
    std::string out(crypto_pwhash_STRBYTES, '\0');
    if (crypto_pwhash_str(out.data(), password.data(), password.size(), opslimit, memlimit) != 0) {
        throw std::runtime_error("Out of memory while hashing a password");
    }
    out.resize(out.find('\0'));
    return out;
}

} // namespace

PasswordHasher::PasswordHasher(boost::asio::any_io_executor results, Cost cost, std::size_t threads,
                               std::size_t max_pending)
    : results_(std::move(results)), max_pending_(max_pending) {
    detail::init_sodium();
    switch (cost) {
    case Cost::Minimal:
        opslimit_ = crypto_pwhash_OPSLIMIT_MIN;
        memlimit_ = crypto_pwhash_MEMLIMIT_MIN;
        break;
    case Cost::Interactive:
        opslimit_ = crypto_pwhash_OPSLIMIT_INTERACTIVE;
        memlimit_ = crypto_pwhash_MEMLIMIT_INTERACTIVE;
        break;
    case Cost::Moderate:
        opslimit_ = crypto_pwhash_OPSLIMIT_MODERATE;
        memlimit_ = crypto_pwhash_MEMLIMIT_MODERATE;
        break;
    }
    // Same cost as real hashes, otherwise the timing gives unknown users away
    dummy_hash_ = hash_now("not anybody's password", opslimit_, memlimit_);
    for (std::size_t index = 0; index < std::max<std::size_t>(threads, 1); ++index) {
        threads_.emplace_back([this] { run(); });
    }
}

PasswordHasher::~PasswordHasher() { stop(); }

template <class T, class Work>
void PasswordHasher::submit(Work work, std::function<void(Outcome<T>)> done) {
    using Guard = boost::asio::executor_work_guard<boost::asio::any_io_executor>;
    auto keep_alive = std::make_shared<Guard>(boost::asio::make_work_guard(results_));
    auto shared_work = std::make_shared<Work>(std::move(work));
    enqueue([executor = results_, keep_alive, shared_work,
             done = std::move(done)](std::exception_ptr rejected) {
        Outcome<T> outcome(rejected);
        if (!rejected) {
            try {
                outcome = Outcome<T>((*shared_work)());
            } catch (...) {
                outcome = Outcome<T>(std::current_exception());
            }
        }
        boost::asio::post(executor, [outcome = std::move(outcome), done, keep_alive]() mutable {
            done(std::move(outcome));
        });
    });
}

void PasswordHasher::hash(std::string password, std::function<void(Outcome<std::string>)> done) {
    submit<std::string>(
        [this, password = std::move(password)]() mutable {
            auto result = hash_now(password, opslimit_, memlimit_);
            sodium_memzero(password.data(), password.size());
            return result;
        },
        std::move(done));
}

void PasswordHasher::verify(std::optional<std::string> stored, std::string password,
                            std::function<void(Outcome<bool>)> done) {
    submit<bool>(
        [this, stored = std::move(stored), password = std::move(password)]() mutable {
            const auto &target = stored ? *stored : dummy_hash_;
            const bool match =
                crypto_pwhash_str_verify(target.c_str(), password.data(), password.size()) == 0;
            sodium_memzero(password.data(), password.size());
            return stored && match;
        },
        std::move(done));
}

void PasswordHasher::enqueue(Job job) {
    std::exception_ptr rejected;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            rejected = std::make_exception_ptr(
                HasherError(HasherError::Kind::Shutdown, "Password hasher is stopping"));
        } else if (queue_.size() >= max_pending_) {
            rejected = std::make_exception_ptr(
                HasherError(HasherError::Kind::Overloaded, "Too many logins in flight"));
        } else {
            queue_.push_back(std::move(job));
            wake_.notify_one();
            return;
        }
    }
    job(rejected);
}

void PasswordHasher::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job(nullptr);
    }
}

void PasswordHasher::stop() {
    std::deque<Job> abandoned;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        abandoned.swap(queue_);
        wake_.notify_all();
    }
    for (auto &thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    const auto reason = std::make_exception_ptr(
        HasherError(HasherError::Kind::Shutdown, "Password hasher stopped before this job ran"));
    for (auto &job : abandoned) {
        job(reason);
    }
}

} // namespace pulse::auth
