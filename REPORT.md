# AI review → command allow/ask/deny rules: final report

Work branch: `v5/feat-ai-review-command-rules` (PR #1091). Final SHA: **`53f83bd3`**.

Commits on top of `uam/v5.2-sync-main`:

| Commit | What it does |
|---|---|
| `00eb2149` | Adds the rule matcher and its tests |
| `ee72ebdf` | Wires rules into the permission flow: persistence, bridge handlers, native tests and docs |
| `b515e2ea` | Adds the Settings page and UI tests |
| `119fdfa9` | Fixes from review round 1 |
| `ad7d9d6b` | Fixes from review round 2 |
| `905c7f95` | Built-in rule: remote commits always ask (David, 2026-11-23) |
| `53f83bd3` | Fixes from review round 3 (of the remote-commit rule) |

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

## Update: remote commits always ask (David's rule, 2026-11-23)

By default, every non-local permanent committing action now waits for explicit approval under every permission level, YOLO included.

**How it works.** A built-in ask rule runs inside the rule evaluation that precedes the YOLO, Accept Edits and AI Review approval paths. YOLO keeps providers in their default ask mode, so they still send UAM their permission requests. The rule is not stored in the rules list, so neither the editor nor `setPermissionCommandRules` can remove it.

**What it holds:**
- **Git:** `git push` with any options or refspecs (force pushes, remote branch and tag create/delete); command-line aliases; `send-pack`, `svn dcommit`, `p4 submit`, `subtree push` and `lfs push`; `git-*` programs and `hub`; and any Git subcommand UAM does not recognise as local, which may be a configured alias.
- **GitHub CLI:** `gh pr merge` (also with flags before `merge`), `pr close --delete-branch`, `repo sync`, `repo create --push`, `release create`/`delete`, `gh api` calls that change something, and any gh command it does not ship.
- **Subversion:** `svn commit`/`ci` and `import`; copy/move to a repository URL or `^/` path; mkdir/delete on one; prop commands with `--revprop` or on a URL; any unknown svn subcommand; `svnmucc`; `svnrdump load`; and `svnsync`.

It follows chains, wrappers (including `call` and `wsl`), shell strings, `submodule foreach`, `bisect run`, `rebase --exec` and `difftool --extcmd`.

**What it leaves alone.** Local work: `git commit`, local branches and tags, fetch and pull, `svn update`, `svn revert`, and working-copy SVN operations, including copying from a URL into the working copy.

**Interaction with user rules.** A user deny still wins, and a user allow cannot approve these actions.

**Notice.** While such a request waits, the approval card shows: "Pushes, merges and other remote commits always wait for your approval." When UAM cannot read what a command runs, the card says instead that it cannot rule out a push.

**Hidden override.** The settings-file key `permission_remote_commits_without_approval=1` is off by default, is written to the file only when set, appears in neither Settings nor the frontend state, and is included in the agent-run fingerprint. When on, it restores the previous behaviour. The Settings page shows the built-in rule as a locked row, and the tester reports it.

**Side effect.** Because the rule is always on, a command whose program UAM cannot read (`$CC -o x x.c`) now also asks under YOLO. This is documented.

**Not covered (documented; open item for David).** MCP tool calls (for example a GitHub server's merge or push-files actions), provider-native auto modes, sandboxed commands that need no approval, CLI View terminals, and pushes started indirectly (hooks the agent wrote, `core.sshCommand`, `git fetch --upload-pack`, scripts).

**Review round 3:** fresh-context `claude-opus-5-5` review of `905c7f95`. **16 suggested (2 blockers, 10 should-fix, 4 nits); 14 implemented in code, 2 addressed in docs only.**

The blockers were `gh pr merge` slipping through when a flag with a value came before `merge`, and svn letting unknown subcommands through. Handled in docs only:
- **#12, optional part:** holding MCP tools whose names suggest merge or push. Left as a documented limit and an open decision, because MCP tools have no typed command identity to match against.
- **#15:** the locked row keeps the same text when the override is on. Documented; the tester reflects the override.

**Tests and checks for this update:**
- **Matcher:** 16 standalone cases pass under clang `-Werror -Wconversion`. New cases: every listed command asks; the reviewer's forms ask; local commits, `svn revert` and `svn update` do not; the override restores today's behaviour; a user deny still wins; a user allow cannot approve.
- **Native integration:** the listed commands ask under YOLO with no user rules; `git commit`, `svn revert` and `svn update` run; the override and its hidden settings key are tested. These compile under g++ and clang but have not run (CMake refuses Linux).
- **Frontend:** the approval-card notice and locked-row tests pass. The full suite passed on `905c7f95` (242 files, 4,229 tests); round 3 changed no frontend code. Type check and build pass.
- **Docs:** `check_docs.py` passes.

**Docs updated:** the "Remote commits always ask" section of the rules page; the YOLO and settings pages; and `documentation-review.md`, which records the decision and its date.

**Review line for the PR:** "1 independent reviewer (claude-opus-5-5), fresh context, 3 iterations. Round 1: 16 suggested, 16 implemented (2 only in part). Round 2: 13 suggested, 13 implemented (3 only in part). Round 3 (remote-commit rule): 16 suggested, 14 implemented, 2 handled in docs only."

**Additional screens needing Before/After capture:**
- The approval card showing the remote-commit notice under YOLO.
- The locked built-in row at the top of Settings › Permission rules.
