# Settings design language

Approved by David in the V5 sweep (2026-10-12). Every settings page must use ONE language, modelled on the MCP Servers page David liked. Reference implementation: the Chat Defaults page (`selectedSection === 'defaults'` in UI-V2/src/components/settings/SettingsModal.tsx) .

Primitives (import from './SettingsPrimitives'):
- `<SettingsSection title description actions flush>`: uppercase tracked section label + one-line muted description + right-aligned section actions, then a bordered card. Title in sentence case in code ("AI permission reviewer"); CSS uppercases it.
- `<SettingRow title description control icon status htmlFor>`: title (13px medium) + muted description on the left, control on the right (Switch, MenuSelect/renderDefaultsMenu, small Button, IconButton). `status` = muted live state line (what is happening). Extra children render under the row (expanded detail, inline error).
- `<SettingsField label hint htmlFor>`: label ABOVE a wide input (paths, text areas, JSON, multi-field forms). Use inside a SettingsSection card.
- `<SettingsLoading rows>`: animated skeleton. Use for any "Loading…" text in settings.
- Legacy `SectionCard` already renders in this language; prefer converting its content into SettingRow/SettingsField.

Rules:
1. One concept per row: toggles → `<Switch hideLabel label=… />` as the row control, never a Switch with its own label inside a row (no double labels). Selects → MenuSelect/existing renderDefaultsMenu as control. Buttons → small secondary/ghost on the right.
2. Switches are NEUTRAL by default (on = ink). Only safety/important switches use `className="uam-switch--accent"` (e.g. YOLO-type, Computer Use master switch). Never decorative orange.
3. No nested cards, no custom boxes, no hard-coded colours, no ad-hoc headings. Page header comes from the shell (don't add another title).
4. Status vs configuration: live state (connected, running, installed version, failed) is a `status` line or StatusDot text, never a control.
5. Disclosure/collapsible blocks only when content is genuinely optional/advanced ("Advanced" at the bottom). Primary settings are always visible.
6. Copy: plain language, describe the consequence, sentence case. Every row has a description unless trivially obvious.
7. Lists (hosts, servers, agents, skills, editors): each item is a SettingRow (icon + name + mono detail in description + status) with icon-only actions in control; "Add …" is a section action button; add/edit flows that are multi-field open in a modal (Overlay + Presence, like the import dialog), not inline.
8. Empty states: one muted sentence + the add action.
9. Motion: sections fade/rise in (built into primitives); expand/collapse uses the existing presence/grid-rows pattern; no flashing of whole lists on refresh (keep previous data while refreshing; disable only the control being saved).
   - Settings nav: the active page shows a selection dash, a 3px accent bar at the item's left edge (no tinted pill). It glides between items with transform; reduced motion disables the glide.
   - Appearance > Motion: Default, Fast, Slow or Off scales the `--dur-*` tokens through `data-motion` on the root (persisted in localStorage `uam-motion`). New motion-driven CSS should use the tokens, not fixed durations, so the setting applies.
10. Keep every aria-label/role that tests query; when a test asserts exact old copy you intentionally changed, update that assertion minimally.
11. Environment scope bar: the one way to choose a computer in Settings. `SettingsScopeBar` sits above the page content on Overview, provider pages, Chat Defaults and MCP Servers: "Applying settings for" + a MenuSelect of "All environments" (default) and each SSH execution host. Pages do not have their own host pickers. The scope is session-only (`settingsScopeHostId` in the UI store) and clears any pending CLI install when it changes. Per-computer actions on provider pages (CLI version install/refresh, provider MCP status and toggles, model refresh) target the selected host, or this computer for "All environments"; with "All environments" the CLI section shows "Choose a computer above to manage its CLI version." Saves of Chat Defaults and MCP server lists send `scopeExecutionHostId` (via `withSettingsScope`) only when the backend sets `settings.hostScopedSettings: true` and a host is selected; then the global AI permission reviewer is hidden too. Without that flag the selector is enabled only on provider pages (for the per-computer actions) and disabled with the tooltip "Per-computer settings need an update to UAM" elsewhere. Pages local to this computer (Appearance, Typography, Motion, Version Control, Editors, Shell Actions, Computer Use, Central Setup, Memory) do not show the bar.
