# Report: provider handoff staging refactor

**Work branch:** `claude/refactor-provider-handoff-staging`, pushed. `v5/refactor-provider-handoff-staging` was refused with a 403, so the `claude/` fallback is used.
**Final SHA:** `f06815de1c854951ddeeaee466da7dc1cd341035`, two commits on `origin/uam/v5.2-sync-main` @ `6f235eb9`:
- `5aca9441`: refactor
- `f06815de`: review fixes

## Interface method
`src/common/provider/provider_runtime.h`:
```cpp
struct ProviderCliHandoffStaging
{
	const std::string& host_platform;
	const std::string& directory;
	const std::string& context_path;   // conversation.md, already written
	const std::string& snapshot;
	std::function<bool(const std::string& path, const std::string& content)> stage_file;
};
virtual bool StageCliHandoffContext(const ProviderCliHandoffStaging&, std::vector<std::string>& argv, std::string& error) const { return true; }
```

## Per-provider moves
- **Claude:** `--append-system-prompt-file <context_path>`, moved unchanged.
- **Copilot:** writes `.github/instructions/handoff.instructions.md` with the `applyTo: "**"` header, moved unchanged.
- **Gemini:** writes `GEMINI.md` and adds `--include-directories <dir>`, moved unchanged.
- **Codex and OpenCode:** use the empty default, as before.
- **Antigravity:** an override returns the old "does not support conversation context files" refusal. The earlier Antigravity guard already refuses first, so this path is unreachable today; the override keeps it refusing if that ever changes.
- **Exact-id matching:** the launcher refuses any id that is not an exact included provider id (aliases, unknown ids, excluded providers) with the same error. `ResolveById` would otherwise accept aliases.
- **`ReusesCodexHandoffSession(chat)`:** replaces five identical copies (the audit found four; the fifth is in the async short-circuit). After review it also replaces the two inverted checks in the async import path.
  - The remote copy in `terminal_launch.h:369`, keyed on the launch profile, stays spelled out. This is recorded in `docs/architecture/provider-boundaries.md`.
- **Build flags:** guards are unchanged. The moved strings now build only with their own provider.
- **Docs:** `provider-boundaries.md` describes the staging boundary and the remaining exceptions; `source-map.md` has a new CLI View handoff row.

## Tests
Both are in `tests/terminal_tests.cpp`, with each provider case behind its flag:
- **`CliHandoffStagingIsOwnedByEachProviderRuntime`:** exact files and arguments per runtime on POSIX and Windows paths, a refused file write, and the Codex, OpenCode and Antigravity cases.
- **`CliHandoffLauncherStagesProviderFilesAndArguments`:** the local launcher end to end for Claude, Copilot, Gemini and OpenCode, plus the bound-Codex no-op and the Antigravity refusal.

## Review rounds
1 independent reviewer, 2 iterations.

- **Round 1 (independent claude-opus-5-5):** 5 suggested, 5 implemented.
  1. Blocker, implemented: the Claude launcher test compared strings that differ on Windows (the launcher joins with `/` for an unnamed host platform). It now compares paths.
  2. Should-fix, implemented: use the helper at the two inverted Codex sites. The `terminal_launch.h` copy was not converted; it is keyed on `provider.id` in a header, so converting it would mean exporting the file-local helper. It is recorded as a known exception in the docs instead.
  3. Nit, implemented: `[[maybe_unused]]` on test flags shared across provider-guarded blocks.
  4. Nit, implemented: simplified the Copilot return.
  5. Nit, implemented: dropped the assertion that pinned the unknown-provider default.
- **Round 2 (fresh-context claude-opus-5-5 re-check of `f06815de`):** 0 findings.

## Proof (what was actually run)
- `python3 scripts/check_docs.py`: passed.
- GCC 13 and Clang 18 `-fsyntax-only`: no errors on the launcher, the six runtimes and `terminal_tests.cpp`.
  - Profiles: all providers on, and each provider off in turn. The reviewers also checked single-provider profiles, with `-Wall -Wextra` for the re-check.
  - Linux only, using CEF headers from GitHub with stubbed generated headers.
- **Not run:** the native build, `uam_core_tests` (including both new tests), the leak-gate builds, the remote handoff path and live provider CLIs. CMake refuses Linux; these need macOS/Windows.

## Screens needing Before/After capture
None. This is a native-only refactor with no UI change.

## Issue preview
**Title:** `refactor(terminal): move CLI handoff staging behind provider runtimes`

The CLI View launcher (`terminal_provider_cli.cpp` ~666-684) branches on provider id to stage handoff context: Claude's `--append-system-prompt-file`, Copilot's `.github/instructions/handoff.instructions.md`, and Gemini's `GEMINI.md` with `--include-directories`. The Codex bound-thread condition is copied in five places. The provider-boundaries agreement puts provider launch arguments behind runtime adapters. Found in a read-only audit of `uam/v5.2-sync-main`; the cleanup is approved by the product owner.

**Desired outcome:** one `IProviderRuntime` method with an empty default stages provider-native handoff context, and the Codex condition is named once. Behaviour stays the same for all six providers, and the `UAM_ENABLE_RUNTIME_*` guards and leak gate stay intact.

**Evidence:** the branch and duplicate copies were confirmed by reading the source at `6f235eb9`.
**Reproduction status:** not applicable (refactor).

**Acceptance criteria:**
- [ ] The launcher has no per-provider staging branch
- [ ] Claude, Copilot and Gemini stage identical files and arguments
- [ ] Codex, OpenCode and Antigravity behaviour is unchanged
- [ ] Terminal tests pass on macOS and Windows
- [ ] Build-flag leak gate passes

**Review:** self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session, scheduled task), on behalf of David Taylor.

## PR preview
**Title:** `refactor(terminal): move CLI handoff staging behind provider runtimes`

CLI View handoff staging moves from provider-id branches in the terminal launcher into `IProviderRuntime::StageCliHandoffContext`, whose default does nothing.
- Claude, Copilot and Gemini take over their existing code unchanged.
- Codex and OpenCode keep the empty default.
- Antigravity keeps its old refusal through an override.
- Ids that are not exact included provider ids keep the launcher's refusal, so alias handling is unchanged.
- The Codex bound-thread condition is now `ReusesCodexHandoffSession(chat)`. It replaces five copies and the two inverted checks in the async import path. The remote launch check in `terminal_launch.h` stays spelled out and is recorded in the docs.
- Build guards are unchanged, and the moved strings now build only with their provider.
- The architecture docs and source map are updated.

**Proof:**
- `python3 scripts/check_docs.py` passed.
- A syntax-only compile with GCC 13 and Clang 18 found no errors in the launcher, the six runtimes and `terminal_tests.cpp`, with all providers on and with each provider off. This used stubbed generated CEF headers on Linux.
- Not run: the native build, `uam_core_tests` (including the two new cases), the leak-gate builds and the remote handoff path. They need macOS/Windows.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 5 suggested, 5 implemented (one partly: the `terminal_launch.h` copy is documented rather than converted). Re-check: 0 findings.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

Related to: (issue number once filed)
