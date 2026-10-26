# UI primitives

Shared primitives own control appearance, interaction and accessibility so features do not restyle equivalent controls independently. Use the existing components and their supported props; presentation changes must work across themes and preserve keyboard behaviour.

MenuSelect moves focus into its visible option list when opened, supports arrow-key navigation and returns focus to its trigger when closed. Positioning a portal must not leave keyboard focus behind on the trigger. Callers can opt into matching the trigger width, wrapping long option labels instead of widening the list; the selected trigger value stays compact. A caller can pass `describedBy` so explanatory text, such as why the control is disabled, is announced with the trigger.

Primitives — use them, don't restyle <button> ad hoc. Import from 'components/ui' (index.ts):
Button (variant primary|secondary|ghost|danger, size sm|md|lg, leadingIcon, loading), IconButton, Tooltip, Card (elevation 0|1|2, flush), SectionHeader (title, description, icon, actions), StatusDot, MenuSelect, Notice (tone), Switch, Overlay, ConfirmDialog. Icons: lucide-react, 14–16px, stroke default. A Notice may omit dismissal while it represents an unresolved state that the user cannot dismiss into success; its heading remains visible when details are collapsed. Ordinary notices retain their dismissal control.

## Settings primitives

Import from `components/settings/SettingsPrimitives`:

- `SettingsSection`: section label and section actions; existing optional description support is available, but the current settings agreement omits decorative section descriptions.
- `SettingRow`: title and concise consequence on the left, control on the right; observed state uses its status line and expanded details appear below.
- `SettingsField`: label above a wide input for paths, multiline text or multi-field forms.
- `SettingsLoading`: the shared loading skeleton.
- Text inputs use the themed `uam-field` class (theme-token background, text and border), which dims and shows a not-allowed cursor while disabled.
- Legacy `SectionCard` already uses the shared language; prefer the row and field primitives when extending it.

[Settings layout](settings.md) owns how these are assembled. [Source](../../UI-V2/src/components/settings/SettingsPrimitives.tsx).
