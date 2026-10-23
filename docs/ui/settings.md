# Settings design language

Settings use the single approved V5 row language, modelled on MCP Servers and Chat Defaults. The shell supplies a title-only page header; short section labels organise related rows without decorative descriptions or nested cards. Each row has one concept, a concise consequence where needed and a right-aligned control. This page owns presentation; [Settings](../settings/settings.md) and its feature pages own behaviour.

The native iPhone counterpart uses iOS grouped list rows and navigation sheets with large touch targets. Each group retains one heading and each row one concept; values and connection status remain distinct from save success. Phone-only preferences stay local, while controller-owned changes use the shared authenticated protocol and disclose unsupported desktop-only operations.

## Controls and disclosure

Use [settings primitives](primitives.md#settings-primitives), with `Switch hideLabel` inside a SettingRow to avoid duplicate labels, MenuSelect for choices and small secondary/ghost buttons for actions. Neutral switches are the default; reserve `uam-switch--accent` for safety-important choices. Labels sit above wide text inputs. Primary settings stay visible; disclose genuinely optional advanced details. No hard-coded colours, ad-hoc boxes, repeated headings or wrapper cards that only add a heading.

## Copy and lists

Use sentence case and plain consequences. A row description is roughly one short line, about 60 characters, and does not repeat its title; a trivially obvious row can omit it. Merge one- or two-row sections with a related neighbour. Hosts, servers, agents, skills and editors use identity/detail/status rows with labelled icon actions and an Add section action; multi-field add/edit flows use the shared Overlay/Presence modal. MCP summaries omit endpoints, executable paths, arguments and workspace directories, which remain in the editor. Empty states have one muted sentence and the add action.

## State and drafts

Observed connected/running/version/failed state is a status line, separate from saved configuration. Keep current content during refresh and disable only the control being saved. Loading uses SettingsLoading. Save/Discard and unsaved-changes handling follow the feature’s real draft state: unchanged saves stay disabled, validation blocks invalid saves, and failures retain drafts and show useful nearby feedback. Retained choices whose save outcome is unknown must stay visibly marked as unconfirmed drafts that may differ from the running app; navigation or an unrelated refresh does not dismiss that status. Group collapse must retain drafts. When applying a connection change needs a restart, put Restart Universal Agent Manager beside the status. A definitive pairing scan or confirmation advances to a dedicated progress step and an explicit success/failure result, with recovery and completion actions visible above the keyboard and safe area.

## Navigation and motion

The active settings item has its tinted `--accent-dim` background and one straight 3 px vertical accent line at the left edge. The line glides using transform; hover and active backgrounds share the same item box. Reduced motion disables the glide. Sections use the existing fade/rise and disclosure presence patterns, with shared [motion tokens](motion.md), not flashing lists.

## Environment control

Use one SettingsScopeBar above applicable page content, labelled “Applying settings for”, with All environments and configured SSH environments. Pages must not invent their own host selectors. The broader behaviour and current implementation gaps live in [Environment scope](../settings/environment-scope.md); the currently narrower UI must not redefine that agreement. `settingsScopeHostId` is session-only and scope changes clear pending CLI installation. Host saves send `scopeExecutionHostId` only when the backend advertises `hostScopedSettings`; otherwise the supported provider-computer actions remain distinct from global saves.

[Reusable primitives](primitives.md) · [Accessibility](accessibility.md) · [Historical settings choices](../reference/settings-history.md).
