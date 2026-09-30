# Pulse protocol v1

This document defines the first text wire format and the types implemented by
`pulse_protocol`. The library serializes and validates messages and incrementally
decodes frames. It does not send messages over a network, authenticate users,
store messages, or implement deduplication. The server and client remain stubs.

## Framing and limits

Each frame consists of a four-byte unsigned payload length in network byte order
(big-endian), followed by exactly that many bytes of UTF-8 JSON. The length counts
JSON bytes, not Unicode characters, and excludes the four-byte header. The valid
range is **1 through 65,536 bytes (64 KiB), inclusive**. There is no terminator,
padding, compression, or byte-order mark in the format.

For example, a JSON payload of 258 bytes begins with `00 00 01 02`. The payload
must be a single JSON object; trailing whitespace is allowed, but trailing JSON
values, invalid UTF-8, and malformed JSON are rejected.

A stream read can contain part of a header, part of a payload, several complete
frames, or a complete frame followed by part of another. Receivers must preserve
partial data between reads. The decoder checks the announced length immediately
after receiving the header and **before allocating a payload buffer**. A zero or
oversized length is rejected without consuming the announced payload.

End-of-stream between frames is valid. End-of-stream after one to three header
bytes, or before all announced payload bytes arrive, is a truncated-frame error.
Framing and message-validation failures make the decoder unusable until reset;
the application should close that connection. Reset starts a new stream and
discards partial data. An incomplete frame is not an error until end-of-stream
is reported.

## Envelope

Every message is an object with these required fields:

| Field | JSON type | Meaning |
| --- | --- | --- |
| `version` | integer | Protocol version; currently `1` |
| `type` | string | One of the message types below |
| `request_id` | string or null | Nonempty string for requests and responses; null for unsolicited events |
| `payload` | object | Fields specific to the message type |

Identifiers are **nonempty JSON strings**, including values that happen to look
numeric. Integers such as `42` are not interchangeable with the string `"42"`.
All required strings below must be nonempty; their byte length is bounded by the
overall frame limit. Whitespace-only strings are accepted by the codec. Business
rules such as text normalization and identifier syntax belong to later stages.

## Implemented types

| Type | Kind | Required payload fields (all strings) |
| --- | --- | --- |
| `message.send` | Client request | `conversation_id`, `client_message_id`, `text` |
| `message.send.result` | Successful response | `conversation_id`, `client_message_id`, `message_id` |
| `error` | Failed response | `code`, `message` |
| `message.received` | Server event | `conversation_id`, `client_message_id`, `message_id`, `text` |

The codec verifies these fields and their types. It does not verify that a
conversation exists, that a user may access it, or that identifiers in a response
match an earlier request. Those checks require application state.

A request's `request_id` correlates that particular request with its response.
The successful result or error must echo the request's `request_id`. An incoming
event is unsolicited and has `request_id: null`; it is not a response and must
not complete a pending request. Message types determine this distinction rather
than arrival order.

`client_message_id` identifies one logical attempt to send a message. The client
must retain it when retrying the same logical send, while each request may use a
new `request_id`. Clients must generate both identifiers with sufficient
uniqueness, for example random UUIDs. The short identifiers in the examples are
illustrative. A future server will use `client_message_id` for deduplication; this
stage only preserves and validates the field, without promising exactly-once
delivery or defining deduplication scope and retention.

### Send request

```json
{
  "version": 1,
  "type": "message.send",
  "request_id": "req-001",
  "payload": {
    "conversation_id": "42",
    "client_message_id": "attempt-001",
    "text": "Привет!"
  }
}
```

### Successful response

```json
{
  "version": 1,
  "type": "message.send.result",
  "request_id": "req-001",
  "payload": {
    "conversation_id": "42",
    "client_message_id": "attempt-001",
    "message_id": "msg-100"
  }
}
```

### Failed response

```json
{
  "version": 1,
  "type": "error",
  "request_id": "req-001",
  "payload": {
    "code": "forbidden",
    "message": "You cannot send messages to this conversation."
  }
}
```

### Incoming event

```json
{
  "version": 1,
  "type": "message.received",
  "request_id": null,
  "payload": {
    "conversation_id": "42",
    "client_message_id": "attempt-001",
    "message_id": "msg-100",
    "text": "Привет!"
  }
}
```

## Errors

The `error` response's `payload.code` must be one of these values:

| Code | Meaning |
| --- | --- |
| `invalid_request` | Malformed JSON, missing or mistyped fields, unknown message type, or invalid field value |
| `unsupported_version` | An integer protocol version other than `1` |
| `unauthorized` | Authentication is absent or invalid |
| `forbidden` | The authenticated user lacks permission |
| `not_found` | The requested resource does not exist |
| `conflict` | The operation conflicts with current state |
| `rate_limited` | The request exceeds an application rate limit |
| `internal_error` | The server cannot complete the operation because of an internal failure |

`payload.message` is a nonempty, human-readable string, not a stable machine
identifier. Clients should branch on `code`, not parse `message`. Unknown error
codes are rejected by the v1 codec. This stage validates all eight codes; only
`invalid_request` and `unsupported_version` describe local validation failures.
The other codes describe future application behavior.

Local decoding exceptions are not automatically transmitted as `error` messages.
Framing failures and truncated streams are transport-level errors. A malformed
message may have no trustworthy `request_id`; the application must not invent a
correlation identifier or send an invalid error envelope. Connection handling and
the policy for replying to valid, correlated requests are future work.

## Version compatibility

`version` must be a JSON integer. Strings such as `"1"`, floating-point numbers
such as `1.0`, booleans, and null are invalid requests. A syntactically valid
integer representable by the JSON library (signed or unsigned 64-bit) other than
`1` produces `unsupported_version`; numbers outside that range are not supported
integer versions. There is no automatic
downgrade or version negotiation in v1.

Receivers ignore unknown fields in both the envelope and payload. Required known
fields must still have their documented types and valid values. This permits
optional additive fields within version 1. Senders must not depend on an unknown
field being preserved when a message is decoded and serialized again.

Unknown message types are `invalid_request`. Adding a mandatory field, changing
an existing field's type or meaning, changing framing, or requiring a new message
type needs an explicitly compatible extension or a new protocol version; existing
v1 receivers are not assumed to understand it. New versions and negotiated
capabilities must be specified before they are used.

## Library API

Include `<pulse/protocol.hpp>` and link the CMake target `pulse_protocol`. The
functions and types are in `pulse::protocol`:

- `serialize(Message)` validates a message and returns UTF-8 JSON.
- `parse(std::string_view)` validates bounded JSON and returns a `Message`.
- `encode_frame(Message)` returns the four-byte header followed by JSON bytes.
- `FrameDecoder::feed(std::span<const std::uint8_t>)` accepts the next bytes and
  returns all complete messages from a successful call, in stream order.
- `FrameDecoder::finish()` reports end-of-stream and detects truncation.
- `FrameDecoder::reset()` discards stream state for a new connection.

`Message::request_id` is `std::optional<std::string>`; `std::nullopt` represents
the event's JSON null. Validation failures throw `ProtocolError`; `code()` returns
the machine-readable error code and `what()` returns diagnostic text.

If a `feed()` call contains valid frames followed by an invalid frame, the call
throws and does not return the earlier messages from that call. It does not roll
back messages returned by previous calls. The caller must handle the failure and
close the connection. Transport code must not assume partial success from a call
that throws.

## Verification

The tests in `tests/protocol` cover round trips for all four implemented types and
all eight error codes, byte-at-a-time delivery, several frames in one chunk,
payloads at the size limit, oversized and zero length headers, truncated headers
and payloads, malformed JSON and UTF-8, invalid field types, unknown versions and
types, ignored additional fields, and decoder failure/reset behavior.

After configuring and building the project, run:

```sh
ctest --test-dir build/dev-debug --output-on-failure --no-tests=error
```

For a Visual Studio multi-configuration build, add `-C Debug` and use that build's
directory. See the README for dependency bootstrap and build commands.
