# Code structure

`src/main.cpp` enters `src/app/`, whose application/domain services coordinate work and persistence. `src/common/` holds shared models, repositories, provider/runtime contracts and platform services; `src/cef/` validates requests and projects native state to React in `UI-V2/src/`. `src/remote/` handles the remote helper, and `src/computer_use/` contains the companion controller. Feature UI calls store/bridge actions, application services enforce ownership and policy, and adapters perform environment-specific work. Keep protocol parsing, OS APIs and repository commands out of React and thin bridge handlers. Share behaviour through interfaces rather than copying provider or platform branches into every caller.

Background work captures the intended chat, host, workspace and operation identity; results must be revalidated before publication. Cancellation, UI closure and process exit are different events. Keep blocking I/O and expensive parsing off the UI thread and bound work, output and retries. Existing exceptions do not authorize new cross-layer shortcuts or an unrelated wholesale refactor.

Paths stored, compared, parsed or written to JSON as text are UTF-8: convert with `PortablePathString`, `Utf8PathString` and `PathFromUtf8` in [`path_utils.h`](../../src/common/paths/path_utils.h). `std::filesystem::path(std::string)`, `.string()` and `.generic_string()` use the Windows code page, so a folder such as `Test Folder ü` would reach UTF-8 consumers as invalid bytes and throw.

Startup bundled Markdown seeding uses an application-owned CEF thread instead of a shared file worker; shutdown revokes publication and joins that thread before CEF/data-root teardown, without interrupting filesystem I/O, so blocked I/O can still delay exit.

[Platform](platform-boundaries.md) · [Providers](provider-boundaries.md) · [VCS](vcs-boundaries.md) · [Data authority](source-of-truth.md) · [Source map](../reference/source-map.md).
