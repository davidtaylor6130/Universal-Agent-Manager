# Visual style

Use shared theme tokens for colour, type, radius and spacing, and keep density calm. These are approved targets, not claims that the code already meets them.

## Colour

- Nine built-in themes — Focus, OG, Light, System, Midnight, Paper, Dusk, Aurora, Contrast (labelled High Contrast) — plus custom themes, each with its own accent.
- One accent per theme, used sparingly: the single primary action, the selected/active tint (`--accent-dim`) and the focus ring (`--focus-ring`). Never decorative.
- Text and icons on an accent fill use `--on-accent`: the theme's own colour reaching at least 4.5:1 on its accent, falling back to black or white only when neither reaches it. Custom themes use their background or text, whichever reads better, falling back to black or white only when neither reaches it.
- Surfaces: `--bg` < `--surface` < `--surface-up` < `--surface-high`. Borders: `--border` and `--border-bright` (hairline, 1px).
- Text: `--text`, `--text-2` secondary, `--text-3` muted/meta.
- Status only: `--success`, `--success-dim`, `--warning`, `--warning-dim`, `--error`, `--error-dim`, `--info`, `--info-dim`.
- Token categories for usage split bars, legends and tooltips: `--token-input`, `--token-cache-read`, `--token-cache-write`, `--token-output`, `--token-reasoning`; each stays a distinct hue clear of provider, status and accent colours, and is never reused for anything else.

## Type

- Base 13px; UI text uses only `--fs-*` tokens, never raw pixel font sizes.
- Scale: `--fs-xs` 11, `--fs-sm` 12, `--fs-base` 13, `--fs-md` 14, `--fs-lg` 16, `--fs-xl` 20, `--fs-2xl` 24.
- Weights 400/500/600. Weight 700 is allowed only for tool-call views and elements styled like them; the `--font-mono` token is reserved for code, CLI output and identifiers (paths, IDs, SSH aliases, model ids).
- Small uppercase tracked labels (11px, letter-spacing ~0.08em, `--text-3`) for group headings.

## Radius and spacing

- Radius and spacing tokens only: `--r-sm` 5, `--r-md` 8, `--r-lg` 12, `--r-full` 999; spacing `--sp-1` through `--sp-8` at a 4px rhythm; soft elevation `--elev-1` to `--elev-3`. No raw values.

## Status

- Success/failure use the same colour and tick/cross icon everywhere.
- On/off states must stay visibly distinct, not only by switch position.
- Use `StatusDot` only where state matters. Custom themes let users choose semantic status colours (error/stop/cancel, success, warning) so colour-blind users can adjust them.

## Density

- Dense but not crowded; heavy progressive disclosure, showing at a glance only what matters.
- Rows are one line; a second line only when content truly needs it.
- Settings provider overview: one row per provider, grouped, readable at a glance.
- The automatic chat-import notice is one line (provider, location, chat name); a settings import list at two lines is the anti-pattern; consistent padding and spacing scale with the content.

## Quantities

Quantities such as allowances, token usage, spend, progress and counts versus capacity use bars, meters, sparklines or segmented bars where useful, with exact numbers as secondary text. Use semantic thresholds where the value has a real limit (for example over 80% warning and over 95% error); do not invent capacity for unknown values.

## Model picker

Fixed-height model picker rows keep the name and identity above compact neutral Context and Output badges with token units. Rows up to 440px use Ctx/Out and tok labels so descriptions remain readable at the default 360px picker width; descriptions hide only at 340px and below, with full labels and exact capacities in accessible row text. Enabled rows disclose descriptions and visible exact capacities through the shared Tooltip, with capacity text announced through the row rather than repeated in its tooltip description; disabled rows use their full accessible label and native hover label without also opening the shared Tooltip; unknown limits render nothing, the **Model token limits** display setting can hide all badges, and these capacity badges do not imply consumed usage or physical parameter size. [Models](../providers/models.md) owns their data and scope.

## Usage

Usage charts include reported charges and estimates; incomplete pricing must disclose excluded unpriced tokens and label the Known cost. [Allowances](../usage/allowances.md) retain unknown values and provider-reported periods rather than inventing a limit.

[Primitives](primitives.md) · [Accessibility](accessibility.md).
