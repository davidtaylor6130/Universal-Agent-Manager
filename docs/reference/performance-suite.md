# Performance suite

`scripts/perf_suite.py` measures the twelve user-facing areas the public [ADEmark](https://github.com/zqkra/ademark) comparison uses (speed, weight and tokens), with methods as close to it as practical, so a performance change can cite the same workload before and after. ADEmark's published results appear only as reference figures; UAM does not run or integrate with that harness. It reuses the timed native cases (their `[TIMING]` and `[SIZE]` lines), the frontend performance suite and the packaged app's existing `[performance]` log lines; it adds no gate. Budgets stay in the tests, as [testing](../engineering/testing.md) describes. Real model token counts need a live provider and are never measured, so token rows report only what UAM adds to a request.

## Pieces

Each piece merges its figures into the `--out` file, so a change can rerun only the piece it affects. Python 3 with the standard library is enough; the `ui` piece needs `npm --prefix UI-V2 ci` first.

```bash
python3 scripts/perf_suite.py fixture /tmp/uam-perf-data                       # inspect the synthetic data root
python3 scripts/perf_suite.py --out Builds/perf.json native Builds/agent/uam_core_tests
python3 scripts/perf_suite.py --out Builds/perf.json ui
python3 scripts/perf_suite.py --out Builds/perf.json app Builds/agent/universal_agent_manager.app
python3 scripts/perf_suite.py report Builds/perf.json                          # markdown against tests/performance_baseline.json
```

On Windows, after `vcvars64.bat` and an app build, use `python` with `Builds\agent\Release\uam_core_tests.exe` and `Builds\agent\Release\universal_agent_manager.exe`; on the shared build box run the pieces inside its test lock, one at a time, with `TEMP` on F:. Run `app` only from a signed-in desktop session: a process started over SSH or by `Win32_Process Create` runs in session 0, without an interactive desktop, where CEF window, GPU and compositing timings do not represent a user's machine. The piece records `windows_session` and the report warns about session 0.

| Piece | Measures |
|---|---|
| `native` | Each case alone in a fresh process: state-push work on 2,000- and 5,000-chat installs (unchanged, one chat changed, large loaded chat changed), streaming push cost, the 2,000-chat memory poll, a cold 10,000-message history page, a cold 1,000-chat catalogue, and the bytes UAM adds to a request (UAM-Control and Computer Use MCP tool schemas, prompt text wrapped around "hello"). |
| `ui` | Runs `npm run test:performance` with `VITE_UAM_PERF_TIMINGS=1`, which makes `expectWithinBudget` print each timing. Loop indices fold into one label (p50, p95, max). Exits with the suite's own result. |
| `app` | Writes a fresh fixture and starts the packaged app on it with an allowlisted environment: fixture home, system-only `PATH`, no tokens, API keys or provider configuration paths. The fixture turns every provider off for this computer, so chats and history still load but no account allowance, version sweep or CLI probe reads the machine's real credentials. Cold start is launch until the UI logs its first native bridge reply on the new data root; relaunch is the median of three later launches, each after a forced stop (SIGTERM to the macOS crash supervisor, `taskkill /F` on Windows), not a clean quit. After a 30 s settle, idle CPU is the process tree's CPU time over 60 s (percent of one core) and RAM at rest is its macOS `phys_footprint` or Windows private working set. Slow poll ticks (≥ 50 ms, with per-stage maxima) and slow state-patch serialization come from the log the app already writes. |

The fixture is 2,000 chats across 20 workspace folders and six providers, twelve short messages each, with one 2,000-message chat; it is synthetic. Because providers are off, idle figures exclude provider version and allowance refreshes a real install with providers on would run.

## Reading the report

The first table has the twelve metrics beside the committed baseline and the published reference figures (T3 Code 2.74 and the leading apps; bare Claude Code CLI for tokens). `*` marks an in-process frontend figure standing in for an end-to-end row (no CEF, IPC or paint) and names what it measures, and `†` marks only what UAM adds to a request. A stand-in is narrower than its row: typing lag comes from input events with at most one chat streaming, not five agents, and the long-thread figure is the bounded 50-message first page UAM renders for a 2,000-message chat. Component figures follow, named by unit suffix. Treat a difference as real only when repeated runs on the same machine agree; hosted runners move by tens of percent.

| Metric | Covered by |
|---|---|
| Cold start, relaunch, RAM at rest, idle CPU | `app`, end to end |
| New thread, typing lag, long thread | `ui` stand-ins (empty chat to usable composer; input to committed draft; 2,000-message history page) |
| "hello" tokens | `native` request overhead only: UAM-Control tool schemas plus prompt text wrapped around the typed prompt, at about 4 bytes per token; text sent separately or at session start is not counted |
| First word, RAM under load, per-thread memory, real-prompt tokens; app-level new thread, typing lag with five agents streaming and long thread | Not yet: they need the packaged app driven through a deterministic fake provider (five streaming agents for typing lag and RAM under load), the planned follow-up; real-prompt tokens stay limited to UAM's own additions |

## CI and baseline

Pull requests into `uam/v5.2-performance` run a report-only **Performance report** job on macOS ARM: `native` and `app` on that run's built test binary and packaged app, merged with the timings the Frontend job recorded, then `report` into the job summary and a `uam-performance-report` artifact. It runs with whatever inputs exist even when an upstream job failed or was skipped (a frontend-only change has no native or app figures), and its measuring steps may fail without failing the job, so the check stays green. `tests/performance_baseline.json` holds the base revision's `native` and `ui` figures from the Windows build box, with the machine and commands in its `meta`; packaged-app figures join it after an interactive-desktop run. A report against a baseline from another machine says so, and its Δ is only indicative. Replace the file in the pull request that intentionally moves the figures, from the same machine. `tests/perf_suite_test.py` checks the parsing and that every named native case still exists.
