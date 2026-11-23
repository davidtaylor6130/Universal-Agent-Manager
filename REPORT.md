# UAM V5.2 de-slop H: UI store dedups

The independent review is done and the work is pushed.

**Work branch:** `v5/refactor-ui-store-dedup`

**Final SHA:** `9df459695ceb0875d5554bfc6a65641002f07360`

The branch is based on `6f235eb9`. `origin/uam/v5.2-sync-main` has since moved on to `c252f9a9`, and the branch has not been rebased.

| Commit | Summary |
|---|---|
| `01f48640` | refactor(ui-store): share one ACP binding placeholder in sessionsSlice |
| `ee162e66` | refactor(ui-store): map native goals in one place |
| `50d91119` | refactor(ui-store): reuse a shared record guard in three validators |
| `9df45969` | refactor(ui-store): pin array rejection in the usage and allowance parsers (review follow-up, tests only) |

**Lines changed:** production code +141 / −275 across 7 files. Test files +171.

## Where the work differs from the audit

- **Item 3:** the three local guards reject arrays, but the exported `isRecord` accepts them. Using `isRecord` directly would let `[]` through where an object is required. `sanitizers.ts` now exports `isPlainRecord` (`isRecord` plus the array check) next to `isRecord`, and the three validators use it.
- **Item 1:** the copies also differ in `running` and `threadId`. The prompt-error copy uses `'default'` and `''` for mode and model instead of the chat's own values, and three copies have no `providerId`, `protocolKind` or `threadId` at all. Each call site passes its own values and only the fields it had before.
- **Item 2:** a full load still leaves a chat with no goals out of `goalsByChatId`, and a patch still writes `[]`. A comment at each site says so.

## Review rounds

**Round 1 (independent `claude-opus-5-5`, fresh context): 3 suggested, 1 implemented.** It found no blockers and no should-fix issues. It found no compile errors, and there is no native code in this diff.

- **Nit 2, implemented:** array-rejection tests for `parseUsageHistory` and `parseProviderAllowances`. They fail if `isPlainRecord` is swapped back to plain `isRecord`.
- **Nit 1, declined:** the browser-only send now writes some keys of the stored binding in a different order. The values are identical and nothing depends on key order.
- **Nit 3, declined:** renaming `acpBindingPlaceholder.test.ts` is cosmetic. It sits beside `permissionResolution.test.ts`, which is named the same way.

The fix added tests and changed no source code, so a second re-check was not run.

## Tests on `9df45969`

- **`npx vitest run`:** 240 files, 4225 tests passed.
- **`tsc --noEmit -p tsconfig.app.json`:** passed. The production code is unchanged since the earlier runs of `-p tsconfig.node.json`, `npm run build` and `python3 scripts/check_docs.py`, which all passed.
- **`npx tsc --noEmit -p .`:** checks no files, because the root tsconfig has `files: []`.

## Screens needing Before/After capture

None. Nothing visible changed.

## Issue preview

**Title:** `refactor(ui-store): remove duplicated ACP binding, goal mapping and record-guard logic`

```markdown
The UI-V2 store repeats the same logic in several places, so a change to one copy can silently miss the others. Found by a read-only audit of `uam/v5.2-sync-main`.

**Desired outcome:** each piece of logic exists once and behaviour stays the same.

- `store/slices/sessionsSlice.ts` writes the same ~30-field idle `AcpBinding` by hand in six local actions. The copies differ in `sessionId`, `running`, `lifecycleState`, `providerId`, `protocolKind` and `threadId`, and one uses a neutral mode and model.
- `store/useAppStore.ts` maps native goals field by field in both `deserializeState` and `applyStatePatch`, and `CppGoal` in `store/cpp/types.ts` repeats `Goal`'s field list. A full load leaves `goalsByChatId[id]` unset for a chat with no goals, while a patch writes `[]`. That difference must stay.
- `services/providerAllowances.ts`, `services/usageHistory.ts` and `store/memorySettingsWrites.ts` each define their own record guard. These also reject arrays, which the exported `isRecord` in `store/cpp/sanitizers.ts` does not.

**Environment:** UI-V2 (React/TS) on `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:** I checked each audit item against the source. Behaviour is unchanged, so there is nothing to reproduce.

**Acceptance criteria:**

- [ ] One placeholder builder, and each call site keeps its exact values and fields
- [ ] One goal mapper, `CppGoal` derived from `Goal`, and the full-load vs patch difference kept
- [ ] One shared guard that still rejects arrays
- [ ] Frontend suite, type check, build and docs check pass

**Review:** Self-triage only; no independent review.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

## PR preview

**Title:** `refactor(ui-store): remove duplicated store logic`

```markdown
Removes three kinds of duplicated logic from the UI-V2 store. Behaviour is unchanged.

- `createAcpBindingPlaceholder(state, sessionId, overrides)` replaces six hand-written idle `AcpBinding` objects in `sessionsSlice.ts`. Each caller passes its own `sessionId`, `running` and `lifecycleState`, plus provider fields only where it set them before. The prompt-error path keeps its neutral `'default'` mode and empty model.
- `mapCppGoal(chatId, cppGoal)` replaces the two goal mappings in `useAppStore.ts`, and `CppGoal` is derived from `Goal`. A full load still leaves a chat with no goals out of `goalsByChatId`; a patch still writes `[]`.
- The three local record guards reject arrays and the exported `isRecord` does not. `sanitizers.ts` now exports `isPlainRecord` (`isRecord` plus the array check), and the three validators use it.

No documentation change: behaviour is unchanged.

**Proof:** Run on `9df45969` (Linux cloud container):
- `npx vitest run`: 240 files, 4225 tests passed, including 11 new tests.
- The new tests pin each placeholder key for key, the 25 goal fields with both goals behaviours, and array rejection in all three validators. The placeholder and goal tests pass on the base code. The array tests fail when `isPlainRecord` is reduced to `isRecord`.
- `tsc --noEmit -p tsconfig.app.json` and `-p tsconfig.node.json` passed (the root `-p .` checks no files). `npm run build` and `python3 scripts/check_docs.py` passed on the production code, which is unchanged since.
- Not run: the full suite on the base commit, native/CTest (no native change) and a visual check (no visible change).

**Review:** 1 independent reviewer, 1 iteration. Independent claude-opus-5-5: 3 suggested, 1 implemented.

- Added array-rejection tests for the usage and allowance parsers.
- Declined a key-order note on the browser-only send binding (values identical; nothing depends on order).
- Declined a test-file rename (cosmetic).

David's approval is pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```
