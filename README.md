# Pulse Messenger

A C++20 client-server messenger in development, focused on asynchronous networking,
clear protocol design, and reproducible builds.

## Project status

**Protocol stage.** The repository contains a versioned JSON protocol library,
an incremental frame codec, Catch2 tests, pinned vcpkg dependencies, and CI for
GCC, Clang, and MSVC. See [the v1 contract](docs/protocol.md).

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
| Dependencies | vcpkg manifest mode | Submodule revision and package baseline pinned |
| Networking | Boost.Asio, OpenSSL | Planned |
| Protocol | Length-prefixed JSON, nlohmann/json | v1 framing and validation implemented |
| Storage | SQLite, SQLiteCpp | Planned |
| Password hashing | libsodium / Argon2id | Planned |
| Logging | spdlog | Planned |
| Testing | Catch2 v3, CTest | Protocol tests implemented; sanitizers planned |

## Install dependencies and build

Prerequisites:

- A C++20-capable compiler.
- CMake 3.24 or newer.
- Ninja.
- Git and internet access for the initial dependency installation.

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

CMake installs `nlohmann-json` and `catch2` automatically from `vcpkg.json`.
The vcpkg gitlink and `builtin-baseline` both pin revision
`4cb050be2cfa7a947cdd2dd1a70e24b17774c979` (Catch2 3.16.0, nlohmann-json 3.12.0#2).
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
| tests/protocol/ | Catch2 protocol and framing tests |
| external/vcpkg/ | Pinned dependency manager submodule |
| vcpkg.json | Manifest and pinned package baseline |
| docs/protocol.md | Implemented v1 wire contract and examples |
| docs/architecture.md | Proposed component boundaries |
| docs/roadmap.md | Milestones and completion criteria |
| .github/workflows/ | Continuous integration |

Domain and storage components will be introduced with their first implementations.

## Testing

The Catch2 suite checks all four implemented message types and eight error codes,
byte-at-a-time and combined frames, size boundaries, truncated streams, malformed
JSON/UTF-8, field validation, version compatibility, and decoder failure/reset.
CTest discovers and runs individual test cases. CI bootstraps vcpkg and runs these
tests on GCC, Clang, and MSVC; formatting and stub startup checks also remain.

~~~sh
ctest --test-dir build/dev-debug --output-on-failure --no-tests=error
~~~

For a Visual Studio build use its build directory and add `-C Debug`. To build
without test targets, configure with `-DBUILD_TESTING=OFF`.

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
