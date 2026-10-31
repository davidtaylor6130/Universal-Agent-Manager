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
- StatusDot
- MenuSelect
- Notice (tone)
- Switch
- Overlay
- ConfirmDialog (optional cancel label, middle secondary action, and `dismissible={false}` for a choice that Escape or the backdrop must not answer)
- MenuItem (icon, label, detail with `detailLayout` inline|stacked, `trailing` pill, shortcut, `selected` for a checkable menuitemradio, `checked` for a menuitemcheckbox, danger, disabled with `disabledReason` or `nativeDisabled`; ref and attribute passthrough; menus, context menus and the command palette)
- SegmentedControl (options with value, label and icon, optional per-option `disabled`, `tooltip`, `ariaLabel` and `iconOnly`; one sliding highlight that stops under reduced motion or Motion Off)
- RowButton (full-width list row; reset, hover and focus only, so row classes keep their look)

Icons: lucide-react, 14–16px, stroke default. A Notice may omit dismissal while it represents an unresolved state that the user cannot dismiss into success; its heading remains visible when details are collapsed. Ordinary notices retain their dismissal control.

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
