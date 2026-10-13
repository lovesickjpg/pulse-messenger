# Storage

Pulse keeps everything in one SQLite file owned by the server process. The
`pulse_storage` library (`include/pulse/storage.hpp`,
`include/pulse/database_worker.hpp`) is the only code that talks to SQL.

## Running with a database

~~~sh
pulse-server --db data/pulse.db
~~~

Opening the file creates missing parent directories, applies pending migrations,
and prints `Database ready: <path> (schema version N)`. A bad path, a corrupt file,
or a schema newer than the binary stops the server before it listens. Without
`--db` the server doesn't touch a database yet; the lobby is still in-memory.

At this stage the server only opens and migrates the database. Accounts, rooms,
and persistent history reach the network in later stages.

## Schema

Times are Unix epoch milliseconds. IDs are SQLite rowids and travel over the wire
as strings. Tables are `STRICT`, and `PRAGMA foreign_keys` is on for every
connection (the open fails if SQLite can't honor it).

| Table | Purpose | Notable constraints |
| --- | --- | --- |
| `users` | Account, Argon2id hash string | `username` unique, ASCII case-insensitive |
| `sessions` | Hash of a login token, expiry, revocation | `token_hash` unique; cascades with the user |
| `conversations` | Direct dialog or room, `last_sequence` counter | `name` unique for rooms; `direct_key` unique per user pair |
| `conversation_members` | Who is in what, with a role | primary key `(conversation_id, user_id)` |
| `messages` | Body, sender, per-conversation `sequence` | unique `(conversation_id, sequence)` and `(conversation_id, sender_id, client_message_id)` |
| `schema_migrations` | Applied versions | created by the migrator itself |

The `(conversation_id, sequence)` unique index is also the history index.
The `client_message_id` key is what makes a retried send detectable; the
messaging stage decides what to answer when it fires.

Membership and permission checks are deliberately **not** in this layer. It keeps
the data consistent; the domain layer decides who may read or write.

## Migrations

Files live in `migrations/NNN_name.sql`, numbered from 1 without gaps. CMake
compiles them into the library at configure time (the generated file is in the
build directory), so a binary always knows its own schema.

- Each file runs in its own `BEGIN IMMEDIATE` transaction together with its
  `schema_migrations` row, so a failure leaves the database untouched.
- Don't put `BEGIN`, `COMMIT`, or `PRAGMA` in a migration.
- Never edit a migration that has shipped; add a new numbered file.
- A database whose newest version is higher than the binary knows is refused.
- Two processes opening the same file are safe: the loser re-checks inside the
  transaction and skips what the winner applied.

## Connection settings

| Setting | Value | Why |
| --- | --- | --- |
| `foreign_keys` | ON | Relations are real constraints |
| `journal_mode` | WAL | Readers aren't blocked by the writer |
| `synchronous` | FULL | A committed message is on disk before we acknowledge it |
| busy timeout | 5 s | Lock contention surfaces as `Busy` instead of hanging |

WAL still allows only one writer at a time. That is plenty for one server
process; the worker serializes writes anyway.

## Threading

`Database` is one connection and must stay on one thread. `DatabaseWorker` owns
a `Database` on a dedicated thread and runs jobs in submission order:

~~~cpp
worker.submit([](Database &db) { return db.find_user("alice"); },
              [](Outcome<std::optional<User>> result) { /* on the network executor */ });
~~~

- Completion handlers run on the executor passed to the worker, never on the
  worker thread. Each pending job holds a work guard, so `io_context::run()`
  doesn't return before its result is delivered.
- The queue is bounded (256 by default). A full queue completes the job with
  `Overloaded` instead of growing without limit.
- `stop()` lets the running job finish, completes queued jobs with `Shutdown`,
  joins the thread, and closes the database. Destroy the worker before the
  `io_context`.
- Opening happens on the worker thread but the constructor waits for it, so
  startup errors throw from the constructor.

## Errors

Everything that leaves the library is a `StorageError` with an `ErrorKind`:

| Kind | Cause | Likely protocol code |
| --- | --- | --- |
| `Conflict` | Unique constraint | `conflict` |
| `NotFound` | Missing row or dangling foreign key | `not_found` |
| `Invalid` | CHECK / NOT NULL / bad page size | `invalid_request` |
| `Busy` | Write lock held past the timeout | `internal_error` (retry) |
| `Migration` | Broken or too-new schema | startup failure |
| `Overloaded` | Worker queue full | `rate_limited` |
| `Shutdown` | Worker stopping | connection closing |
| `Internal` | Anything else | `internal_error` |

## Transactions

`Database::in_transaction(f)` wraps `f` in one transaction and rolls back on any
exception. Calls nest by joining the outer transaction; if an inner one fails,
the outer commit is refused, so swallowing an inner error can't sneak a partial
write through. `append_message` bumps the conversation counter and inserts in one
transaction, so a rejected duplicate doesn't burn a sequence number.

## Backups

`Database::backup_to(path)` runs `VACUUM INTO`: a consistent snapshot taken
while the server keeps running. It refuses to overwrite an existing file and
can't run inside a transaction. To restore, stop the server and put the copy at
the `--db` path. Don't copy a live `.db` file by itself; with WAL it can miss
data that is still in the `-wal` file.

## Tests

`tests/storage/storage_tests.cpp` uses a fresh temporary directory per test case.
It covers migrations (fresh, repeated, too-new), persistence across reopen,
rollback, every unique constraint, foreign keys, sequences and paging, direct
dialog uniqueness, sessions, backups, and the worker (ordering, thread
hand-off, full queue, stop). `tests/transport/integration.py` starts the real
server with `--db`, restarts it, and checks the startup failure paths.

To reset a local development database, stop the server and delete the file and
its `-wal` / `-shm` siblings. Don't do this to data you care about.
