# De-slop native drift: report

The work is pushed to `v5/refactor-deslop-native-drift`, which starts from `origin/uam/v5.2-sync-main` @ `c252f9a9d`.

**Final SHA:** `4cfcffcf8d9c03211afc39945ee30e08ba54de1a`

All seven findings held up when I re-checked them against the code. I made one commit per finding, in the requested priority order:

| # | Commit | Kind |
|---|---|---|
| 1 | `d1ec112d` fix(memory): write worker and library entries through one format | real bug + regression test |
| 2 | `14d39a45` fix(settings): show the setup timeout the runtime uses | latent drift + regression test |
| 6 | `cb8320c9` refactor(state): serialise chat goals in one place | no behaviour change |
| 7 | `a14d08a2` refactor(acp): restore saved tool calls through one mapping | no behaviour change + round-trip guard test |
| 9 | `51857f67` refactor(providers): name the Copilot, OpenCode and Codex version floors | no behaviour change + floor test |
| 12 | `826dc6a2` refactor(vcs): share command helpers whose names say which streams they return | no behaviour change + stream test |
| 8 | `4cfcffcf` refactor(import): share the remote OpenCode and Codex import loop | no behaviour change |

## Where I departed from the scout report, and why

- **1:**
  - I kept the worker-only quality rules in `MemoryService`: the high/medium/low confidence vocabulary, required evidence, and skipping sensitive entries. Manual entries legitimately have no evidence and free-text confidence, and the remote path already applies the worker rules before it calls the store.
  - The cap is 160 **bytes**, not characters. A legacy title of up to 700 bytes still matches by its first 160 bytes, unless a file holds exactly the capped title.
  - The store still matches only entries the Library can list.
  - The worker used to restart the occurrence count at 1 for a file with no count. It now counts that file as one previous occurrence.
- **2:**
  - Every path that fills `app.settings` already normalises it, so the app never showed 60 instead of 600. The defect was drift between two rules, not a visible bug, and the commit says so.
  - I kept the defensive clamps rather than serialising raw values.
- **9:**
  - Codex `/status` (`terminal_native_identity.h`) and the OpenCode fixture-verified lists (`1.17.20`/`1.18.35`) are separate verifications that happen to share a version today, so I did not tie them to these floors.
  - The Codex UAM Control message keeps its exact literal. Codex-off builds also compile that code, and `scripts/build_flag_native_residue.json` records the exact string. Function-local constants plus a `static_assert` tie the check and the message to `codex::kUamControlMinimumVersion`.
  - I left the Copilot managed-read refusal text (literal `1.0.98`) alone because `v5/fix-copilot-protocol-4` rewrites that line. There is a follow-up issue preview below.
- **12:** I kept the one-line `BuildGitCommandInDirectory` aliases. They mean the same thing in both files, and removing them would rewrite about 30 call lines for no safety gain.
- **Parallel branches:**
  - `git merge-tree` is clean against `v5/fix-memory-worker-retry-storm` and `v5/fix-copilot-protocol-4`.
  - `v5/perf-947-live-catalogues` does not exist on the remote, so I could not check my one `state_serializer.cpp` hunk against it.

## Review rounds

- **Round 1** (fresh-context claude-opus-5-5): 8 suggested (1 blocker, 1 should-fix, 6 nits), 7 implemented.
  - The blocker: splitting the Codex message would have leaked new strings into Codex-off builds.
  - Also fixed:
    - The memory docs called an overwrite an agreement; it is now recorded as a gap.
    - The docs now say bytes and ASCII.
    - A legacy long title can no longer shadow a file with the exact title.
    - The store now gives an error message instead of an empty one.
    - The settings handler value goes through the helper, and the commit wording is softened.
    - The architecture-page sentence moved to the source map.
  - Declined: changing the Copilot `1.0.98` refusal text. The protocol-4 branch rewrites that exact line, so it became a follow-up issue preview instead.
- **Round 2** (one fresh-context claude-opus-5-5 re-check, because the round-1 fixes were material): 3 suggested (2 should-fix, 1 nit), 3 implemented.
  - The exact-title test depended on directory order; it now rewrites the legacy file first.
  - The store no longer overwrites same-title files it cannot list.
  - The Codex `static_assert` now ties the floor the check actually uses.

## New issue preview (not filed)

**Title:** `refactor(copilot): build the managed-read refusal text from the stable CLI floor`

> The managed Copilot continuation refusal in `src/common/provider/copilot/cli/copilot_managed_read_policy.cpp` (the `kStatus` branch of `HandleResult`) still spells `1.0.98`. The version gate now uses `uam::copilot::managed_read::kStableCliMinimumVersion` (from `v5/refactor-deslop-native-drift`). The text was left alone because `v5/fix-copilot-protocol-4` rewrites that line. Once both have merged, a floor bump would update the gate but not the message.
>
> **Desired outcome:** the refusal is built from `kStableCliMinimumVersion` and still reads as one plain sentence, for example "Managed Copilot continuation requires stable Copilot CLI 1.0.98 or newer."
>
> **Evidence:** code reading. In the tree merged with protocol-4, the literal is at about line 350. Reproduction: not applicable (latent drift).
>
> **Acceptance criteria:**
> - [ ] No `1.0.98` literal remains in the Copilot managed-read or saved-image code outside `kStableCliMinimumVersion`.
>
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `refactor(native): remove drifted duplicate native helpers and fix the memory title split`

> Seven native findings from the de-slop scout, one commit each. Two fix drift between duplicated rules:
>
> - **Memory:** the local worker capped titles at 700 bytes and the Memory Library store at 160, so the same long title written by both produced a second file and the count never grew. Both writers now use one shared entry format in `common/memory/memory_entry_format.h`.
> - **Settings:** the frontend serializer dropped the "zero means the 600-second default" rule for the provider setup timeout. All readers now use one helper.
>
> The other five remove copies that could drift, with no behaviour change: the goal serialiser, the Claude and Copilot tool-call restore, the provider version floors, the Git/SVN command helpers (renamed for the streams they return), and the remote OpenCode/Codex import loop.
>
> **Proof:**
> - The headless runner (`uam_runner`) builds on Linux. The SQLite amalgamation came from the npm registry because the network proxy blocks sqlite.org.
> - Every changed `.cpp` and test file passes a GCC 13 syntax-only check against CEF header stubs, with no errors in changed code. The Codex-off and OpenCode-off profiles pass for the guarded files.
> - Scratch runs on Linux:
>   - On the old store, a long title already written by the worker gives 2 files, count 1; the new store gives 1 file, count 2.
>   - An unlisted same-title file is left untouched.
>   - A file holding the exact capped title wins in both directory orders.
>   - The tool-call round trip passes.
>   - The VCS helpers return the expected streams.
> - `python3 scripts/check_docs.py` passes, and `git merge-tree` is clean against both parallel branches.
> - Not run: `uam_core_tests` and the desktop build (macOS/Windows only; the CEF SDK cannot be downloaded here), the build-flag leak gate, and live provider checks. The new regression cases (`MemoryWorkerAndLibraryEntriesWithALongTitleShareOneFile`, `SettingsFrontendShowsTheSetupTimeoutTheRuntimeUses`) and guards (`SavedToolCallRestoresEveryPersistedField`, `ProviderVersionFloorsStayWithinTheirInstallFallbacks`, `VcsCommandHelpersNameTheStreamsTheyReturn`) still need a macOS or Windows run.
>
> **Review:** 2 independent reviewers, 2 iterations. Independent claude-opus-5-5 (round 1): 8 suggested, 7 implemented. Independent claude-opus-5-5 (round 2 re-check): 3 suggested, 3 implemented. The Copilot refusal-text change was declined in favour of a follow-up issue, to avoid conflicting with `v5/fix-copilot-protocol-4`.
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

**Screens that need Before/After capture:** none are required, since no React/UI code changed. Optional:
- **Settings → Memory Store:** a long-titled memory saved by a local worker and then added manually shows one entry with count 2 (before: two entries).
- **Settings → Goal Loops:** the setup timeout display is only affected by a stored value of zero or less, which normal loads already normalise, so there is no practical before/after.
