# UAM V5.2 review, round 3: #310, #808, #525

Each branch was diffed against `origin/uam/v5.2-sync-main`, and I read each issue and its comments. Nothing in the repository was edited. My temporary worktrees are removed and the checkout is back at `34a54f2d`.

**What I ran:**
- **Docs check:** `check_docs.py` passes on all three branches.
- **#310 frontend tests:** the focused tests pass (27).
- **#808 tests:** the motion contract (12) and the ChatView performance file (10) pass.
- **#525 tests and bundle:** the store and sidebar tests pass (1,246) with SSH on. A bundle built without SSH passes its type check and build. The leak gate's interface scan finds 1 SSH match and nothing unexcused.
- **#310 native:** `vcs_commit_service.cpp` passes a syntax-only `g++` compile (C++20, all flags on). The project can't be configured on Linux, so the new native tests were not compiled or run. The test file can't be checked without the CEF headers.

---

## v5/feat-310-repository-diffs (#310)

1. **Should-fix: a timed-out read is reported as a lost connection.** `src/app/vcs_commit_service.cpp:261-266, 2122`
   - **Problem:** a probe that timed out on a host that is still connected (slow `git rev-parse` on a large or network-mounted checkout) shows "Lost the connection to the remote host while reading the repository: the command timed out." The runner's own text is also joined mid-sentence with a capital letter ("…repository: The remote runner request failed."). The `canceled` branch can never happen, because `RemoteVcsContext::Run` never sets that field.
   - **Fix:** return an error kind as well as the text. Use two one-line messages: "Reading the repository on the remote host timed out." and "Lost the connection to the remote host while reading the repository." Send the raw runner text to the log only, and drop the cancelled branch.
2. **Nit: a mixed Git and SVN checkout can still lose the error.** `vcs_commit_service.cpp:2119`
   - **Problem:** if the Git probe drops and the reconnect lets the SVN probe succeed, `available` is true and `connection_error` is thrown away. The summary then shows whole-working-copy SVN rows, which the change set out to prevent.
   - **Fix:** if `connection_error` is set and the requested type isn't in `vcs_types`, return the error.
3. **Nit: a stale-result refusal now shows as an error.** `ChatView.tsx:1942` and `vcs_handlers.cpp`
   - **Problem:** with `reportFailure`, the 409 "The chat workspace changed while its repository context was loading." now appears in the summary with Retry. That happens when the host's connection details change but the workspace path doesn't. The 429 and 409 busy replies are reasonable to show; this one is a race.
   - **Fix:** drop it silently or retry once, as the effect does for its own cancellation.
4. **Should-fix (process): verification isn't reported.**
   - **Problem:** the acceptance criteria require exact frontend test counts and a production build, and the commit reports neither. The two new native cases have never been compiled.
   - **Fix:** record full `npm --prefix UI-V2 run test` counts, `run build`, and the two cases `RemoteVcsStatusReportsALostConnectionInsteadOfNoRepository` and `RemoteVcsStatusStillReportsAPlainFolderAsHavingNoRepository` on macOS, then post them on #310.

The other checks are clean:
- **Interface change:** the `DetectRemote` signature change is applied to both implementations and the single caller.
- **Typing path:** nothing new is added; the read runs only when a turn completes.
- **Docs:** `changes.md` is updated in the same commit.
- **Tests:** they exercise the real failure, including a runner killed mid-probe.

**Verdict:** approve once 1 and 4 are done. #310 stays open for the SVN baseline and touch verification.
**Suggestions:** 4
**Screens needing Before/After capture:** the end-of-transcript **Changes from this chat** summary after an SSH host drops. Before it was absent; after it shows the error with Retry, then the recovered list. Capture desktop and the narrow phone companion.

---

## v5/feat-808-motion-loading (#808)

1. **Blocker: the transcript skeleton's lines merge into one solid block.** `ChatView.tsx:3752, 3759` and `SubagentTranscript.tsx:250`
   - **Problem:** `uam-skeleton` was also added to the `.conversation-skeleton__text` containers. Each container now fills its whole column, gaps included, with `--surface-up`. The lines are the same colour, so they disappear into a square-cornered block. The container's own sweep then paints over its lines, because its absolutely positioned `::after` paints after the relatively positioned children. The test hides this: `motionContract.test.ts:96` skips `__text`.
   - **Fix:** remove `uam-skeleton` from the three `__text` divs. Change the test to assert that `__text` class lists do not contain `uam-skeleton`.
2. **Should-fix: the performance claim isn't measured.**
   - **Problem:** this is the V5.2 performance release. The commit says the skeletons are now composited instead of repainted, with no before/after figures, which `testing.md` requires. The new performance case runs in jsdom, where CSS has no effect. It only proves the placeholders don't remount while typing, which is useful but not the claim.
   - **Fix:** cite before/after figures for the history-loading and Settings-loading states, from a DevTools Performance or paint-flashing capture or `perf_suite`. Otherwise reword the claim.
3. **Nit: the Usage skeleton contradicts "the one loading placeholder".** `docs/ui/primitives.md:32`
   - **Problem:** Usage keeps its own `uam-usage-skel` opacity pulse.
   - **Fix:** name Usage as the exception here (`motion.md` already does), or migrate it.
4. **Nit: the docs overstate what users see with motion off.** `docs/ui/motion.md:296`
   - **Problem:** it says that under Motion Off or reduced motion "the surrounding status label carries the loading state". The sidebar's "Loading chats…" and the icon grid's "Loading icons" are screen-reader-only, so sighted users see only still blocks. This was already true before this branch.
   - **Fix:** make those labels visible, or narrow the claim to the transcript and Settings.

The other checks are clean:
- **Motion:** the sweep is transform-only, follows the speed scale, and stops under Motion Off and reduced motion; the placeholders are `aria-hidden`.
- **Typing path:** there is no new per-frame React work.
- **Docs:** they are updated in the same commit.

**Verdict:** changes needed. Fix 1 and add the measurement before review sign-off.
**Suggestions:** 4
**Screens needing Before/After capture:**
- chat history loading skeleton
- subagent transcript loading
- sidebar startup list
- icon fallback and icon-library loading grid
- Settings loading rows

Capture each with Default motion and with Motion Off or reduced motion, in light and dark.

---

## v5/feat-525-disabled-provider-names (#525)

1. **Should-fix: the store's types still say routing always exists.** `UI-V2/src/store/storeTypes.ts:172-175` and `useAppStore.ts:1479`
   - **Problem:** `AppState` still declares `executionRouting` and the two routing actions as always present, and `{}` is cast to that slice type. Every current reader is guarded. But a future unguarded read would compile and then throw only in builds without SSH.
   - **Fix:** make the three members optional, or give them an SSH-only type, so the compiler forces `SSH_ENABLED` guards.
2. **Should-fix: nothing tests the no-SSH behaviour.**
   - **Problem:** the leak gate proves the names are gone, not that the app still behaves.
   - **Fix:** add a Vitest that mocks `config/buildFeatures` with `SSH_ENABLED: false` and covers:
     - automatic routing in `createSession` returns false;
     - `deserializeState` and `applyStatePatch` produce no `executionRouting`;
     - the settings snapshot leaves it out;
     - `NewChatModal` renders with no automatic routing.
3. **Should-fix: the Windows gate change rests on an old base.** `.github/workflows/build-flag-leaks.yml:123`
   - **Problem:** I confirmed the claim through the API: run 44641841609 on `v5.1.0-rc.1` passed all 19 Windows rows. But no native rows have run on `uam/v5.2-sync-main` since, because pull requests skip them without the label. Removing `continue-on-error` could turn the next integration push red on compiler drift.
   - **Fix:** run the workflow with the `build-flags` label or a manual run on this branch, and cite the green Windows rows before merging.
4. **Nit: native code still sends routing state without SSH.** `src/common/config/settings_frontend_json.h:186`
   - **Problem:** builds without SSH still serialise `executionRouting` into every state push. This is native residue and wasted bytes.
   - **Fix:** guard it with `#if UAM_ENABLE_SSH` and re-record the residue, as the native follow-up.

The other checks are clean:
- **Saved routing data:** native code never reads `executionRouting` from a UI settings save, so the stored routing settings stay unchanged on disk.
- **Guards:** the routing dialog, wait notice and Settings page were already guarded.
- **Providers:** builds with SSH behave exactly as before, so all six providers are unaffected.
- **Docs:** they are accurate and in the same commit. Interface SSH → Implemented, with the one reviewed match.

**Verdict:** approve after 1–3. #525 stays open for the computer-use state fields and the native names left behind by the SSH, computer-use and provider flags.
**Suggestions:** 4
**Screens needing Before/After capture:** the New Chat dialog's routing control. In a no-SSH build it should be absent; in a full build the automatic routing choice should be unchanged.
