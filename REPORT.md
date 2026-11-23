# Model-catalogue [desync]: review loop result

**Work branch:** `v5/fix-model-catalogue-desync`, final SHA `b8757ecc4291d42f6549b9f705ce6a1c918cc6d4` (2 commits on `origin/uam/v5.2-sync-main` at `6f235eb9`).

## Finding

- **No catalogue-service write bypasses the generation stamp.** This includes the loading and refresh-error flags and the worktree discovery scopes. An independent reviewer agreed.
- **One real memo desync was found and fixed: hosts are trimmed in the key but were not trimmed in the value.**
  - The memo keys a scope by the trimmed execution host, but `CatalogSnapshotForChat` looked up the host's checked CLI version with the untrimmed `chat.execution_host_id`.
  - A chat whose host id has stray whitespace (for example `"local "`) therefore built a Gemini catalogue without reasoning levels. The catalogue list (the slice) builds the same key with them.
  - A trusting push memoises the chat's value; the verifying push builds the list first and reports `[desync]`.
  - The fix (`CatalogSnapshotHost`) is at the read site, so both lookups use the key's trimmed host. The memo is kept.
- **This is probably not the cause of the reported 35-value claude-cli repairs.** Launch-option metadata changes only Gemini models. The likeliest cause there is still on-disk path resolution changing back and forth (the `RecentPathProbe` memo), which is unproven.
- **The `[desync]` line now carries the evidence needed to tell those cases apart.**
  - It gives a count for each kind of repaired value.
  - For catalogue repairs, it also gives how many memoised path probes changed since the previous verifying pass, instead of always blaming a write.

## Tests and checks

- **New test:** `StatePushCatalogueMemoReadsTheTrimmedHostOfAChat` in `tests/interaction_performance_tests.cpp`, using the existing after-every-push cache verification.
  - It also asserts that the push trusted the memo and that the checked version decorates the model, so it cannot pass vacuously.
  - Before the fix it should fail; after the fix it should pass. Both are traced by reading the code (two independent reviews agree), not run.
- **Ran:**
  - `python3 scripts/check_docs.py` passed.
  - `g++ -std=c++20 -fsyntax-only` on `src/cef/state_serializer.cpp` and `tests/interaction_performance_tests.cpp`, with nlohmann/json and stub CEF headers: no errors on changed lines. The remaining errors (`AddFrontendAsset`, `PngDataUrl`) are identical on the base and come from this Linux setup.
  - `recent_path_probe.h` compiled syntax-only on its own.
- **Not run:** the native build and `uam_core_tests`. CMake refuses Linux ("supports only Windows and macOS builds").

## Not verified

- The new test has not been executed on macOS or Windows.
- The cause of the install's 35-value repairs is not established. The next `[desync]` line from that install (kind counts and path-probe changes) should confirm or rule out disk resolution.
- Whether real chats carry untrimmed host ids is unknown. The reviewer noted the many `Trim(chat.execution_host_id)` calls across the code.

## Review rounds

**Round 1:** independent claude-opus-5-5 reviewing the investigation (empty diff): 7 suggested (0 blocker, 5 should-fix, 2 nit), 5 implemented.

1. Mechanism and impact correction: implemented in this report. Flip-flopping resolution fits the burst better than a one-off retarget. A missed key shows an empty list, not a stale one.
2. Host trimmed in the key but not in the value: implemented, with a regression test.
3. Path-probe instrumentation: implemented as a change count in `RecentPathProbe` reported in `[desync]`.
   - Declined part: counting `weakly_canonical` errors in `path_utils.h`. That is a widely shared helper and outside the core fix; the change count already shows whether resolution is moving.
4. Not replacing a good probe value with an error fallback: **declined.** The reviewer made it conditional on finding 3 confirming the cause, which is unproven. The original task rule is not to fix an unproven cause.
5. `[desync]` wording and docs: implemented.
6. Overstated "const cannot modify" guarantee (nit): implemented.
7. Count by kind (nit): implemented.

**Round 2:** one fresh-context claude-opus-5-5 re-check of the fixes: 7 suggested (0 blocker, 1 should-fix, 6 nit), 5 implemented.

1. Path-count wording and per-thread baseline: implemented.
2. Vacuous-pass guard in the test (nit): implemented.
3. Path clause only when a catalogue diverged (nit): implemented.
4. Header comment exception for the `RecentPathProbe` memo (nit): implemented.
5. Include `<utility>` (and `<optional>`) directly (nit): implemented.
6. Other untrimmed host lookups in handlers that are not memoised (pre-existing nit): **declined.** They are outside the memo and not desyncs. Better handled by normalising `execution_host_id` when chats load, as a separate issue.
7. The `[desync]` owner text includes the raw workspace path (pre-existing nit): **declined.** It is a pre-existing log-privacy change outside this fix; worth a separate issue.

## Issue preview

**Title:** `bug(state-push): model catalogue [desync] repairs recur and blame a bypassed write`

The state-push checker repeatedly repairs model-catalogue values, so the model picker can show a wrong list (empty or fallback-only after a missed key) for up to about 3 seconds. Source: logs from a real install.

**Reproduction:** steps not recorded.
**Expected:** a `[desync]` line only when a write skips its stamp, and few repairs once disk paths are stable.
**Actual:** 12 `[desync]` lines over 3 days. Three of them repaired 35 values each, seven came within one minute, and every line blames "a write bypassed a revision stamp".

**Environment:** V5.2 performance branch (`uam/v5.2-sync-main` at `6f235eb9`). OS and workspace layout not recorded.

**Evidence:**
- Observed in code: every catalogue-service write advances the generation.
- Observed in code: a chat with an untrimmed host id read a different Gemini catalogue than its memo key's scope. This is fixed on `v5/fix-model-catalogue-desync` but cannot explain claude-cli repairs.
- Hypothesis: workspace path resolution flipping back and forth on disk (the `RecentPathProbe` memo), which would change every local catalogue at once.

**Reproduction status:** not reproduced on the install. The native build is refused on Linux.

**Acceptance criteria:**
- [ ] The next `[desync]` line from the install, with kind counts and path-probe changes, identifies the source of the 35-value repairs.
- [ ] Catalogue repairs stop where paths resolve the same way every time.

**Review:** self-triage, plus two independent claude-opus-5-5 reviews of the investigation and fix.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code harness, scheduled routine), on behalf of David Taylor.

Follow-up issues worth filing separately: normalise `execution_host_id` when chats load, and keep raw workspace paths out of `[desync]` lines.

## PR preview

**Title:** `fix(state-push): read a chat's catalogue host trimmed and explain [desync] lines`

The model-catalogue memo keyed scopes by the trimmed execution host, but the catalogue looked up the host's checked CLI version untrimmed. A chat whose host id has stray whitespace could memoise a Gemini catalogue without reasoning levels, and each verifying push then reported a `[desync]`. Both lookups now use the key's trimmed host, at the read site, and the memo is kept.

The `[desync]` line now gives a count for each kind of repaired value. For catalogue repairs it also reports how many memoised path probes changed since the previous verifying pass, since on-disk path resolution is the one catalogue input no stamp covers. This does not prove the cause of the reported 35-value claude-cli repairs; it records the evidence needed to.

Related to the model-catalogue `[desync]` report (issue not yet filed).

**Proof:**
- `python3 scripts/check_docs.py` passed.
- `g++ -fsyntax-only` on the changed source and test files (stub CEF headers): no errors on changed lines.
- New case `StatePushCatalogueMemoReadsTheTrimmedHostOfAChat`: written, not run (the native build refuses Linux). macOS and Windows runs pending.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: round 1 7 suggested, 5 implemented; round 2 7 suggested, 5 implemented (14 suggested, 10 implemented). The declined items are listed above. Required Claude review and David's approval are pending.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## Screens needing Before/After capture

None. This is a native state-push and logging change with no UI change.
