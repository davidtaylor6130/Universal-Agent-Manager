# RENAME_SWAP fallback: final report

Saves that replace an existing file on macOS SMB and exFAT volumes now fall back to a copy-then-rename replace instead of failing. Two independent reviews ran; every blocker and should-fix is implemented, and one minor suggestion and one sub-suggestion were declined. Not run yet: anything on a Mac, a real SMB or exFAT volume, and macOS/Windows compilation.

**Work branch:** `v5/fix-rename-swap-fallback`. Final SHA: `45766aec`. Commits: `103838d1` (fix), `ffbb3115` (round 1 fixes), `45766aec` (round 2 fixes). No PR, issue or comment was created.

## Review rounds
Both reviewers were fresh-context Opus sub-agents. Each got the issue, the diff against `origin/uam/v5.2-sync-main` and the project docs, and was asked to hunt compile errors in the macOS and Windows branches.

**Round 1 (first review of `103838d1`): 13 suggested, 12 implemented.** 0 blockers, 6 should-fix, 7 nits. No compile errors found.
- **Implemented:**
  - Decide a failed install by the destination's identity, so the only copy of the previous file can never be deleted.
  - A held failure never overwrites an existing `.bak`, and conditional chat publication keeps its evidence path.
  - Copy only, no hard link, so the destination never gets a second link and its ctime doesn't change.
  - Tests for the recovery paths.
  - Accurate docs on when the sweep runs and when the copy is synced.
  - Stopped activation refuses a network folder without putting the transfer on hold.
  - `<sys/param.h>` include order, the cache comment, scoping the data-folder claim, plainer warning wording.
- **Declined:**
  - #11 (nit): the instruction-file error can name the wrong recovery file after a double failure. That case is rare and the copy is kept beside the file. The `fcopyfile` part of that finding was added to the Mac check list instead.
  - The lock-stall half of #13: the volume is queried once per chosen folder, so a stall is unlikely.

**Round 2 (re-check of the round 1 fixes): 7 suggested, 7 implemented.** 0 blockers, 3 should-fix, 4 nits.
- **Implemented:**
  - A held copy moves to `<name>.held.<token>`, a name the cleanup sweep never removes, and the error message names it.
  - Matching the old file after a failed rename now also requires the same size and modification time, because smbfs and exFAT can reuse inode numbers.
  - A test for refusing a network folder at stopped activation.
  - The single-link test now proves the old file's identity, link count and ctime are untouched.
  - Tests for the "another file took the name" case and for the conditional path's "error after the rename landed" and step-3 rename failures.
  - Docs list every caller the sweep doesn't cover.
  - The warning paraphrases in the docs match the new wording.
- **Declined:** the sub-suggestion to also align the startup "handoff is held" message. It's latent, because transfers stay disabled (`kTransfersValidated=false`).

## How the fallback works
- It tries `RENAME_SWAP` (macOS) or `RENAME_EXCHANGE` (Linux) first. On `ENOTSUP`, `EOPNOTSUPP`, `EINVAL`, `ENOSYS` or `ENOTTY`, the volume is remembered by `st_dev` for the rest of the process. Any other error still fails the save as before.
- The fallback:
  1. copies the old file (synced when the save is durable, mode kept);
  2. syncs the folder;
  3. does `rename(temp, path)`;
  4. moves the copy to the temp name, where a swap would have left it, so `.bak` and evidence handling are unchanged.
- If the rename fails, the destination's identity, size and modification time decide the outcome:
  - still the old file: the copy is removed and the save fails with nothing changed;
  - the new file: the rename landed despite the error, so the save counts as done;
  - missing: the copy is put back;
  - anything else: the save fails held. The old file is kept as the `.bak` if none exists, otherwise as `<name>.held.<token>`. Conditional publication keeps it at its evidence path.
- **Weaker guarantee:** the fallback is only as atomic as the volume's own rename. A crash can leave a `.tmp.` copy, which is swept at least 24 hours later and only by `AtomicWriteFileDetailed`. A fallback `.bak` carries the time of the save. This is documented in `docs/engineering/durable-writes.md`.
- **Data-folder chooser:** a network folder is refused at selection, arming and activation, following the "local-folder" agreement. A local drive without swap (exFAT) is accepted, with one added line: "This drive cannot replace files in a single step, so a power loss during a save there is riskier." The default root and `UAM_DATA_DIR` aren't checked.

## Tests (all run on Linux)
- **15 cases pass** in a temporary Linux build of the data-folder test binary: 12 for the save fallback, plus the preflight, settings and activation network cases. The 57 transition cases that fail on Linux fail identically on the untouched base.
- **Runner:** `uam_runner` builds on Linux. sqlite.org is blocked here, so the build used the same sqlite source taken from an npm package.
- **Docs:** `python3 scripts/check_docs.py` passes.
- **Not run:**
  - macOS and Windows compilation (the macOS and Windows code was checked by reading, by me and both reviewers);
  - the full core test suite;
  - any real SMB or exFAT volume.

## Needs a real check on the Mac
1. Save an existing memory entry and an existing `AGENTS.md`/`CLAUDE.md` on an SMB share, then on exFAT. Expect success, a `.bak` holding the previous text, and no leftover `.tmp.` or `.held.` files.
2. Confirm that the instruction-file preparation step (`fcopyfile` metadata plus owner/mode check) succeeds on those volumes.
3. Choose a data folder on SMB (expect a refusal) and on exFAT (expect the warning line).
4. Let macOS and Windows CI compile and run the new cases.

## Screens needing Before/After capture
- **Settings → Storage, data-folder choice on an SMB share.** Before: accepted. After: refused with "Choose a folder on a drive in this computer. Network folders cannot hold UAM's data."
- **Settings → Storage, pending choice on an exFAT volume.** After: the status line ends with the no-swap warning.

No React code changed; both texts come from native status and error strings.

## Issue preview
**Title:** `bug(storage): saves that replace existing files fail on SMB and exFAT volumes`

Every durable save that replaces an existing file fails on macOS SMB shares and exFAT volumes. Affected today: updating workspace memory entries in `<workspace>/.UAM`, and saving existing AGENTS.md/CLAUDE.md files in repos on a network share. A data folder on such a volume would fail every chat, settings and folder save after a file's first write.

**Reproduction:** On macOS, open a workspace on an SMB share, let memory write an entry, then update it; or save an existing CLAUDE.md there.
**Expected:** The save succeeds and the previous file becomes the `.bak`.
**Actual:** "Failed to atomically swap destination".

**Environment:** `uam/v5.2-sync-main`, macOS, SMB share (`smbfs`) or exFAT.

**Evidence:** A capability query on a real SMB share reports `smbfs` with `VOL_CAP_INT_RENAME_SWAP` unsupported; local APFS supports it (user-reported). `io_utils.h` used `renameatx_np(RENAME_SWAP)` as the only replace path, with no fallback (observed in source). The data-folder chooser has no filesystem check.
**Reproduction status:** Not reproduced in this session (no Mac or SMB share available).

**Acceptance criteria:**
- [ ] Replacing an existing file on SMB and exFAT succeeds and keeps the `.bak`
- [ ] The swap capability is cached per volume
- [ ] The data-folder chooser handles network and no-swap volumes

**Review:** Self-triaged only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## PR preview
**Title:** `fix(storage): fall back to a durable replace when a volume lacks RENAME_SWAP`

On macOS, replacing an existing file only used `RENAME_SWAP`, which SMB and exFAT refuse, so those saves failed. When the swap is unsupported, the save now copies the previous file, syncs the folder, renames the new file over the old one, and remembers the volume by `st_dev`. A failed rename is resolved by the destination's identity, size and modification time, so the previous file is never lost: it is put back, or kept as the `.bak` or as `<name>.held.<token>`. Other errors still fail. The data-folder chooser refuses network volumes and warns in one line for local volumes that can't swap. The weaker guarantee is documented in `docs/engineering/durable-writes.md`.

**Proof:** `uam_runner` builds on Linux. 15 new cases pass in a temporary Linux build of the data-folder test binary; the 57 transition cases that fail on Linux fail identically on the base. `check_docs.py` passes. Not run: macOS/Windows compilation, the full core suite, and real SMB/exFAT saves on a Mac.

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 (fresh-context review) — suggested 13; implemented 12.
- Agent 2: claude-opus-5-5 (fresh-context re-check) — suggested 7; implemented 7.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.
