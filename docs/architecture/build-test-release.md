# Build and release architecture

CMake builds native services, invokes the React production build and packages resources appropriate to enabled features and platforms. Build inputs and artifact validation must stay connected so excluded features cannot reappear through stale output. Development uses an isolated build folder reused between edits; packaging, automated tests, review and user acceptance remain separate gates.

[Building](../building.md) · [Build flags](../build-flags.md) · [Agent commands](../../AGENTS.md).

[Engineering reference](../reference/architecture/build-test-release.md).
