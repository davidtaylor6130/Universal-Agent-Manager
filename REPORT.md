# Report: v5/fix-native-settings-small

The review loop is finished. The final head of `v5/fix-native-settings-small` is **`cf0bdba8`**, pushed and based on `uam/v5.2-sync-main` at `6f235eb9`. No PRs, issues or comments were created.

## Commits

| SHA | Change |
|---|---|
| `abe98f41` | fix(computer-use): save allowlist edits while one chat's cleanup is pending |
| `711c6c0a` | fix(skills): keep listing the Skills library past an unreadable subfolder |
| `f8df1041` | fix(remote-hosts): keep a host's custom icon across helper reinstalls |
| `33865904` | fix(settings): address review of the allowlist, Skills and host icon fixes |
| `cf0bdba8` | fix(skills): report a library subfolder that can be listed but not entered |

## Review rounds

**Round 1 (fresh-context Opus reviewer, full branch): 7 suggested, 6 implemented.** No blockers. It found no compile errors in the parts it could build, and each original test fails on the old code.

- **Implemented, both should-fixes:**
  - The Skills list reply now carries `skippedFolderCount`. The missing on-screen notice is recorded as a known gap in `docs/documentation-review.md`.
  - A new handler-level reinstall test runs a real install against a fake ssh and checks the host keeps its icon and options.
- **Implemented, nits:**
  - The walk uses each entry's own type, so links and Windows junctions are never entered.
  - The library folder itself must be enterable, not just listable.
  - A folder deleted during the walk is ignored rather than reported.
  - Two doc wording fixes.
- **Partly taken:** the reviewer suggested failing the walk on errors other than permission denied. Only deleted folders are ignored, because failing would bring back "one bad folder breaks the library".
- **Declined:** a failed or cancelled reinstall restores the old host record, so an icon changed during the install is lost. That bug predates this branch and changes rollback behaviour, so it belongs in its own change.

**Round 2 (fresh-context Opus re-check of the fixes): 1 suggested, 1 implemented.**

- A subfolder that can be listed but not entered hid its skills without being counted as skipped. It is now skipped and counted, and the test covers it.

Neither reviewer could compile the CEF handler tests; both checked them by reading against the existing fixture. After each Skills change the stand-alone Skills test was re-run with file-permission privileges dropped: it passes, and the docs check passes.

## Screens needing Before/After capture

1. **Settings → Computer Use:** an allowlist edit while one chat's cleanup is pending (failure notice → saved).
2. **Skills library with an unreadable subfolder:** empty or error → remaining skills listed.
3. **Settings → Remote hosts:** a host's icon after Update helper (default → custom icon kept).

## Issue previews

### `bug(computer-use): allowlist edits refused while one chat's cleanup is pending`

With Computer Use on, any change to the app allowlist fails after one chat's cleanup has stalled, including changes that tighten it.

**Reproduction:** Turn Computer Use off for one chat so its cleanup stays pending, keep it on for all chats, then add or remove an allowlisted app.
**Expected:** The allowlist saves.
**Actual:** "Computer Use cannot be re-enabled until its previous control grants are cleared."

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; native, all platforms.

**Evidence:** Settings sends only `allowlistEnabled` and `allowedApplications`. In `src/cef/chat_config_handlers.cpp` (~2290), the missing `computerUseGloballyEnabled` falls back to the saved On value, so the re-enable check runs on every allowlist save.
**Reproduction status:** Found by reading the code; not reproduced in the app.

**Acceptance criteria:**

- [ ] Allowlist saves while On succeed when one chat's cleanup is pending
- [ ] Turning Computer Use on from Off still waits for cleanup

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

### `bug(skills): one unreadable subfolder empties the whole Skills library`

One subfolder that UAM can't open makes the whole Skills library show no skills.

**Reproduction:** Put a folder without read permission inside the library folder, then open the library.
**Expected:** The other skills list.
**Actual:** An empty list with "Failed to enumerate Markdown Store files."

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; macOS and Linux file permissions.

**Evidence:** In `src/app/markdown_store_service.cpp`, the walks at ~517 and ~283 don't pass `skip_permission_denied`, and any error returns an empty list (~532-535). The import scanner at ~367 already passes the flag.
**Reproduction status:** Reproduced in a stand-alone program against the base revision, with file-permission privileges dropped: empty list plus the error.

**Acceptance criteria:**

- [ ] Readable skills still list past an unreadable subfolder
- [ ] Skipped folders are reported once, not as a failure

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

### `bug(remote-hosts): helper reinstall or update resets the host's custom icon`

Reinstalling or updating a host's helper replaces the host's custom icon with the default one.

**Reproduction:** Set a custom icon on an SSH host, then run Update helper or reinstall it from Settings.
**Expected:** The icon stays.
**Actual:** The icon resets to the default.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:** In `src/cef/remote_host_handlers.cpp`, ~220-229 copies the other options to the new host record but not `custom_icon`, and ~377 then replaces the saved record with it (`*existing = host`).
**Reproduction status:** Found by reading the code; not reproduced in the app.

**Acceptance criteria:**

- [ ] Reinstall and update keep the custom icon and all other user-owned host options
- [ ] A new host field can't be silently left out of the carry-over

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**`fix(settings): keep allowlist saves, Skills listing and host icons working`**
Base `uam/v5.2-sync-main` ← head `v5/fix-native-settings-small` (`cf0bdba8`)

> Three small native fixes:
> - **Computer Use:** allowlist edits while On are no longer refused when one chat's cleanup is pending. Only turning the feature on from Off is still refused until cleanup finishes.
> - **Skills library:** it lists readable skills past subfolders it can't open or enter, and reports them once in a log line. The list reply also carries `skippedFolderCount`. A library folder that can't be opened or entered still fails. The on-screen notice is a recorded known gap.
> - **Remote hosts:** reinstalling or updating a host's helper keeps every user-owned option, including the custom icon, through one tested helper (`CarryUserOwnedHostFields`).
>
> The docs pages for each area are updated in the same commits.
>
> Closes the three issues above once they are filed.
>
> **Visual evidence:** pending. Before/After needed for:
> 1. Settings → Computer Use: an allowlist edit while one chat's cleanup is pending (failure notice → saved).
> 2. Skills library with an unreadable subfolder (empty or error → remaining skills listed).
> 3. Settings → Remote hosts: a host's icon after Update helper (default → custom icon kept).
>
> **Proof:**
> - `python3 scripts/check_docs.py` passed on every commit.
> - **Skills library:** the fixed and base code were each built into a stand-alone test program with GCC 13.3, which has the new test's checks. It ran as root with file-permission privileges dropped.
>   - The base code returned an empty list with an error.
>   - The fix passed: locked and listable-only subfolders were skipped and reported, the other skills listed, and a new skill could be created.
>   - A library folder with no permissions, or read-only, still failed. A mutation without the new folder-access check made the read-only case pass silently.
>   - `markdown_store_service.cpp` compiles with no new warnings under GCC 13.3 and Clang 18.
> - **Host icon:** the new test's code ran against the real header and passed on GCC 13.3 and Clang 18. Adding a field to `ExecutionHost` made it fail to compile. Removing the icon line from the helper made it fail.
> - **Not run:**
>   - The connected handler program with the Computer Use and reinstall cases (CEF, macOS CI only).
>   - `uam_core_tests` and the full native and CTest gates (the desktop build refuses Linux).
>   - `uam_runner` (the network policy blocked the SQLite download at configure).
> - No UI code changed, so no frontend checks ran.
>
> **Review:** 1 independent reviewer, 2 iterations. Claude review of the UI-visible changes and David's approval are still pending.
>
> - Independent claude-opus-5-5, round 1: 7 suggested, 6 implemented. The 7th, a pre-existing rollback that can drop an icon changed during a failed or cancelled install, was declined as out of scope for a separate change. One nit was taken in part: folders removed during the walk are ignored, but other open errors still skip the folder rather than fail the walk.
> - Independent claude-opus-5-5, round 2 (re-check of the fixes): 1 suggested, 1 implemented.
>
> **Created with:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
