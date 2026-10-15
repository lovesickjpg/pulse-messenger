# Pulse Messenger

A C++20 client-server messenger in development, focused on asynchronous networking,
clear protocol design, and reproducible builds.

## Project status

**Identity stage.** Every connection is TLS, and the CLI verifies the server's
certificate and name before sending anything. Users register and log in
(Argon2id password hashes, throttled logins, expiring and revocable sessions), and
only signed-in users can post to and read the temporary `lobby`. Accounts and
sessions persist in SQLite with `--db`. See [docs/security.md](docs/security.md),
[docs/protocol.md](docs/protocol.md), and [docs/storage.md](docs/storage.md).

Lobby messages themselves are still in-memory. Private chats, rooms, persistent
history, and reconnection are still planned.

## Planned functionality

- Private conversations and group rooms.
- Persistent message history.
- Reconnection and duplicate-message handling.
- A command-line client, followed by an optional desktop interface.

## Technology choices

| Area | Choice | Integration status |
| --- | --- | --- |
| Language | C++20 | Configured |
| Build | CMake 3.24+, Ninja | Configured |
| Formatting | clang-format 18 | Configuration and CI check provided |
| CI | GitHub Actions, GCC / Clang / MSVC | Workflow provided |
| Dependencies | vcpkg manifest mode | Submodule revision and package baseline pinned |
| Networking | Boost.Asio, OpenSSL | Asynchronous TLS transport with server verification |
| Protocol | Length-prefixed JSON, nlohmann/json | v1 framing and validation implemented |
| Storage | SQLite, SQLiteCpp | Schema, migrations, worker thread; stores accounts and sessions |
| Password hashing | libsodium / Argon2id | Implemented on a bounded worker pool |
| Logging | spdlog | Planned |
| Testing | Catch2 v3, Python 3, CTest | Protocol, storage, auth, and end-to-end tests; sanitizers planned |

## Install dependencies and build

Prerequisites:

- A C++20-capable compiler.
- CMake 3.24 or newer.
- Ninja.
- Git and internet access for the initial dependency installation.
- Python 3 for the transport integration test (unless `BUILD_TESTING=OFF`).

Clone with submodules, or initialize them in an existing clone:

~~~sh
git clone --recurse-submodules https://github.com/lovesickjpg/pulse-messenger.git
cd pulse-messenger
# For an existing clone instead:
git submodule update --init --recursive
~~~

Bootstrap the pinned vcpkg tool from the repository root. Windows PowerShell:

~~~powershell
.\external\vcpkg\bootstrap-vcpkg.bat -disableMetrics
~~~

Linux:

~~~sh
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics
~~~

CMake installs `nlohmann-json`, `catch2`, `boost-asio` (with SSL), `openssl`,
`libsodium`, and `sqlitecpp` (which pulls in SQLite) from `vcpkg.json`. The first
configure builds OpenSSL from source and can take several minutes. The vcpkg gitlink and `builtin-baseline` both
pin revision `4cb050be2cfa7a947cdd2dd1a70e24b17774c979` (Catch2 3.16.0,
nlohmann-json 3.12.0#2, SQLiteCpp 3.3.3, SQLite 3.53.4).
Update the revision and baseline together in a separate PR with all platform checks.

Run from the repository root in a shell that can find these tools:

~~~sh
cmake --fresh --preset dev-debug
cmake --build --preset dev-debug --parallel
ctest --test-dir build/dev-debug --output-on-failure --no-tests=error
~~~

Use `--fresh` after changing toolchains. The presets use the repository-relative
vcpkg toolchain; `dev-release` inherits it. For Windows/MSVC use `x64-windows`,
and for Linux x86-64 use `x64-linux`. Set `VCPKG_TARGET_TRIPLET` in your ignored
`CMakeUserPresets.json`, or supply it explicitly when configuring:

~~~sh
cmake --fresh --preset dev-debug -DVCPKG_TARGET_TRIPLET=x64-linux
~~~

For Windows, run `cmake --fresh --preset dev-debug -DVCPKG_TARGET_TRIPLET=x64-windows`
in an MSVC x64 compiler environment. Keep personal absolute paths out of shared files.

Alternatively, Windows can use Visual Studio's multi-config generator (as in CI):

~~~powershell
cmake -S . -B build/ci-windows -G "Visual Studio 17 2022" -A x64 "-DCMAKE_TOOLCHAIN_FILE=external/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build/ci-windows --config Debug --parallel
ctest --test-dir build/ci-windows -C Debug --output-on-failure --no-tests=error
~~~

Choose the generator matching your installed Visual Studio version.

## Local demo

Make a development CA and server certificate once (see
[docs/security.md](docs/security.md) for what this does and doesn't trust):

~~~sh
./build/dev-debug/bin/pulse-dev-certs certs/local
~~~

Start the server, then start a client in a couple of other terminals. On Linux:

~~~sh
./build/dev-debug/bin/pulse-server --cert certs/local/server.crt --key certs/local/server.key --db data/pulse.db
./build/dev-debug/bin/pulse-client --ca certs/local/ca.crt
~~~

On Windows with the Ninja preset:

~~~powershell
.\build\dev-debug\bin\pulse-server.exe --cert certs\local\server.crt --key certs\local\server.key --db data\pulse.db
.\build\dev-debug\bin\pulse-client.exe --ca certs\local\ca.crt
~~~

For a Visual Studio build, executables are under `bin/Debug/` instead of `bin/`.
In the client:

| Command | What it does |
| --- | --- |
| `/register <name> <password>` | Create an account (doesn't log you in) |
| `/login <name> <password>` | Log in; the session token stays in memory |
| `/logout` | End the session on every connection using it |
| `/help` | List commands |
| `/quit` | Wait for pending replies, then exit (EOF does the same) |
| anything else | Send to `lobby`; shows as `[message_id] sender: text` |

Only signed-in clients send to and receive from the lobby. Ctrl+C stops either
application.

Server options: `[port]` (default **5555**), `--cert`/`--key` (required),
`--db file` (without it, accounts live in memory), `--session-ttl seconds`
(default 12 hours), `--password-cost interactive|moderate|minimal` (minimal is
for tests), and `--run-for seconds`. The server listens on **127.0.0.1** only.
`--db` creates missing directories and applies migrations; `/data/`, `*.db`,
`certs/local/`, and `*.key` are git-ignored.

Client options: `[port]`, `--host name` (default `localhost`; must match the
certificate), and `--ca file` (otherwise the system trust store). `--help` on
either binary prints usage.

Each connection has a ten-second TLS handshake deadline, a 1 MiB write queue, a
ten-second write deadline, and a five-minute read inactivity timeout. A stalled
or malformed connection is closed; other clients keep running. The CLI has a
five-second connection deadline and waits at most ten seconds for replies after
EOF or `/quit`.

The lobby is still a transport demo: messages and IDs last only for this server
run, with no rooms, deduplication, or delivery recovery yet.

For MSVC, configure a Visual Studio toolchain in CLion or use an x64 Native Tools
Command Prompt. A regular shell may not have the compiler environment configured.
CLion's bundled CMake and Ninja may be available inside the IDE without being on
the terminal's PATH.

For a Release build, use the dev-release configure and build presets.

### CLion

1. Clone with submodules, bootstrap vcpkg, and open the repository root.
2. Configure a compiler under Settings → Build, Execution, Deployment → Toolchains.
3. Under CMake, enable the imported dev-debug preset and select the toolchain.
4. Reload CMake, build, and run pulse-protocol-tests or CTest.

If the presets are not listed, use Find Action → Load CMake Presets.

## Repository layout

| Path | Purpose |
| --- | --- |
| apps/server/ | Server entry point |
| apps/client-cli/ | Console client entry point |
| include/pulse/protocol.hpp | Public protocol API |
| src/protocol/ | JSON validation and incremental frame codec |
| include/pulse/connection.hpp | Connection API, used on one event-loop thread |
| src/transport/ | Reads, ordered writes, deadlines, and connection cleanup |
| include/pulse/tls.hpp | TLS contexts and server-name checks |
| include/pulse/auth.hpp | Username/password rules, tokens, login throttle, Argon2id pool |
| src/auth/ | libsodium-backed auth helpers and the hashing pool |
| apps/server/server.cpp | Accepting connections, auth requests, the lobby |
| tools/dev-certs/ | `pulse-dev-certs`: local CA and server certificate |
| tests/auth/ | Catch2 tests for auth helpers and the hashing pool |
| include/pulse/storage.hpp | Database API: users, sessions, rooms, messages, history |
| include/pulse/database_worker.hpp | Dedicated database thread with a bounded queue |
| src/storage/ | SQLite access, migration runner, worker implementation |
| migrations/ | Numbered SQL migrations, compiled into `pulse_storage` |
| tests/storage/ | Catch2 storage and worker tests on temporary databases |
| tests/protocol/ | Catch2 protocol and framing tests |
| tests/transport/ | Process-level server and CLI integration scenario |
| external/vcpkg/ | Pinned dependency manager submodule |
| vcpkg.json | Manifest and pinned package baseline |
| docs/protocol.md | Implemented v1 wire contract and examples |
| docs/security.md | TLS trust setup, passwords, sessions, what's out of scope |
| docs/storage.md | Schema, migrations, settings, threading, backups |
| docs/architecture.md | Component boundaries |
| docs/roadmap.md | Milestones and completion criteria |
| .github/workflows/ | Continuous integration |

Domain components will be introduced with their first implementations.

## Testing

The Catch2 suite checks all four implemented message types and eight error codes,
byte-at-a-time and combined frames, size boundaries, truncated streams, malformed
JSON/UTF-8, field validation, version compatibility, and decoder failure/reset.
The storage suite runs against temporary database files: migrations (fresh,
repeated, too-new), persistence across reopen, transaction rollback, unique and
foreign-key constraints, gap-free sequences, history paging, sessions, backups,
and the worker thread (ordering, bounded queue, shutdown).
The transport scenario starts actual processes and checks three clients, partial
and combined frames, broken connections, a slow reader, piped CLI input, and
shutdown with active connections, plus `--db` startup, restart, and failure
paths. It also covers registration, login, throttling, logout and revocation
across connections, session expiry, request ordering, rejected certificates
(expired, wrong name, unknown CA), plaintext clients, and that no password or
token reaches the server log. The auth suite tests username/password rules,
tokens, the login throttle, and the hashing pool. CTest runs all of it. CI is configured to run
them on GCC, Clang, and MSVC and check C++ formatting.

~~~sh
ctest --test-dir build/dev-debug --output-on-failure --no-tests=error
~~~

For a Visual Studio build use its build directory and add `-C Debug`. To build
without test targets, configure with `-DBUILD_TESTING=OFF`.

To run just the transport scenario:

~~~sh
ctest --test-dir build/dev-debug -R transport-integration --output-on-failure
~~~

## Security scope

All traffic is TLS 1.2+, and the client verifies the server's chain, dates, and
name before sending anything. Passwords are stored as Argon2id hashes and session
tokens as hashes. The server listens on loopback only, still reads message
contents (no end-to-end encryption), and is meant for local test data for now.
Details and limits: [docs/security.md](docs/security.md).

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Work in short-lived branches, open focused
pull requests, and document the checks actually performed.

## Roadmap

See [the roadmap](docs/roadmap.md) and [the architecture plan](docs/architecture.md).

## License

The project uses the MIT License. See [LICENSE](LICENSE).
