Review loop is done and the fixes are pushed to `v5/feat-routing-load-signal`. The desktop app and the native core test suite still have not been built or run, because both need macOS or Windows.

**New SHA:** `8bd337b20ef0924dca63abcc9b1a1f9affda78ef` (`8bd337b2`). The branch has three commits: 3e26f7f3 (the feature), e1cbe9f7 (round-1 fixes) and 8bd337b2 (round-2 fixes).

**Review rounds**

| Round | Reviewer | Suggested | Implemented |
|---|---|---|---|
| 1 | independent, fresh context, claude-opus-5-5 | 16 (0 blocker, 9 should-fix, 7 nit) | 16 |
| 2 | re-check of the round-1 fixes, fresh context, claude-opus-5-5 | 4 new (1 should-fix, 3 nit) | 4 |

- Round 2 found 15 of the 16 round-1 items fully fixed. The 16th (the live-region list) was partly fixed: its new placement looked detached from the notice, and that is round 2's should-fix.
- Nothing was declined.
- The round-1 fixes that change behaviour:
  - Idle computers, including ones turned on for routing, are no longer sampled. Admission doesn't read load yet, so sampling them only cost state pushes. Load is read only for a computer with live or waiting work, or while settings show load.
  - Builds without SSH read and push nothing.
  - State is pushed only when the rounded figures on screen change.
  - A reader stops at once when a host's install fence or a missing helper blocks connections.
  - Figures count as stale after 15 s instead of 30 s.
  - The chat header chip is now a button named by its own text.
  - The per-computer wait list sits outside the notice's live region.
  - The Live load section moved to the end of the routing page.

**Issue preview 1**
```markdown
feature(routing): live load signal per computer and visible routing reasons

Routing chooses a computer by comparing fixed chat counts and nothing reads machine load, so it can overload a computer. Users see one generic wait sentence with no reason per computer and no in-use or waiting counts.

**Desired outcome:** Each computer reports cores, load (or CPU %), available memory and the helper's live process count, through a versioned runner request; older helpers read as unknown, never idle. The app samples this off the UI thread at most every 10 s per computer. The interface shows a per-chat host chip ("host A · 3/8"), a reason for each computer a waiting chat skipped, and live load, in-use and waiting figures per computer in settings. Admission decisions do not change.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** Observed in source: `execution_routing_service.cpp` ~111–126 compares static counts; the runner protocol has no load method; `execution_routing_integration.cpp:81` holds the only wait text, shown behind `SSH_ENABLED` (`MainPanel.tsx:377`).
**Reproduction status:** Source inspection only; no live multi-host run.

**Acceptance criteria:**

- [ ] A helper without the capability shows unknown load, never zero
- [ ] Readings never block the UI thread and are at most 10 s old
- [ ] A waiting chat lists each computer it skipped with a reason
- [ ] Settings show load, in-use and waiting figures per computer
- [ ] Admission outcomes are unchanged

**Review:** Self-triaged; no independent review.

**Prepared by:** Claude Opus 5.5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.
```

**Issue preview 2**
```markdown
feature(routing): load-aware admission policy

Routing can now see live load per computer, but admission still uses concurrent-session counts alone, and idle computers are not sampled. Before load affects admission, a product decision is needed.

**Desired outcome:** Decide which figure holds or steers work (load per core, available memory, helper processes, or a mix) and at what thresholds. Decide whether unknown load blocks, counts as full or is ignored, and whether the local computer becomes an automatic candidate. Decide how a computer shared with other desktops or helper versions is counted. Once admission uses load, sample computers that are turned on for routing even when idle.

**Evidence:** Recorded as open in `docs/documentation-review.md#routing-load-admission`. `runnerProcesses` covers only the answering helper's service and includes short helper commands.
**Reproduction status:** Not applicable.

**Review:** Self-triaged; no independent review.

**Prepared by:** Claude Opus 5.5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.
```

**PR preview**
```markdown
feat(routing): live load signal per computer and visible routing reasons

Adds a versioned `host.load` runner request (`hostLoad` capability 1). It reports cores, one-minute load average (CPU % on Windows), available memory and the helper's live process count for every connected desktop. Older helpers report unknown load, never idle, and the local Mac reports the same figures.

A background sampler reads every 10 s, on its own thread per computer, only while that computer has live or waiting work or settings show load. Readings older than 15 s count as stale, and a reader stops at once when its host's install fence or a missing helper blocks connections. State is pushed only when the rounded figures on screen change; builds without SSH read nothing.

Routing decisions now record a reason for each computer they skip, as diagnostics only: admission is unchanged. The chat header shows "host A · 3/8", the wait notice lists each computer skipped and why, the SSH routing page ends with a Live load section, and Compute Pools rows split their counts by computer.

Visual evidence: not captured. Before/after needed for the chat header chip, the routing wait notice, the SSH routing Live load section (settings page and New chat dialog) and Compute Pools rows.

**Proof:**
- Runner-only build on Linux passed. SQLite was linked from the system 3.45 package because the pinned download host is blocked here.
- The real runner binary answered `host.load` over its direct bridge with capability 1 and live figures.
- New native cases were compiled on Linux with a minimal stand-in test harness against the runner objects: 10 pass (wire format, live process count across two sessions, five sampler cases including `Drop`, three skip-reason cases). The 9 existing service-only routing cases also pass, showing admission is unchanged. The sampler cases passed 20 repeat runs and a ThreadSanitizer run with no reports.
- The changed app files parse with `-fsyntax-only` against stand-in CEF headers, with `UAM_ENABLE_SSH` both 1 and 0; the CEF download is blocked here.
- Not run: the desktop app build, `uam_core_tests` (including the new cases in `execution_routing_tests.cpp` and `compute_pool_tests.cpp`), CTest gates, and macOS or Windows compilation of `host_load.cpp`.
- UI-V2: `npx vitest run` 240 files and 4,223 tests pass; `npx tsc --noEmit -p .` and `npm run build` pass.
- `python3 scripts/check_docs.py` passes.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 20 suggested, 20 implemented (round 1: 16 suggested, 16 implemented; round 2 re-check: 4 suggested, 4 implemented). Required Claude review of the React changes is covered by these rounds; David's approval is pending.

**Created with:** Claude Opus 5.5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.
```

**Screens needing Before/After capture:** the chat header host chip (remote chat), the routing wait notice with its per-computer list, the SSH routing Live load section (settings page and New chat routing dialog), and Compute Pools rows with the per-computer split.
