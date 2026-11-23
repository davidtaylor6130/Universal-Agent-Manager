# MCP server configuration fixes: report

**Work branch:** `v5/fix-mcp-config-env-names-paths` (from `origin/uam/v5.2-sync-main`)
**Final SHA:** `13a6d2e90de17fd232990089df9fc00272b2b3a6`

| Commit | Change |
|---|---|
| `1cdef7f7` | fix(codex): keep MCP server env values in each server's own env table (bug 1) |
| `f33e4f8f` | fix(codex): pass MCP server names as bare -c keys (bug 2) |
| `686200b5` | fix(mcp): validate server paths against the executing host's platform (bug 3) |
| `a2caa580` | fix(settings): drop only invalid MCP server entries at load (bug 4) |
| `cc93bc4c` | fix(mcp): keep Codex MCP env values out of logs and keep the load notice (review round 1) |
| `4390b3bc` | fix(mcp): refuse cmd.exe-length Codex launches and restore the load notice (review round 2) |
| `6168ad66` | fix(codex): forward MCP env values by name, never on the command line (integration security finding) |
| `a2e1ac36` | test(codex): place the Codex MCP launch tests apart from the signing-key branch's |
| `13a6d2e9` | fix(codex): refuse loader and shell variables for Codex MCP servers (review round 3) |

All four findings were confirmed in the code. Bug 2's quoting also affected header names (`env_http_headers."X-Api-Key"`), and that is fixed too. Bug 4 was reproduced: with one valid and one relative-path server saved, the old code loaded 0 servers and the fixed code loads 1.

**Bug 1 route (changed after the integration security finding):** Codex receives each stdio server's env values from its own environment, forwarded to the server by name with `mcp_servers.<key>.env_vars`. No MCP env value, and not the UAM tool signing key (`UAM_CONTROL_REQUEST_KEY`), appears on Codex's command line. Codex can't rename a forwarded variable, so a launch is refused when:
- two servers set one name to different values;
- a user's server sets a name that would redirect or inject code into Codex or its shells (`PATH`, `PATHEXT`, `HOME`, `USERPROFILE`, `SHELL`, `COMSPEC`, `ENV`, `BASH_ENV`, `NODE_OPTIONS`, `NODE_PATH`, or a `CODEX_`, `UAM_`, `LD_` or `DYLD_` name);
- a user's server sets a variable UAM's environment already has to another value.

**Residual:** the agent's shell commands inherit the forwarded values. This is recorded as a known gap in `docs/documentation-review.md`.

**Merge with `v5/fix-uam-control-hidden-tools-secrets`:** my edits to the `uam-tools.md` paragraph and the Codex checklist row were reverted to the base text, so that branch's wording ("Codex CLI View forwards it by variable name") wins. My Codex notes are on separate lines, and my tests sit apart from theirs. A trial merge has no conflicts, and `check_docs.py` passes on the merged tree. Its `UamControlSigningKeyStaysOut…` test expects the key in Codex's environment and absent from argv, which the code does.

## Review rounds

Each round used a fresh-context Opus sub-agent (requested with the `opus` alias; exact model ID not recorded) that was given the issue, the diff and the project docs.

- **Round 1: 7 suggested (1 blocker, 3 should-fix, 3 nits); 6 implemented in full, 1 in part.**
  - Implemented in full: secrets in the ACP launch log, the command-line length limit, the overwritten load notice and its wording, the native path rule in `Apply`, TOML escaping for `developer_instructions`, and the `uam-tools.md` wording.
  - In part: finding 2 (secrets on argv). I added redaction of Windows launch-failure text and updated the docs. I declined moving structured Codex launches to per-thread app-server config: that change reaches the SSH helper protocol and the ACP lifecycle, which can't be compiled or verified here. It is documented as a follow-up.
- **Round 2 (re-check of the round-1 fixes): 6 suggested (0 blockers, 1 should-fix, 5 nits); 5 implemented in full, 1 in part.**
  - Implemented in full: the cmd.exe 8,191-character limit for `.cmd` shims, the duplicated Windows error text, a single length check that names the largest server, the POSIX comment and docs, and restoring the notice after the bundled-skills message.
  - In part: the redaction tests. I added the portable `Redacted` test but not the `_WIN32`-only `BuildWindowsLaunchCommand` case, because it can't be compiled or run here.
- **Integration security finding (env table on the command line):** implemented. Env values now go into Codex's environment and are forwarded by name; a test asserts no MCP env value appears in Codex argv or its launch log. The finding also superseded round 1's deferred per-thread-config item.
- **Round 3 (fresh-context re-check of the named-variable change): 7 suggested (1 blocker, 1 should-fix, 5 nits); 7 implemented.**
  - The blocker: `NODE_OPTIONS`, `LD_*` and `DYLD_*` were accepted when the host lacked them; they are now reserved.
  - The should-fix: clearer guidance when a variable already exists.
  - The nits: the reserved check now runs before the clash check; stale doc and comment wording; a `const char*` loop; test-unique variable names, with a cleared `NODE_OPTIONS`.

## Issue previews (not filed)

No duplicates were found when searching open and closed issues for MCP env, Codex quoting, SSH paths and cleared servers. Proposed labels already exist in the repository.

### 1. `bug(codex): MCP server env values collide and change Codex's own environment`
Labels: `bug`, `provider`

UAM put each Codex stdio MCP server's environment values into Codex's process environment and forwarded them by name with `env_vars`. Two servers using the same variable name collide, and a server's `PATH` or `NODE_OPTIONS` changes Codex itself and every shell command it runs.

**Reproduction:** Configure two stdio MCP servers that both set `API_KEY` with different values, then start a Codex chat.
**Expected:** Each server receives its own value; Codex and its shell commands are unaffected.
**Actual:** One value wins for both servers, and the values are visible in Codex's environment.
**Environment:** `uam/v5.2-sync-main`, `src/common/provider/provider_setup.h` (Codex branch of `Apply`).
**Evidence:** Code inspection: `SetEnvironment(environment, variable, ...)` followed by `-c ...env_vars=[names]`. Codex's `env_vars` entries cannot rename a variable.
**Reproduction status:** Confirmed from code, and the generated arguments were checked in a scratch harness; not run against a live Codex.

**Acceptance criteria:**

- [ ] No MCP env value appears on Codex's command line; values are forwarded by name.
- [ ] Two servers setting one name to different values, or a user server setting a loader, shell, Codex or UAM variable or changing an existing one, are refused with a message that names only the server and variable.
- [ ] Launch logs and error text do not contain the values.

**Review:** Self-triaged; no independent report review.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

### 2. `bug(codex): MCP server names keep their quote marks in Codex`
Labels: `bug`, `provider`

UAM wrote overrides as `-c mcp_servers."<name>".url=...`. Codex splits `-c` key paths on dots and keeps quote marks, so it registers the server literally as `"uam-probe"`. Header names in `env_http_headers."<header>"` keep their quotes the same way.

**Reproduction:** Configure an MCP server named `uam-probe` and start Codex.
**Expected:** Codex lists `uam-probe`.
**Actual:** Codex lists `"uam-probe"`, quotes included.
**Environment:** `uam/v5.2-sync-main`, `provider_setup.h`; behaviour reported with codex-cli 0.171.0.
**Evidence:** User-reported local check with codex-cli 0.171.0. Codex source parses the `-c` value as TOML and falls back to a raw string.
**Reproduction status:** The report is user-observed; the generated arguments were checked in a scratch harness here.

**Acceptance criteria:**

- [ ] Names matching `[A-Za-z0-9_-]+` are bare keys.
- [ ] Any other name maps to a stable slug plus digest.
- [ ] Two names that would share one key are refused with a rename message.
- [ ] A unit test checks the generated arguments.

**Review:** Self-triaged; no independent report review.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

### 3. `bug(settings): Windows UAM rejects POSIX MCP paths for SSH hosts`
Labels: `bug`, `reliability`

MCP command and workspace paths had to be absolute on the computer running UAM, with only a Windows-form fallback. A Windows UAM therefore refuses `/usr/bin/node` for a Linux SSH host.

**Reproduction:** On Windows, add a stdio MCP server for a Linux SSH host with the command `/usr/bin/node`.
**Expected:** It saves.
**Actual:** "needs an absolute executable path".
**Environment:** `uam/v5.2-sync-main`, `src/common/config/mcp_server_config.h`.
**Evidence:** Code inspection.
**Reproduction status:** Confirmed from code. A Windows run was not attempted (no Windows machine here).

**Acceptance criteria:**

- [ ] Paths are checked against the executing host's recorded platform.
- [ ] Every-computer entries and hosts with no recorded platform accept either absolute form.
- [ ] Host-scoped entries use their host's platform.

**Review:** Self-triaged; no independent report review.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

### 4. `bug(settings): one invalid MCP entry clears every saved MCP server`
Labels: `bug`, `reliability`

Settings load validated the MCP list all-or-nothing and cleared it on any failure; a host-scoped list was discarded the same way. The next save persisted the empty list.

**Reproduction:** Save one valid MCP server and one with a relative command path, then restart.
**Expected:** The valid server loads, and a notice explains the invalid one.
**Actual:** No MCP servers load, and the next save removes them all.
**Environment:** `uam/v5.2-sync-main`, `src/common/config/settings_store.cpp`.
**Evidence:** Reproduced with a harness against `settings_store.cpp`: 0 servers loaded before the fix, 1 after.
**Reproduction status:** Reproduced (native settings store, Linux).

**Acceptance criteria:**

- [ ] Only invalid entries are dropped.
- [ ] Reasons are logged without paths or values.
- [ ] One application notice is shown, and it survives later startup messages.

**Review:** Self-triaged; no independent report review.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `fix(mcp): isolate Codex MCP env and names, check paths per host, keep valid servers`

Fixes four MCP configuration defects:

1. Codex stdio servers receive their env values from Codex's environment by name (`env_vars`), never on the command line. Launches are refused when values for one name differ, or when a user server would set a loader, shell, Codex or UAM variable or change one Codex already has.
2. Codex server keys are bare when the name allows, and a stable slug plus digest otherwise; names that would collide are refused. Header and env tables are inline TOML.
3. MCP paths are validated against the executing host's platform, so a Windows UAM accepts POSIX paths for Linux SSH hosts.
4. Settings load drops only invalid MCP entries, logs why without paths or values, and shows one application notice that later startup messages no longer overwrite.

Review follow-ups:

- Env tables are redacted from launch diagnostics and Windows launch-failure text.
- A Codex launch that would exceed the OS command limit (including cmd.exe's 8,191 for `.cmd` shims) is refused by server name.

Boundary: the agent's shell commands inherit forwarded MCP env values (known gap). Merges cleanly with `v5/fix-uam-control-hidden-tools-secrets`.

Related: issue previews 1–4 above (not filed).

**Visual evidence:** pending; see the screens below.

**Proof:**

- `cmake -S . -B Builds/runner -DUAM_RUNNER_ONLY=ON -DCMAKE_BUILD_TYPE=Release` and `cmake --build Builds/runner --target uam_runner` passed after each commit. The SQLite amalgamation came from the npm `better-sqlite3` package via `FETCHCONTENT_SOURCE_DIR_UAM_SQLITE`, because the proxy blocks sqlite.org.
- `python3 scripts/check_docs.py` passed: 203 pages, 1509 links, 6 providers × 32 capabilities.
- A scratch harness (`-Wall`) mirrored `CodexMcpEnvironmentValuesNeverReachItsCommandLine` against the real `provider_setup::Apply` for Codex, with the `uam-control` session server. It covered:
  - values in the environment and `env_vars` names in argv;
  - no value or signing key in argv or the redacted log;
  - the clash refusal and the reserved refusals (`PATH`, `NODE_OPTIONS`, `LD_PRELOAD`, `CODEX_HOME`, `UAM_CONTROL_REQUEST_KEY`);
  - the existing-variable refusal, same-value acceptance, the `zeta` ordering, and the oversize refusal.
- A separate harness ran bare and derived keys, header inline tables and the key-collision refusal. Python `tomllib` parsed the emitted TOML values.
- A trial merge with `origin/v5/fix-uam-control-hidden-tools-secrets` had no conflicts, and `check_docs.py` passed on the merged tree.
- In-repo test bodies were extracted and run against a stand-in harness with `settings_store.cpp`; all pass:
  - `LaunchArgvRedactionHidesPrivateConfiguration`
  - `McpServerPathsFollowTheirExecutionHostsPlatform`
  - `SettingsLoadDropsOnlyInvalidMcpServerEntries`
  - the existing `McpServerConfigurationIsWorkspaceScopedCapabilityGatedAndSecretSafe`
- The bug 4 regression loaded 0 servers before the fix and 1 after.
- `persistence_coordinator.cpp` and `acp_launch.cpp` passed a syntax-only compile with a CEF pointer stub.
- Not run:
  - `uam_core_tests` and the full native and CTest gates (they need CEF, and the desktop build refuses Linux).
  - `CodexMcpEnvironmentValuesNeverReachItsCommandLine`, `CodexMcpOverridesUseBareServerKeysAndInlineTables`, `SettingsLoadShowsOneNoticeForDroppedMcpServerEntries`, and the other branch's merged `UamControlSigningKeyStaysOut…` test.
  - Compilation of `application.cpp`, `settings_handlers.cpp` and the Windows platform files.
  - Any live Codex or provider CLI run.
  - UI-V2 checks (no frontend files changed).

**Review:** 1 independent reviewer, 3 iterations, plus one integration security finding (implemented). Independent Opus sub-agent (`opus` alias, exact model ID not recorded): 20 suggested, 18 implemented, 2 implemented in part.

- Round 1: 7 suggested, 6 implemented, 1 in part. The per-thread transport was deferred, then superseded by the named-variable route.
- Round 2: 6 suggested, 5 implemented, 1 in part. The Windows-only launch test can't be compiled here.
- Round 3: 7 suggested, 7 implemented.

Required Claude review of any UI is not applicable (no React changes). David's approval is pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

1. **Application status notification at startup:** a saved settings file with one valid and one invalid MCP server. Before: no servers and no notice. After: the valid server, plus "Could not load 1 saved MCP server entry because it is invalid; other servers are unaffected."
2. **Settings > MCP Servers on Windows:** saving a stdio server for a Linux SSH host with the command `/usr/bin/node`. Before: the "needs an absolute executable path" error. After: it saves.
3. **Codex CLI View server list:** a server named `uam-probe`, and one named with a space or dot. Before: names shown in quotes. After: bare names, or the derived `slug-<digest>` key.
