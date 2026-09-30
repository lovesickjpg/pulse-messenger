#include <pulse/protocol.hpp>

#include <algorithm>
#include <utility>

namespace pulse::protocol {
namespace {

[[noreturn]] void invalid(std::string message) {
    throw ProtocolError("invalid_request", std::move(message));
}

void require_string(const nlohmann::json &object, std::string_view field) {
    const auto it = object.find(std::string(field));
    if (it == object.end() || !it->is_string() || it->get_ref<const std::string &>().empty()) {
        invalid(std::string(field) + " must be a nonempty string");
    }
}

void validate(const Message &message) {
    if (message.version != current_version) {
        throw ProtocolError("unsupported_version", "Only protocol version 1 is supported");
    }
    if (!message.payload.is_object()) {
        invalid("payload must be an object");
    }
    if (message.type == "message.received") {
        if (message.request_id) {
            invalid("Events require a null request_id");
        }
    } else if (!message.request_id || message.request_id->empty()) {
        invalid("Requests and responses require a nonempty request_id");
    }

    if (message.type == "message.send" || message.type == "message.send.result" ||
        message.type == "message.received") {
        require_string(message.payload, "conversation_id");
        require_string(message.payload, "client_message_id");
        if (message.type != "message.send") {
            require_string(message.payload, "message_id");
        }
        if (message.type != "message.send.result") {
            require_string(message.payload, "text");
        }
    } else if (message.type == "error") {
        require_string(message.payload, "code");
        require_string(message.payload, "message");
        constexpr std::array codes{"invalid_request", "unsupported_version", "unauthorized",
                                   "forbidden",       "not_found",           "conflict",
                                   "rate_limited",    "internal_error"};
        const auto &code = message.payload.at("code").get_ref<const std::string &>();
        if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
            invalid("Unknown error code");
        }
    } else {
        invalid("Unknown message type");
    }
}

} // namespace

ProtocolError::ProtocolError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

const std::string &ProtocolError::code() const noexcept { return code_; }

std::string serialize(const Message &message) {
    validate(message);
    nlohmann::json object{{"version", message.version},
                          {"type", message.type},
                          {"request_id", message.request_id ? nlohmann::json(*message.request_id)
                                                            : nlohmann::json(nullptr)},
                          {"payload", message.payload}};
    try {
        auto result = object.dump();
        if (result.size() > max_payload_size) {
            invalid("JSON payload exceeds 64 KiB");
        }
        return result;
    } catch (const nlohmann::json::exception &) {
        invalid("JSON must contain valid UTF-8 strings");
    }
}

Message parse(std::string_view json) {
    if (json.empty() || json.size() > max_payload_size) {
        invalid("JSON payload length must be between 1 and 65536 bytes");
    }
    if (json.starts_with("\xEF\xBB\xBF")) {
        invalid("UTF-8 BOM is not allowed");
    }
    try {
        const auto object = nlohmann::json::parse(json.begin(), json.end());
        if (!object.is_object() || !object.contains("version") ||
            !object.at("version").is_number_integer()) {
            invalid("version must be an integer in a JSON object");
        }
        if (object.at("version") != current_version) {
            throw ProtocolError("unsupported_version", "Only protocol version 1 is supported");
        }
        require_string(object, "type");
        if (!object.contains("request_id") || !object.contains("payload")) {
            invalid("Missing request_id or payload");
        }
        Message result;
        result.type = object.at("type").get<std::string>();
        if (!object.at("request_id").is_null()) {
            require_string(object, "request_id");
            result.request_id = object.at("request_id").get<std::string>();
        }
        result.payload = object.at("payload");
        validate(result);
        return result;
    } catch (const nlohmann::json::exception &) {
        invalid("Malformed JSON or invalid UTF-8");
    }
}

std::vector<std::uint8_t> encode_frame(const Message &message) {
    const auto payload = serialize(message);
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame;
    frame.reserve(4 + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<std::uint8_t>(size >> shift));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::vector<Message> FrameDecoder::feed(std::span<const std::uint8_t> bytes) {
    if (failed_) {
        invalid("Decoder failed; reset before reuse");
    }
    std::vector<Message> messages;
    try {
        while (!bytes.empty()) {
            if (header_size_ < header_.size()) {
                const auto count = std::min(header_.size() - header_size_, bytes.size());
                std::copy_n(bytes.begin(), count, header_.begin() + header_size_);
                header_size_ += count;
                bytes = bytes.subspan(count);
                if (header_size_ < header_.size()) {
                    continue;
                }
                std::uint32_t length = 0;
                for (const auto byte : header_) {
                    length = (length << 8) | byte;
                }
                // Check the complete header before allocating or consuming payload bytes.
                if (length == 0 || length > max_payload_size) {
                    invalid("Frame payload length must be between 1 and 65536 bytes");
                }
                expected_size_ = length;
                payload_.reserve(expected_size_);
            }
            const auto count = std::min(expected_size_ - payload_.size(), bytes.size());
            if (count != 0) {
                payload_.append(reinterpret_cast<const char *>(bytes.data()), count);
                bytes = bytes.subspan(count);
            }
            if (payload_.size() == expected_size_) {
                messages.push_back(parse(payload_));
                header_size_ = 0;
                expected_size_ = 0;
                payload_.clear();
            }
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
    return messages;
}

void FrameDecoder::finish() {
    if (failed_) {
        invalid("Decoder failed; reset before reuse");
    }
    if (header_size_ != 0) {
        failed_ = true;
        invalid(header_size_ < header_.size() ? "EOF in frame header" : "EOF in frame payload");
    }
}

void FrameDecoder::reset() noexcept {
    header_size_ = 0;
    expected_size_ = 0;
    payload_.clear();
    failed_ = false;
}

} // namespace pulse::protocol
