#pragma once

#include <sodium.h>

#include <stdexcept>

namespace pulse::auth::detail {

// sodium_init() is thread-safe and idempotent, but it can fail, and then nothing is safe
inline void init_sodium() {
    static const bool ready = sodium_init() >= 0;
    if (!ready) {
        throw std::runtime_error("libsodium failed to initialize");
    }
}

} // namespace pulse::auth::detail
