#include <pulse/protocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <limits>

using namespace pulse::protocol;
using nlohmann::json;

namespace {

Message send() {
    return {1,
            "message.send",
            "req-001",
            {{"conversation_id", "42"}, {"client_message_id", "attempt-001"}, {"text", "Привет!"}}};
}

std::vector<Message> examples() {
    return {
        send(),
        {1,
         "message.send.result",
         "req-001",
         {{"conversation_id", "42"}, {"client_message_id", "attempt-001"}, {"message_id", "99"}}},
        {1, "error", "req-001", {{"code", "forbidden"}, {"message", "Membership required"}}},
        {1,
         "message.received",
         std::nullopt,
         {{"conversation_id", "42"},
          {"client_message_id", "attempt-001"},
          {"message_id", "99"},
          {"text", "Привет!"}}}};
}

void expect_error(const std::function<void()> &action, std::string_view code = "invalid_request") {
    try {
        action();
    } catch (const ProtocolError &error) {
        REQUIRE(error.code() == code);
        return;
    }
    FAIL("Expected ProtocolError");
}

std::vector<std::uint8_t> raw_frame(std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

TEST_CASE("Every implemented message type round trips JSON and frames", "[protocol]") {
    for (const auto &message : examples()) {
        CAPTURE(message.type);
        const auto wire = serialize(message);
        const auto decoded = parse(wire);
        REQUIRE(decoded.version == message.version);
        REQUIRE(decoded.type == message.type);
        REQUIRE(decoded.request_id == message.request_id);
        REQUIRE(decoded.payload == message.payload);
        FrameDecoder decoder;
        const auto messages = decoder.feed(encode_frame(message));
        REQUIRE(messages.size() == 1);
        REQUIRE(serialize(messages.front()) == wire);
        REQUIRE_NOTHROW(decoder.finish());
    }
}

TEST_CASE("All documented error codes round trip", "[protocol]") {
    for (const auto code : {"invalid_request", "unsupported_version", "unauthorized", "forbidden",
                            "not_found", "conflict", "rate_limited", "internal_error"}) {
        const Message error{1, "error", "req-001", {{"code", code}, {"message", "Failure"}}};
        REQUIRE(parse(serialize(error)).payload.at("code") == code);
    }
    auto error = examples().at(2);
    error.payload["code"] = "unknown_error";
    expect_error([&] { (void)serialize(error); });
}

TEST_CASE("A frame can arrive one byte at a time", "[framing]") {
    const auto frame = encode_frame(send());
    FrameDecoder decoder;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto messages = decoder.feed(std::span(frame).subspan(i, 1));
        REQUIRE(messages.size() == (i + 1 == frame.size() ? 1 : 0));
        if (!messages.empty()) {
            REQUIRE(messages.front().payload.at("text") == "Привет!");
        }
    }
    REQUIRE_NOTHROW(decoder.finish());
}

TEST_CASE("Multiple frames arrive in one chunk", "[framing]") {
    std::vector<std::uint8_t> combined;
    for (const auto &message : examples()) {
        const auto frame = encode_frame(message);
        combined.insert(combined.end(), frame.begin(), frame.end());
    }
    FrameDecoder decoder;
    const auto messages = decoder.feed(combined);
    REQUIRE(messages.size() == 4);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        REQUIRE(serialize(messages[i]) == serialize(examples()[i]));
    }
    REQUIRE_NOTHROW(decoder.finish());
}

TEST_CASE("Every split point preserves frame boundaries", "[framing]") {
    auto combined = encode_frame(send());
    const auto second = encode_frame(examples().back());
    combined.insert(combined.end(), second.begin(), second.end());
    for (std::size_t split = 0; split <= combined.size(); ++split) {
        CAPTURE(split);
        FrameDecoder decoder;
        auto first = decoder.feed(std::span(combined).first(split));
        auto rest = decoder.feed(std::span(combined).subspan(split));
        first.insert(first.end(), rest.begin(), rest.end());
        REQUIRE(first.size() == 2);
        REQUIRE(first[0].type == "message.send");
        REQUIRE(first[1].type == "message.received");
        REQUIRE_NOTHROW(decoder.finish());
    }
}

TEST_CASE("Oversized and zero lengths fail upon header completion", "[framing]") {
    for (const auto header :
         {std::array<std::uint8_t, 4>{0, 1, 0, 1}, std::array<std::uint8_t, 4>{255, 255, 255, 255},
          std::array<std::uint8_t, 4>{0, 0, 0, 0}}) {
        FrameDecoder decoder;
        REQUIRE(decoder.feed(std::span(header).first(3)).empty());
        expect_error([&] { (void)decoder.feed(std::span(header).last(1)); });
        expect_error([&] { decoder.finish(); });
    }
}

TEST_CASE("EOF at every incomplete header or payload position fails", "[framing]") {
    const auto frame = encode_frame(send());
    for (std::size_t length = 1; length < frame.size(); ++length) {
        CAPTURE(length);
        FrameDecoder decoder;
        REQUIRE(decoder.feed(std::span(frame).first(length)).empty());
        expect_error([&] { decoder.finish(); });
    }
    FrameDecoder empty;
    REQUIRE_NOTHROW(empty.finish());
}

TEST_CASE("The inclusive 64 KiB byte limit uses big-endian framing", "[framing]") {
    auto message = send();
    const auto initial_size = serialize(message).size();
    auto text = message.payload.at("text").get<std::string>();
    text.append(max_payload_size - initial_size, 'x');
    message.payload["text"] = text;
    const auto frame = encode_frame(message);
    REQUIRE(frame.size() == max_payload_size + 4);
    REQUIRE(frame[0] == 0);
    REQUIRE(frame[1] == 1);
    REQUIRE(frame[2] == 0);
    REQUIRE(frame[3] == 0);
    FrameDecoder decoder;
    REQUIRE(decoder.feed(frame).at(0).payload == message.payload);
    REQUIRE_NOTHROW(decoder.finish());
    message.payload["text"] = text + "x";
    expect_error([&] { (void)encode_frame(message); });
    expect_error([&] { (void)parse(std::string(max_payload_size + 1, ' ')); });
}

TEST_CASE("Invalid JSON UTF-8 and BOM are rejected", "[protocol]") {
    for (const auto input : {"", "{", "[]", "null", "{} trailing", "{\"version\":1,}"}) {
        expect_error([&] { (void)parse(input); });
    }
    const auto valid = serialize(send());
    expect_error([&] { (void)parse(std::string("\xEF\xBB\xBF") + valid); });
    auto invalid_utf8 = valid;
    invalid_utf8.insert(invalid_utf8.find("Привет"), 1, static_cast<char>(0xFF));
    expect_error([&] { (void)parse(invalid_utf8); });
    auto message = send();
    message.payload["text"] = std::string(1, static_cast<char>(0xFF));
    expect_error([&] { (void)serialize(message); });
}

TEST_CASE("Required envelope fields reject missing and incorrect types", "[protocol]") {
    const auto valid = json::parse(serialize(send()));
    for (const auto field : {"version", "type", "request_id", "payload"}) {
        auto object = valid;
        object.erase(field);
        expect_error([&] { (void)parse(object.dump()); });
    }
    for (const auto &value : {json(nullptr), json(true), json("1"), json(1.0), json::array()}) {
        auto object = valid;
        object["version"] = value;
        expect_error([&] { (void)parse(object.dump()); });
    }
    for (const auto field : {"type", "request_id"}) {
        for (const auto &value : {json(nullptr), json(42), json(true), json(""), json::object()}) {
            auto object = valid;
            object[field] = value;
            expect_error([&] { (void)parse(object.dump()); });
        }
    }
    for (const auto &value : {json(nullptr), json("text"), json(1), json::array()}) {
        auto object = valid;
        object["payload"] = value;
        expect_error([&] { (void)parse(object.dump()); });
    }
}

TEST_CASE("All message types validate every required payload field", "[protocol]") {
    for (const auto &message : examples()) {
        for (auto it = message.payload.begin(); it != message.payload.end(); ++it) {
            CAPTURE(message.type, it.key());
            auto missing = message;
            missing.payload.erase(it.key());
            expect_error([&] { (void)serialize(missing); });
            auto wire = json::parse(serialize(message));
            wire["payload"].erase(it.key());
            expect_error([&] { (void)parse(wire.dump()); });
            for (const auto &value :
                 {json(nullptr), json(42), json(true), json(""), json::array()}) {
                auto invalid = message;
                invalid.payload[it.key()] = value;
                expect_error([&] { (void)serialize(invalid); });
                wire = json::parse(serialize(message));
                wire["payload"][it.key()] = value;
                expect_error([&] { (void)parse(wire.dump()); });
            }
        }
    }
}

TEST_CASE("Only incoming events have null request identifiers", "[protocol]") {
    for (auto message : examples()) {
        auto wire = json::parse(serialize(message));
        if (message.type == "message.received") {
            message.request_id = "req-event";
            wire["request_id"] = "req-event";
        } else {
            message.request_id.reset();
            wire["request_id"] = nullptr;
        }
        expect_error([&] { (void)serialize(message); });
        expect_error([&] { (void)parse(wire.dump()); });
    }
}

TEST_CASE("Unsupported integer versions have a distinct error", "[protocol]") {
    for (const auto &version :
         {json(0), json(-1), json(2), json(std::numeric_limits<std::uint64_t>::max())}) {
        auto wire = json::parse(serialize(send()));
        wire["version"] = version;
        expect_error([&] { (void)parse(wire.dump()); }, "unsupported_version");
    }
    auto message = send();
    message.version = 2;
    expect_error([&] { (void)serialize(message); }, "unsupported_version");
}

TEST_CASE("Unknown fields are accepted and unknown types are rejected", "[protocol]") {
    auto wire = json::parse(serialize(send()));
    wire["future_field"] = json::array({1, 2});
    wire["payload"]["future_payload_field"] = true;
    const auto decoded = parse(wire.dump());
    REQUIRE(decoded.payload.at("text") == "Привет!");
    REQUIRE(decoded.payload.at("future_payload_field") == true);
    wire["type"] = "future.command";
    expect_error([&] { (void)parse(wire.dump()); });
}

TEST_CASE("A malformed framed message poisons the decoder until reset", "[framing]") {
    FrameDecoder decoder;
    expect_error([&] { (void)decoder.feed(raw_frame("{")); });
    const auto frame = encode_frame(send());
    expect_error([&] { (void)decoder.feed(frame); });
    expect_error([&] { (void)decoder.feed({}); });
    expect_error([&] { decoder.finish(); });
    decoder.reset();
    REQUIRE(decoder.feed(frame).size() == 1);
    REQUIRE_NOTHROW(decoder.finish());
}

TEST_CASE("A later invalid frame fails the entire feed call", "[framing]") {
    auto chunk = encode_frame(send());
    const auto invalid = raw_frame("{");
    chunk.insert(chunk.end(), invalid.begin(), invalid.end());
    FrameDecoder decoder;
    expect_error([&] { (void)decoder.feed(chunk); });
    expect_error([&] { (void)decoder.feed(encode_frame(send())); });
    decoder.reset();
    REQUIRE(decoder.feed(encode_frame(send())).size() == 1);
    REQUIRE_NOTHROW(decoder.finish());
}
