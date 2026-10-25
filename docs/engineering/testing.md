# Testing

Choose tests by changed behaviour and failure risk, not a required test count. A defect fix needs a focused case that exercises the actual failure and passes after the correction where practical; cover material failure, ownership, persistence and recovery boundaries affected by the change. Do not mirror implementation details, add trivial tests for wording-only edits, weaken assertions or delete failing cases to produce a green result. Documentation-only work uses navigation, coverage and consistency checks unless executable tooling changes require their own verification.

| Change | Expected verification |
|---|---|
| Native behaviour or persistence | Build the affected test target, run exact connected cases, then the required complete native inventory and remaining CTest gates after connected edits settle. Include round-trip/save-failure and stale-result cases where those boundaries change. |
| React/UI behaviour | Run focused regressions during iteration, then the frontend suite and production build; visually check changed states/themes and retain required Claude review. |
| Platform/provider/VCS adapter | Exercise the shared contract and affected adapter; explicitly report unavailable OS, authenticated CLI or remote acceptance. |
| Performance | Reproduce the costly path and measure the same workload before/after; preserve correctness and timing budgets. A smaller function or a passing build is not measured improvement. |
| Build/packaging/exclusion | Verify the affected configuration and resulting artifact, including shipped resources and excluded-feature mentions where applicable. |

Reuse one isolated build folder and installed dependencies. Run the connected checks first, batch related edits before the final broad run and repeat passed gates only after a new change, failure or unresolved concern. Exact commands and mandatory gates remain in [AGENTS.md](../../AGENTS.md). Hosted CI runs the native core inventory serially and reruns each failed case once in a fresh process; a case passes only if that retry passes, the retried names stay in the log, and budgets and assertions are unchanged. This tolerance applies only to shared CI runners, not local or acceptance runs. Record command, revision, result and limitations; distinguish reproduced baseline failures from new regressions, and never call a failing full gate passed because focused cases succeeded. Builds, tests and visual checks do not constitute user sign-off.
