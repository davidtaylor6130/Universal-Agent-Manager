# Copilot SDK protocol 4: refused with a clear message

Copilot CLI 1.0.113 and later report SDK protocol 4. Protocol 4 is **still refused**. All Copilot protocol checks now go through one shared gate, and the refusal is one plain line: "This Copilot CLI uses protocol 4; UAM supports 3."

Supporting protocol 4 safely needs a port and a live check on the Mac first. Permission enforcement is unchanged.

**Work branch:** `v5/fix-copilot-protocol-4`, final SHA `d2fce895d51ad8e253dc4e63d84ef2b1cfd63ad5`. It has two commits on top of `origin/uam/v5.2-sync-main`: `de49eb20` and `d2fce895`.

## What protocol 4 changes

**Sources:**
- npm `@github/copilot-sdk@1.0.34`. This is the latest SDK on npm; it is protocol 3, paired with CLI 1.0.112.
- npm `@github/copilot-linux-x64@1.0.113` and `@1.0.114`. These were unpacked and not run. The executable's bundled `copilot.tgz` holds `cli-main.js`, `copilot-sdk/` (`SDK_PROTOCOL_VERSION = 4`, minimum 4) and `runtime.node`.

**First version:** CLI 1.0.112 reports protocol 3. CLI 1.0.113 (published 2026-11-17) is the first stable version that reports 4. Neither the CLI changelog nor npm has a protocol-4 SDK note.

**Unchanged:** method names; ping, status, quota, send (including immediate mode), abort, model, compact, permission replies, plan exit, managed settings and tool requests; and the permission, user-input and plan-exit request data.

**Changed:**
- **Setup options.** `session.create` and `session.resume` take grouped options. For example, `requestPermission` becomes `permissions.request`, `requestUserInput` becomes `interaction.requestUserInput`, and `mcpServers` becomes `mcp.servers`.
- **Server reads only grouped keys.** This was verified in 1.0.114: `enablePermissionCallback:t.permissions?.request??!1`.
- **Native runtime error.** Both 1.0.113 and 1.0.114 contain "Session option '…' has moved to '…' in protocol 4; update the SDK or request payload".
- **Events.** `parentToolCallId` is dropped from message, usage and tool events, and `tool.execution_partial_result` is replaced by `tool.shell_output`.

**Why it stays refused:** UAM sends the old flat keys. A protocol-4 server would either reject the setup or turn the permission callback off. If it turned the callback off, UAM's approval ceilings would stop applying.

The full write-up is in `docs/reference/copilot-sdk-protocol-4-assessment.md`.

## What changed

- **Shared gate.** `src/common/provider/copilot_sdk_protocol.h` holds the verified list, currently {3}. Four sites use it:
  - ping, for chat start and model refresh, locally and over SSH;
  - saved-image Retry `status.get`;
  - managed continuation `status.get`;
  - the account reader's `connect`, a fourth site found during the work.
- **Refusal timing.** Chat and model refresh are refused right after `ping`, before `models.list` or `session.create`/`resume` is sent. Saved-image Retry and continuation are refused before image delivery or continuation input.
- **Messages.** "This Copilot CLI uses protocol N; UAM supports 3." or "This Copilot CLI did not report a protocol; UAM supports 3." Neither suggests updating the CLI.
- **Docs.** The Copilot provider page has a new SDK protocol section, plus updated `structured-chat`, `models` and `usage` rows and the managed-continuation text. Also updated: `docs/providers/attachments.md`, the reference index, the source map and the new assessment page.

## Tests

**Run on Linux.** The desktop build refuses Linux and CEF downloads are blocked by the network policy, so I used a minimal stand-in harness and stub CEF headers:
- All 14 cases in `tests/account_allowance_fetcher_tests.cpp` passed, including the new `AccountAllowanceCopilotRefusesUnverifiedSdkProtocolBeforeAccountRead`.
- `CopilotSdkProtocolGateAdmitsOnlyVerifiedProtocolsWithOnePlainLine` passed, compiled with `-Wall -Wextra -Werror`.
- `python3 scripts/check_docs.py` passed: 204 pages, 6 providers × 32 capabilities.

**Syntax check only.** g++ and clang++ `-fsyntax-only` were run on every changed source and test file. There were no errors outside the CEF stub headers, and the clang output matches the base branch.

**Not run here:**
- `CopilotNativeProtocolFourPingIsRefusedBeforeAnyFurtherWireMessage`, which feeds the exact 1.0.114 ping reply.
- The new `protocol-4` variant in `CopilotSavedOwnedRetryRequiresNativeEvidenceAndRetiresActualTimeout`.
- The new `status-protocol-4` variant in `ManagedInitialCopilotUnsupportedOrIncompleteReadPolicyRemainsTruthfulOneShot`.
- The full `uam_core_tests` and CTest suites.

## Review rounds

- **Round 1** (independent `claude-opus-5-5`, fresh context): 8 suggested (0 blockers, 2 should-fix, 6 nits), 7 implemented.
  - **Declined:** de-duplicating `Count()` with `ReportedSdkProtocol`. `Count()` is a general helper used for many other fields, and this was style only.
  - **Should-fix #2, setup check that could pass for the wrong reason:** the reviewer's alternative was implemented. The setup call that an empty fixture could never satisfy was removed, and the precondition is asserted directly, with the protocol-3 iteration as the positive control.
- **Round 2** (fresh-context `claude-opus-5-5` re-check of the fixes): 1 suggested (an optional nit), 0 implemented.
  - **Declined:** asserting the refusal text in the managed-continuation fixture. The run's error text only surfaces through macOS-only connected-fixture plumbing that cannot be run here. The same refusal path is covered, and the text is asserted by the shared gate unit test and the saved-image fixture.

## Needs a live check on the Mac

- **On this branch:** run the full native suite and the connected handler tests (saved-image and managed-continuation `protocol-4` variants).
- **On this branch:** check that Copilot 1.0.114 shows the one-line refusal in Chat, model refresh, the usage/allowance reader and over SSH.
- **Before porting 4:** do 1.0.113/1.0.114 reject the flat setup keys or quietly accept them?
- **Before porting 4:** what does the server do with tool permission requests when `permissions.request` is missing? This is the key safety question.
- **Before porting 4:** is `tools.filterPrecedence` accepted on create? What does `tool.shell_output` actually contain? Does `agentId` reliably map to the subagent's parent tool call?

## Screens needing Before/After capture

There are no UI code changes. The only visible change is the error text shown for a Copilot chat or model refresh with CLI 1.0.113+:
- **Before:** "Copilot SDK server protocol 3 is required; update the CLI or UAM before retrying."
- **After:** "This Copilot CLI uses protocol 4; UAM supports 3."

Capture both on the Mac in a Copilot chat's error line and in the model picker's refresh error. No capture was taken here.

## Issue preview

**bug(copilot): CLI 1.0.113+ reports SDK protocol 4 and every Copilot chat fails**

Copilot CLI 1.0.113 and later answer `ping` with `protocolVersion: 4`, and UAM requires exactly 3. Every Copilot chat and model refresh fails, locally and over SSH. The error tells users to update the CLI, which cannot help.

**Reproduction:** Install Copilot CLI 1.0.114, then start a Copilot chat or refresh models.
**Expected:** Chat works, or UAM refuses with a clear reason.
**Actual:** "Copilot SDK server protocol 3 is required; update the CLI or UAM before retrying."

**Environment:** `uam/v5.2-sync-main`, Copilot CLI 1.0.114, macOS, local and SSH.

**Evidence:** David reported that 70 of 70 pings returned protocol 4, and that the last successful model list came before the CLI update. The exact-3 checks are in `copilot_native_protocol.cpp` (ping and saved-image), `copilot_managed_read_policy.cpp` and `account_allowance_fetcher.cpp`. The npm packages show protocol 4 first appears in 1.0.113.
**Reproduction status:** Not reproduced live; the code paths and the npm packages were inspected.

**Acceptance criteria:**

- [ ] An unverified protocol is refused before any session setup, in one plain line that names the reported protocol.
- [ ] All Copilot protocol checks share one verified-protocol list.
- [ ] Protocol 4 is supported only after its wire changes are ported and permission routing is verified live.

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

**fix(copilot): refuse unverified SDK protocols with one plain line**

Copilot CLI 1.0.113+ reports SDK protocol 4. UAM's exact-3 checks broke Copilot chat and model refresh and showed a misleading "update the CLI" message.

This PR moves all four Copilot protocol checks into one shared gate (`copilot_sdk_protocol.h`, verified list {3}). The gate refuses before any session setup, prompt, image delivery or continuation input with "This Copilot CLI uses protocol 4; UAM supports 3."

Protocol 4 stays refused. It moves `requestPermission`/`requestUserInput` into grouped setup options that the server reads exclusively, so supporting it needs a port and a live check. The details are in `docs/reference/copilot-sdk-protocol-4-assessment.md`.

**Proof:**
- **Linux, stand-in harness:** all 14 account-reader cases passed, including the new protocol-4 and missing-protocol refusal case. `CopilotSdkProtocolGateAdmitsOnlyVerifiedProtocolsWithOnePlainLine` passed with `-Werror`.
- **Syntax check:** g++ and clang++ `-fsyntax-only` show no errors outside stubbed CEF headers.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Not run:** the ping-fixture case, the saved-image and managed-continuation protocol-4 fixture variants, the full native suite, and a live Copilot 1.0.114 check.

**Review:** 1 independent reviewer, 2 iterations.

- Agent 1: claude-opus-5-5 — round 1 suggested 8, implemented 7. Declined: de-duplicating `Count()`, because it is a general helper and the change is style only.
- Agent 1: claude-opus-5-5 — round 2 re-check suggested 1, implemented 0. Declined: an optional nit to assert the continuation refusal text, which only macOS-only fixture plumbing can show. The same path is covered, and the text is asserted elsewhere.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
