# UI primitives

Shared primitives own control appearance, interaction and accessibility so features do not restyle equivalent controls independently. Use the existing components and their supported props; presentation changes must work across themes and preserve keyboard behaviour. These are approved targets, not claims that the code already meets them.

MenuSelect moves focus into its visible option list when opened, supports arrow-key navigation and returns focus to its trigger when closed. Positioning a portal must not leave keyboard focus behind on the trigger. Callers can opt into matching the trigger width, wrapping long option labels instead of widening the list; the selected trigger value stays compact. A caller can pass `describedBy` so explanatory text, such as why the control is disabled, is announced with the trigger.

Primitives never restyle a raw `<button>`; every repeated element is a shared primitive; primitives compose (dialog -> confirm pair, danger/delete button, import button).

Import from `components/ui` (index.ts):

- Button (variant primary|secondary|ghost|danger, size sm|md|lg, leadingIcon, loading, `asLink` for the inline text-link look)
- IconButton (`tooltip={false}` skips its own Tooltip when the caller already wraps it; role, aria-checked and style pass through for toggles)
- Chip (compact choice button; `selected` exposes aria-pressed, tone neutral|danger, size sm|md, icon; all button attributes and ref pass through)
- Tabs and Tab (role tablist/tab, aria-selected, roving tabindex with arrow, Home and End keys; variant underline|pill; one sliding indicator that stops under reduced motion or Motion Off)
- Tooltip
- Card (elevation 0|1|2, flush)
- SectionHeader (title, description, icon, actions)
- StatusDot (read-only state report only. `success` and `error` render a lucide tick or cross in the tone colour (`.uam-status-box`). Inside Settings pages and their popovers only (`.uam-settings-scope`), it sits in a small rounded square (`--r-sm` radius, `--success-dim` or `--error-dim` fill, 1px border at 35% of the tone, `size + 8` px); elsewhere it stays a bare tick or cross. Never colour alone. Every ad-hoc read-only success or failure icon in Settings uses it. Buttons, copy feedback, close marks and menu selection ticks are not status reports and keep their own icons. Neutral, warning, info and accent tones stay plain dots.)
- MenuSelect
- Notice (tone; one row: tone icon (check, triangle alert or circle alert; X is reserved for the dismiss control and is never a status icon), semibold title with the message inline, optional `actions` and dismiss; `details` is the only collapsible part, a Details disclosure for long logs or output, collapsed by default; no header, divider or chevron otherwise)
- Switch (the only on/off control. Every switch has a neutral track (surface when off, high-contrast ink when on). Inside Settings pages and their popovers (the `.uam-settings-scope` class on the Settings root and on settings-owned portaled popovers such as the Providers MCP servers popover) the thumb carries the state as a green tick (`--success`) when on or a red X (`--error`) when off, sliding with it; the thumb is `--surface`, so both icons keep at least 3:1 in every theme. Outside Settings pages and their popovers, such as the composer menu, companion preferences, the New chat routing dialog and the standalone `/skills` window (even though they reuse settings components), the switch keeps its plain black-and-white thumb with no icons, because settings rules never change non-settings UI. There is no accent, gradient or colour-track variant; disabled keeps the same look at reduced opacity. In Settings, checkboxes used as toggles, segmented On/Off choices and custom toggle buttons are converted to Switch, and a switch or status mark carries no On/Off text beside it. The chat composer keeps its own controls. Multi-select checkboxes in lists and tables are not toggles and stay as checkboxes.)
- ProgressFill (wraps the item doing the work: `label`, `value` 0-100 for a determinate fill, or `indeterminate` (implied when no value) for a sweep; sets role progressbar with aria-valuenow/min/max, or aria-busy when indeterminate. Rows that also hold controls cannot be a progressbar, so they take the `uam-progress-fill` class with `data-indeterminate`, `role="group"` and `aria-busy`. `variant="meter"` draws a standalone bar for a measured quantity with a real limit instead: role meter with `valueText` as aria-valuetext, `tone` neutral|warning|error, an optional `marker` tick on the same scale (cut out of the track by a 2px surface ring), `size` md (6px) or lg (10px, the allowance bar), `segments` for equal notches such as the 7 days of a weekly window, and `projection` (`at`, `label`, `tone` warning|error) for a labelled caret under the track that flips its label left past about 60%; the caret is decorative, so say the same in `valueText`. `active={false}` keeps an empty track with no role, tick, notches or caret, and a missing value sweeps inside the track. Styling lives in `index.css`; see [visual style](visual-style.md#quantities).)
- Disclosure (replaces native `<details>`: `summary`, `children`, `defaultOpen` or controlled `open`/`onOpenChange`; the panel slides open from its summary and closes back into it.)
- Reveal (wraps any conditional content: `open`, `as` div or span; enters from slightly below and leaves back the same way, keeping its last children on screen through the exit. Use it instead of a bare `cond && …`.)
- Swap (keyed cross-slide for content replaced in place, such as list and detail panes or Form/JSON views: outgoing slides out left while incoming slides in from the right.)
- UsageGauge (`usedPercent`, `size`, `decorative`; the composer allowance half-dial, described below)
- Overlay
- ConfirmDialog (optional cancel label, middle secondary action, `dismissible={false}` for a choice that Escape or the backdrop must not answer, and `onDismiss` when the cancel button is itself a choice and Escape or the backdrop must only close; its message is the dialog's accessible description via `aria-describedby`, and `returnFocusTo` names the row to refocus on close when the opening control, such as a closed menu item, is gone)
- MenuItem (icon, label, detail with `detailLayout` inline|stacked, `trailing` pill, shortcut, `selected` for a checkable menuitemradio, `checked` for a menuitemcheckbox, danger, disabled with `disabledReason` or `nativeDisabled`; ref and attribute passthrough; menus, context menus and the command palette)
- SegmentedControl (options with value, label and icon, optional per-option `disabled`, `tooltip`, `ariaLabel` and `iconOnly`; one sliding highlight that stops under reduced motion or Motion Off)
- RowButton (full-width list row; reset, hover and focus only, so row classes keep their look)

Icons: lucide-react, 14–16px, stroke default. A Notice, or a message row with its own dismiss control, marks an error with a circle alert, so the X there only ever means dismiss; read-only state reports keep StatusDot's cross. A Notice may omit dismissal while it represents an unresolved state that the user cannot dismiss into success; its title remains visible on the bar. Ordinary notices retain their dismissal control.

UsageGauge (`usedPercent`, `size`, `decorative`) is the shared half-dial usage meter: drawn like a line icon, an arc in three equal segments separated by small gaps. Only the segment holding the value takes its semantic colour (`--success` 0–33% used, `--meter-warning` 33–66%, `--error` 66–100%); the other segments and the needle use the surrounding icon colour (`currentColor`), so it matches neighbouring icons. The needle sits at the clamped used percentage that rotates on `--dur-base`/`--ease-out-soft` and therefore settles immediately under reduced motion or Motion Off. Its default 20px width is a deliberate exception to the 14–16px icon size: the half-dial is about 12px tall, giving roughly a 14px icon's footprint; arc and needle use the 2px icon stroke. A standalone known value, including 0%, exposes `role="meter"` with `aria-valuenow` and `aria-valuetext`; an unknown or non-numeric value shows a decorative neutral `currentColor` arc without a needle or meter value. Inside a button, whose children are presentational, pass `decorative` so the gauge is `aria-hidden` and the button's accessible name carries the readable value (a deliberate deviation from the meter-semantics rule in [Accessibility](accessibility.md), which the composer allowance control uses). Forced-colours mode draws the arc in `CanvasText` and the current segment and needle in `Highlight`.

## Settings primitives

Import from `components/settings/SettingsPrimitives`:

- `SettingsSection`: section label and section actions; existing optional description support is available, but the settings agreement omits decorative section descriptions.
- `SettingRow`: title and concise consequence on the left, control on the right; observed state uses its status line and expanded details appear below. The row links its description and status to the control (or its radio/button group) with `aria-describedby`.
- `SaveButton`: the settings save action; while `saving` it shows the pending label, sets `aria-busy` and announces the label in a polite live region.
- `SettingsField`: label above a wide input for paths, multiline text or multi-field forms.
- `SettingsLoading`: the shared loading skeleton.
- Text inputs use the themed `uam-field` class (theme-token background, text and border), which dims and shows a not-allowed cursor while disabled.
- Legacy `SectionCard` already uses the shared language; prefer the row and field primitives when extending it.

[Settings layout](settings.md) owns how these are assembled. [Source](../../UI-V2/src/components/settings/SettingsPrimitives.tsx).

Close, dismiss and remove controls use the lucide `X` icon directly inside the button. They are neutral at rest and turn red (`--error-dim` background, error-tinted icon) on hover and keyboard focus; one shared rule in `index.css` applies this to every such button, so features never colour close buttons themselves.
