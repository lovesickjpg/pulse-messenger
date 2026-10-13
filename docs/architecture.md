# Architecture plan

Status: protocol framing and validation, local TCP transport, the CLI lobby demo,
and the storage layer (schema, migrations, database worker) are implemented. The
network path doesn't use storage yet, and the domain layer is proposed.

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
| pulse_transport (implemented) | Connection lifetime, incremental reads, bounded write queues, deadlines |
| Server (implemented) | Loopback accept, session ownership, temporary lobby broadcast |
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
half-close the socket after flushing requests and keep reading replies.

SQLite runs on a dedicated `DatabaseWorker` thread with a bounded job queue; it
owns the only connection and hands results back to the network executor (see
[storage.md](storage.md)). Expensive password hashing will use a separate bounded
worker pool. The server destroys the worker before the `io_context`; `stop()`
finishes the running job and fails queued ones with `Shutdown`.

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

Before sending credentials outside local development, implement TLS with peer
and hostname verification. Enforce authentication and conversation membership
on the server. The first release does not provide end-to-end encryption.
