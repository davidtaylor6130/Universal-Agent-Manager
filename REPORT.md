# AI review → command allow/ask/deny rules: final report

Work branch: `v5/feat-ai-review-command-rules`. Final SHA: **`ad7d9d6b`**.

Commits on top of `uam/v5.2-sync-main`:

| Commit | What it does |
|---|---|
| `00eb2149` | Adds the rule matcher and its tests |
| `ee72ebdf` | Wires rules into the permission flow: persistence, bridge handlers, native tests and docs |
| `b515e2ea` | Adds the Settings page and UI tests |
| `119fdfa9` | Fixes from review round 1 |
| `ad7d9d6b` | Fixes from review round 2 |

No PR, issue or comment was created.

## How AI review works today, and why it is slow

**When it runs.** Under the AI Review level, a review starts for every pending request that:
- is not high risk;
- has no native reviewer (Claude auto mode or Codex `auto_review`);
- does not need a decision that only the provider can ask for;
- fits the Plan ceiling.

That includes commands the literal read filter already admits, such as `pwd` or `cat README.md`.

**How it runs.** Each review starts a fresh provider CLI process (for Claude Code: `claude -p --tools "" --no-session-persistence …`) with a 20-second timeout. Only the current pending request is reviewed, so queued requests wait one review at a time. Nothing is cached.

**What it sends.**
- A fixed 559-byte instruction.
- The request as JSON, up to 16 KiB: title, kind, content, the raw input, the full request params and any Codex payload. A typical `npm test` request is about 1.3 KiB, with the command repeated six times.
- The provider CLI's own system prompt, on top of that.

**Not measured:** real latency and tokens per decision. That needs a live provider.

## The rule design

**Rule fields.** Action (allow, ask or deny), a target and an optional scope:
- **Targets:** shell command by pattern, read-only work, file edits, web access, other tools, or any request.
- **Scope:** optionally one provider, and one workspace given as a full path. A worktree chat also matches rules for its source folder and for the chat's own folder.

**Precedence.** Deny wins over ask, and ask over allow, whatever the list order.

**Where rules run.** After the provider-required and Plan/captured-read ceiling checks, and before any automatic approval or AI review.
- **Deny** rejects the request once.
- **Ask** holds it for the user under every level, YOLO included.
- **Allow** only works under AI Review, and only where UAM's reviewer itself could have approved: same eligibility, not high risk, complete and non-sensitive input, plus a per-category check. Read-only work that names something credential-like (`.env`, `.ssh`, keys) never skips review through a rule.

**Allow patterns** match one plain command from its first word. A command is never allowed if it contains any of these anywhere, quoted or not:
- the characters `; & | < > ( ) $ \` % !`;
- a vertical tab, form feed or no-break space;
- a glob, a leading `~`, a leading assignment, or a path as the program.

This holds for cmd.exe and PowerShell as well as POSIX shells.

**Deny and ask patterns** look everywhere a program can start:
- after `;`, `&&`, `||` and pipes, and inside `$( )`, backticks and process substitutions;
- inside `bash -c`, `cmd /c` (including the glued form `cmd /crm`), PowerShell `-Command`, `eval`, `iex` and `Invoke-Expression` strings;
- inside command-line git aliases (`git -c alias.x=…`);
- after wrappers such as `sudo`, `env`, `xargs`, `timeout` and `find -exec`, where every later word is treated as a possible program.

They also re-read the command with quotes removed and `\` treated as `/`, and compare arguments without case.

**Expansion.** A literal argument matches definitely. An argument the shell expands (a glob, `$E`, `%E%` or a substitution) matches only possibly, when what it could become overlaps the pattern. A possible match holds the request for the user; only a definite match denies.

**Things that always go to the user** whenever any deny or ask command rule applies:
- a brace list (`{a,b}`);
- a program whose name is only known at run time;
- PowerShell `-EncodedCommand` and `Invoke-Command` script blocks;
- a command larger than the analysis limits (64 KiB, 128 words per command, 4,096 words or 512 program positions in total).

**Speed.** Matching is linear in pattern and command size. A 24 KB hostile command took 78 s before round 1 and about 1 ms now. The worst case measured, with 200 rules and a 64 KB command, is about 240 ms.

**Default rules:**
- **Allow:** read-only work.
- **Ask:** deletion (`rm`, `rmdir`, `unlink`, `shred`, `find -delete`, `del`, `erase`, `rd`, `Remove-Item`, `ri`), and the files `.env`, `.env.local`, `.env.production` and `.env.development`.
- **Deny:** force pushes, `.ssh` and AWS credential files, macOS keychain password reads, `git credential` and `gh auth token`.

**Behaviour change for YOLO users (open decision for David).** Ask outranks YOLO, so YOLO chats now ask for deletion commands and for commands whose glob or variable arguments could reach a `.env` file, such as `cat *` or `echo $HOME`. This is recorded in `docs/documentation-review.md`. The interim choice is to keep the defaults; any default can be disabled.

**AI review switch.** "Ask the AI reviewer" (on by default). When it is off, requests no rule decides wait for the user.

**Verdict cache.** Approve or deny verdicts are kept in memory for the current app run. A verdict is reused only when the typed shell command, the tool input (sandbox flags, working directory, timeouts), title, content, reviewer, provider, ceiling and workspace all match; only per-call ids may differ.
- Codex requests and non-command tools are never cached.
- Uncertain or failed reviews are not cached.
- The cache is cleared when rules or the switch are saved, and at shutdown.

A rule that starts to decide a request while its review is running is applied instead of the review's verdict.

**Storage.** Rules are saved as JSON in `settings.txt` and validated again at load: invalid entries are dropped, never broadened. A list that cannot be read falls back to the defaults, with a load warning.

**Settings page.** "Permission rules" under Security:
- the AI review switch;
- the rules list, with an enable switch and labelled Edit and Delete actions on each rule;
- Add rule and Edit through the shared Overlay editor;
- "Test a command", which uses the same native matcher, names a deciding rule's scope, and applies the same credential check as at run time.

## Review rounds

**Round 1:** independent `claude-opus-5-5`, fresh context, given the issue, the diff and the docs. **16 suggested (1 blocker, 9 should-fix, 6 nits); 16 implemented, 2 of them only in part.**

The blocker was the cubic-time matcher. Declined parts:
- **#13 (in part):** the check rejecting uppercase first words in allow patterns. Such rules can never match anything, so they are harmless.
- **#15 (in part):** the bridge-handler tests (strict-validation 400 and rollback after a failed save). They need the CEF handler test setup, and the validation they would cover is already tested directly.

**Round 2:** one fresh-context `claude-opus-5-5` re-check of the round-1 fixes. **13 suggested (3 blockers, 6 should-fix, 4 nits); 13 implemented, 3 of them only in part.**

The blockers were all ways expanded arguments could get past deny rules: an empty-matching glob, nested or multi-word braces, and dynamic arguments. Declined parts:
- **#10:** if the AI Review level changes during a review, the verdict is still dropped without applying a rule. The request stays with the user, which is safe; I only fixed the code comment.
- **#12:** the frontend check is stricter than native for operators inside double quotes. It errs on the safe side, so I kept it.
- **#13 (some tests):** I added the brace, dynamic-argument, empty-glob and cache-field tests. I declined tests for the fingerprint, worktree scope at run time, the allow hold when no one-time option is offered, and a mid-review change to an ask rule, because each needs a large new fixture.

## Tests and what was run

- **Matcher:** `tests/command_rules_tests.cpp` has 14 cases: precedence, globs, quoting, `;`/`&&`/`$()`/backtick and cross-shell injection, wrappers, Windows paths, case, expansion, defaults, the ceiling, scope, validation, and timing. They pass in a standalone build under clang (`-Werror -Wconversion`) and g++ (`-O2 -Werror`).
- **Randomized check:** 200,000 generated commands produced zero allows of a command containing an operator character.
- **Reviewers' probes:** re-run against the final code.
- **Native integration:** `tests/acp_permission_rule_tests.cpp` covers:
  - the read-only default replacing the reviewer;
  - no allow outside AI Review;
  - no allow for high-risk requests;
  - rules running after the Plan ceiling;
  - ask holding requests under YOLO;
  - deny on every level;
  - AI review switched off;
  - cache replay across tool-call ids, and the cache-key fields;
  - no hold when there is no typed command;
  - a rule superseding a review in progress;
  - persistence, defaults and corrupt-load recovery.

  These tests have never been run. Every touched native file and both test files compile under g++ and clang against downloaded nlohmann and CEF headers; CMake refuses Linux, so a full build is needed.
- **Frontend:** `npm ci`; `npx tsc --noEmit -p .` (a near no-op, because the root tsconfig has `files: []`) and `-p tsconfig.app.json`; `npm run build`; `npx vitest run` gives 241 files and 4,227 tests passing.
- **Docs:** `python3 scripts/check_docs.py` passes.

## Not verified

- The native integration and settings tests have not run; they need a macOS or Windows CTest run.
- No live-provider, Windows, SSH or desktop journey testing.
- No measured latency or tokens for the old reviewer.
- Antigravity's headless route gives UAM no callback, so rules cannot act on it.
- OpenCode V2's remembered session grants are answered before UAM sees them.
- Claude UI review and David's approval are still pending.

## Issue preview

**feature(permissions): replace per-call AI review with allow/ask/deny command rules**

Under AI Review, UAM starts an isolated provider CLI reviewer for every eligible request, read-only commands included. Each review has a 20-second timeout and runs one request at a time per chat, with no reuse. Product-owner request: user-set allow and deny command rules, evaluated locally, with AI review only for unmatched calls.

**Desired outcome:**
- allow/ask/deny rules by command pattern, tool, provider and workspace;
- deny wins;
- rules run before AI review and never exceed the provider ceilings or the high-risk refusals;
- safe defaults;
- AI review only for unmatched calls, with a way to turn it off;
- a Settings list with a test field.

**Evidence:** `acp_permission_handlers.cpp`: `TryStartPermissionReview`, `kPermissionReviewTimeoutMs` of 20,000 ms, a reviewer prompt of up to 16 KiB (a typical request about 1.3 KiB). Live latency and token cost not measured.

**Acceptance criteria:**
- [ ] `;`, `&&`, `$()`, backticks and cmd/PowerShell forms cannot get a denied command past an allowed prefix
- [ ] No rule approves above the ceiling
- [ ] The Settings list supports add, edit, remove and test
- [ ] Live-provider and desktop acceptance by David

**Review:** self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code CLI 2.1.330), on behalf of David Taylor.

## PR preview

**feat(permissions): run user allow/ask/deny rules before AI review**

Adds local permission rules, checked before automatic approval and AI review. Deny rejects once, ask holds the request for the user under every level, and allow approves once only where UAM's AI reviewer could have. AI review runs only for unmatched requests and can be switched off. Identical command reviews are reused within a run. Adds a Permission rules page under Security.

**Screens needing Before/After capture from the real app:**
- Settings › Security sidebar (new "Permission rules" item).
- The Permission rules page in light and dark (new page).
- The rule editor dialog, including a validation error.
- A chat tool call showing "Permission rule (deny/ask/allow)".
- Chat defaults › Reviewer rows (unchanged, for context).

**Proof:**
- Frontend: `npm ci`, both tsc checks and `npm run build` pass; `npx vitest run` passes 241 files and 4,227 tests.
- Native matcher: 14 cases pass in standalone clang `-Werror` and g++ builds, plus the 200,000-command randomized check and the reviewers' probes.
- Native integration tests and every changed source file compile under g++ and clang, but were not run, because CMake refuses Linux.
- `check_docs.py` passes.
- Not run: native CTest on macOS or Windows, live providers.

**Review:** 1 independent reviewer (claude-opus-5-5), fresh context, 2 iterations.
- Round 1: 16 suggested, 16 implemented (2 only in part).
- Round 2 (re-check of the fixes): 13 suggested, 13 implemented (3 only in part).

Claude UI review and David's approval are still pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code CLI 2.1.330), on behalf of David Taylor.
