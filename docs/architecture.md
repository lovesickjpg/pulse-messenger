# Architecture plan

Status: protocol framing and validation, TLS transport, the storage layer
(schema, migrations, database worker), and authentication (registration, login,
sessions) are implemented. The lobby is a temporary demo; the conversation
domain layer is proposed.

## Deployment model

One server process owns the database file. Multiple clients communicate with the
server over a persistent connection. The first release targets a Linux server
and a CLI client built on Linux and Windows.

## Components

| Component | Responsibility |
| --- | --- |
| pulse_protocol (implemented) | Frame boundaries, request/response/event types, JSON validation |
| pulse_core | Conversations, membership, message and authorization rules |
| pulse_storage (implemented) | SQL queries, migrations, transactions, database worker |
| pulse_transport (implemented) | TLS handshake and verification, connection lifetime, incremental reads, bounded write queues, deadlines |
| pulse_auth (implemented) | Username/password rules, session tokens, login throttle, Argon2id worker pool |
| Server (implemented) | Loopback TLS accept, per-connection identity and request ordering, auth requests, temporary lobby |
| CLI client (implemented) | Console input, display, connection state, EOF handling |

The domain layer should not depend on sockets or the CLI. Transport translates
validated protocol messages into domain operations. Storage implements persistence.
Add each component with its first behavior rather than committing empty directories.

## Execution model

Each process runs one network event-loop thread. `Connection` owns its socket,
decoder, timers, and a write queue capped at 1 MiB. Async handlers hold shared
ownership; server callbacks use weak references so sessions do not own themselves.
Only one write is active at a time. Its buffer remains alive through cancellation,
even after the queue is cleared.

The server owns active sessions in a set. Closing a session cancels its timers,
closes the socket, clears queued writes, and removes it from the set once.
Ctrl+C, SIGTERM, or the optional run timer close the acceptor and every session.
The event loop then drains canceled handlers. Shutdown discards queued messages.
An ordinary peer EOF between frames instead drains that session's pending writes.

The CLI polls stdin every 25 ms on the same event loop. Linux uses `poll` and
`read`; Windows reads available console key events or checks pipe availability.
This lets the client exit on disconnect while stdin remains open. EOF and `/quit`
stop reading input, wait until every pending request has its reply (ten seconds
at most), then close. TLS has no clean half-close, so the client no longer shuts
down its sending side early.

SQLite runs on a dedicated `DatabaseWorker` thread with a bounded job queue; it
owns the only connection and hands results back to the network executor (see
[storage.md](storage.md)). Argon2id runs on `PasswordHasher`, a two-thread pool
with at most 32 waiting jobs. Both post results back to the network executor and
hold a work guard per job, so `io_context::run()` waits for them. The server
destroys both before the `io_context`; their `stop()` finishes running jobs and
fails queued ones with `Shutdown`.

Each connection's requests run one at a time. While a request waits on the hasher
or the database, later requests from that connection queue (up to 32) and run in
order once it finishes, so a login followed immediately by a send works.
Async completions hold only a weak reference to the connection and drop their
result if it closed in the meantime.

`Connection` runs the TLS handshake before reading or writing anything; frames
queued earlier wait for it. Closing skips the TLS close_notify exchange (it would
race the pending read); frames are length-prefixed, so a cut-off stream is still
detected. Peer EOF and a truncated TLS stream both flush queued replies, then close.

Document ownership, shutdown order, pending operations, and error handling before
adding more network threads. If the network executor becomes multi-threaded,
serialize access to each connection's mutable state.

## Protocol direction

The implemented framing is a 4-byte unsigned big-endian payload length followed by
UTF-8 JSON, with a 64 KiB payload limit. The decoder checks the length before
allocating the payload buffer and handles partial and combined frames.

Requests use a version, type, request_id, and payload. Message submission also
uses a client_message_id for future deduplication. The version 1 wire contract is
documented in [protocol.md](protocol.md). The current server implements only the
temporary lobby; persistent messaging and authorization belong to later stages.

## Persistence direction

Versioned SQL migrations and foreign keys are in place. A message and its
conversation sequence are persisted in the same transaction
(`Database::append_message`). Acknowledge durable acceptance only after that
transaction commits; wiring this to the protocol is the messaging stage.

Reconnection should recover history using a server-issued cursor. Specify how
live events and history overlap, and deduplicate them on the client.

## Security scope

TLS with chain, date, and hostname verification is in place, and authentication
is enforced per request; see [security.md](security.md). Conversation membership
checks come with the messaging stage. The first release does not provide
end-to-end encryption.
