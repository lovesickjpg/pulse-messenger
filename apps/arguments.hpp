#pragma once

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pulse {

inline unsigned parse_number(std::string_view text, unsigned maximum) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > maximum) {
        throw std::invalid_argument("Invalid numeric argument: " + std::string(text));
    }
    return value;
}

} // namespace pulse
