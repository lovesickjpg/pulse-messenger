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

At the foundation stage:

~~~sh
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel
~~~

Format changed C++ files with clang-format 18 using the repository configuration.
The GitHub Actions formatting job checks committed .cpp, .hpp, and .h files.

Once protocol tests are introduced, add their CTest command here and to CI. Tests
should cover observable behavior and error cases. The initial executable startup
checks are not a substitute for those tests.

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
