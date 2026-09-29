# Pulse Messenger

A C++20 client-server messenger in development, focused on asynchronous networking,
clear protocol design, and reproducible builds.

## Project status

**Foundation stage.** The repository currently contains buildable server and CLI
client entry points, CMake presets, formatting rules, and a CI workflow.

The executable stubs print a status message and exit. Networking, authentication,
chat commands, and message persistence are planned and are not implemented yet.
The CI result is available in the repository's Actions tab after a workflow run.

## Planned functionality

- User registration and authentication.
- Private conversations and group rooms.
- Persistent message history.
- Reconnection and duplicate-message handling.
- TLS-protected client-server connections.
- A command-line client, followed by an optional desktop interface.

## Technology choices

| Area | Choice | Integration status |
| --- | --- | --- |
| Language | C++20 | Configured |
| Build | CMake 3.24+, Ninja | Configured |
| Formatting | clang-format 18 | Configuration and CI check provided |
| CI | GitHub Actions, GCC / Clang / MSVC | Workflow provided |
| Dependencies | vcpkg manifest mode | Planned |
| Networking | Boost.Asio, OpenSSL | Planned |
| Protocol | Length-prefixed JSON, nlohmann/json | Planned |
| Storage | SQLite, SQLiteCpp | Planned |
| Password hashing | libsodium / Argon2id | Planned |
| Logging | spdlog | Planned |
| Testing | Catch2 v3, CTest, sanitizers | Planned |

## Build the foundation

Prerequisites:

- A C++20-capable compiler.
- CMake 3.24 or newer.
- Ninja.

Run from the repository root in a shell that can find these tools:

~~~sh
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel
~~~

On Linux, run the stubs:

~~~sh
./build/dev-debug/bin/pulse-server
./build/dev-debug/bin/pulse-client
~~~

On Windows with the Ninja preset:

~~~powershell
.\build\dev-debug\bin\pulse-server.exe
.\build\dev-debug\bin\pulse-client.exe
~~~

For MSVC, configure a Visual Studio toolchain in CLion or use an x64 Native Tools
Command Prompt. A regular shell may not have the compiler environment configured.
CLion's bundled CMake and Ninja may be available inside the IDE without being on
the terminal's PATH.

For a Release build, use the dev-release configure and build presets.

### CLion

1. Clone the repository and open its root directory.
2. Configure a compiler under Settings → Build, Execution, Deployment → Toolchains.
3. Under CMake, enable the imported dev-debug preset and select the toolchain.
4. Reload the CMake project, then build pulse-server and pulse-client.

If the presets are not listed, use Find Action → Load CMake Presets.

## Repository layout

| Path | Purpose |
| --- | --- |
| apps/server/ | Server entry point |
| apps/client-cli/ | Console client entry point |
| docs/architecture.md | Proposed component boundaries |
| docs/roadmap.md | Milestones and completion criteria |
| .github/workflows/ | Continuous integration |

Protocol, domain, storage, and test directories will be introduced together with
their first implementations.

## Testing

No behavioral tests exist at the foundation stage. The initial CI workflow checks
compilation, executable startup, and formatting. Protocol tests will be introduced
with the first protocol implementation.

## Security scope

TLS and authentication are planned. The foundation does not provide a working
messaging service. End-to-end encryption is outside the first release scope;
in the planned initial architecture the server processes message contents.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Work in short-lived branches, open focused
pull requests, and document the checks actually performed.

## Roadmap

See [the roadmap](docs/roadmap.md) and [the architecture plan](docs/architecture.md).

## License

The project uses the MIT License. See [LICENSE](LICENSE).
