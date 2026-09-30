# Contributing to Pulse

## Workflow

1. Start from the latest main branch.
2. Create or select an issue with a concrete acceptance criterion.
3. Create a short-lived branch, such as feat/frame-codec or fix/reconnect.
4. Implement one coherent change.
5. Build the affected targets and run relevant checks.
6. Open a pull request and request review from the other maintainer.
7. Merge after the required checks and review pass.

## Local checks

Initialize and bootstrap the pinned vcpkg submodule as described in README.md, then run:

~~~sh
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel
ctest --test-dir build/dev-debug --output-on-failure --no-tests=error
~~~

Format changed C++ files with clang-format 18 using the repository configuration.
The GitHub Actions formatting job checks committed .cpp, .hpp, and .h files.

Protocol tests cover observable behavior and error cases. For Visual Studio
multi-config builds, pass `-C Debug` to CTest. Executable startup checks are not
a substitute for protocol tests.

Update vcpkg in a separate PR: change the submodule revision and manifest baseline
together, and verify GCC, Clang, and MSVC before merging.

## Code conventions

- Use C++20 and the committed formatting rules.
- Prefer RAII and make resource ownership explicit.
- Explain non-obvious asynchronous lifetimes and cancellation behavior.
- Keep network transport separate from chat logic.
- Give each implemented library a clear CMake target.
- Use parameterized database queries and explicit transactions.
- Keep shared configuration independent of personal machine paths.

## Commit messages

Use short English descriptions, for example:

- build: add CMake presets
- feat: decode length-prefixed frames
- fix: reject oversized messages
- docs: describe authentication responses

These prefixes are a team convention, not a release automation requirement.

## Documentation

Update the README only when a feature is implemented and verified. Keep planned
features visibly marked as planned. Changes to the wire format must also update
docs/protocol.md once it exists.

## Review focus

Check correctness, resource lifetime, error paths, authorization, and consistency
with the documented protocol. The author describes how the change was verified.
