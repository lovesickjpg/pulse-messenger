#pragma once

#include <exception>
#include <optional>
#include <utility>

namespace pulse {

struct Unit {};

// What a finished background job hands back: a value, or the exception it died with
template <class T> class Outcome {
  public:
    Outcome() = default;
    explicit Outcome(T value) : value_(std::move(value)) {}
    explicit Outcome(std::exception_ptr error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !error_; }
    [[nodiscard]] std::exception_ptr error() const noexcept { return error_; }
    // Rethrows the job's exception, so callers can just try/catch
    T &get() {
        if (error_) {
            std::rethrow_exception(error_);
        }
        return *value_;
    }

  private:
    std::optional<T> value_;
    std::exception_ptr error_;
};

} // namespace pulse
