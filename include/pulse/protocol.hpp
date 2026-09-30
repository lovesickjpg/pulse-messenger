#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace pulse::protocol {

inline constexpr std::uint32_t current_version = 1;
inline constexpr std::size_t max_payload_size = 64 * 1024;

class ProtocolError : public std::runtime_error {
  public:
    ProtocolError(std::string code, std::string message);
    [[nodiscard]] const std::string &code() const noexcept;

  private:
    std::string code_;
};

struct Message {
    std::uint32_t version = current_version;
    std::string type;
    std::optional<std::string> request_id;
    nlohmann::json payload = nlohmann::json::object();
};

// Both directions validate the same wire contract. Unknown fields are ignored.
[[nodiscard]] std::string serialize(const Message &message);
[[nodiscard]] Message parse(std::string_view json);
[[nodiscard]] std::vector<std::uint8_t> encode_frame(const Message &message);

class FrameDecoder {
  public:
    [[nodiscard]] std::vector<Message> feed(std::span<const std::uint8_t> bytes);
    // Call at EOF, including after a peer disconnects.
    void finish();
    // A failed decoder must be reset before reuse on a new connection.
    void reset() noexcept;

  private:
    std::array<std::uint8_t, 4> header_{};
    std::size_t header_size_ = 0;
    std::size_t expected_size_ = 0;
    std::string payload_;
    bool failed_ = false;
};

} // namespace pulse::protocol
