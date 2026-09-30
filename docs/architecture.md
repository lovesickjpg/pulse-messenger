# Architecture plan

Status: protocol framing and validation are implemented in pulse_protocol.
The application entry points remain stubs; the remaining components are proposed.

## Deployment model

One server process owns the database file. Multiple clients communicate with the
server over a persistent connection. The first release targets a Linux server
and a CLI client built on Linux and Windows.

## Components

| Component | Responsibility |
| --- | --- |
| pulse_protocol (implemented) | Frame boundaries, request/response/event types, JSON validation |
| pulse_core | Conversations, membership, message and authorization rules |
| pulse_storage | SQL queries, migrations, transactions |
| Server transport | Connections, TLS, timers, serialized write queues |
| CLI client | Command parsing, display, connection state |

The domain layer should not depend on sockets or the CLI. Transport translates
validated protocol messages into domain operations. Storage implements persistence.
Add each component with its first behavior rather than committing empty directories.

## Execution model

Start with one network event-loop thread. Give each connection explicit lifetime
and one ordered write queue with a bounded size.

Run SQLite operations on a dedicated database worker. Run expensive password
hashing in a bounded worker pool. Return results to the network executor.

Document ownership, shutdown order, pending operations, and error handling before
adding more network threads. If the network executor becomes multi-threaded,
serialize access to each connection's mutable state.

## Protocol direction

The implemented framing is a 4-byte unsigned big-endian payload length followed by
UTF-8 JSON, with a 64 KiB payload limit. The decoder checks the length before
allocating the payload buffer and handles partial and combined frames.

Requests use a version, type, request_id, and payload. Message submission also
uses a client_message_id for future deduplication. The version 1 wire contract is
documented in [protocol.md](protocol.md). Networking and business operations are
not yet implemented.

## Persistence direction

Use versioned SQL migrations and foreign keys. Persist a message and assign its
conversation sequence in the same transaction. Acknowledge durable acceptance
only after the transaction commits.

Reconnection should recover history using a server-issued cursor. Specify how
live events and history overlap, and deduplicate them on the client.

## Security scope

Before sending credentials outside local development, implement TLS with peer
and hostname verification. Enforce authentication and conversation membership
on the server. The first release does not provide end-to-end encryption.
