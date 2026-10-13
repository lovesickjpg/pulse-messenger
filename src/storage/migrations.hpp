#pragma once

#include <span>
#include <string_view>

namespace pulse::storage {

struct Migration {
    int version;
    std::string_view name;
    std::string_view sql;
};

// Generated at configure time from migrations/NNN_name.sql, sorted by version
std::span<const Migration> migrations();

} // namespace pulse::storage
