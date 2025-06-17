# Guidelines for Codex Agents

- Always run `make` to build the server and client when changes are made to the C++ sources. Binaries are produced in the `dist/` directory.
- If the Android client is modified, verify it builds with `./gradlew assembleDebug` inside `android-client`.
- There are currently no automated tests. Ensure compilation succeeds without errors.
- When adding features, keep the code portable and compatible with both x86_64 and ARM64 compilers.
