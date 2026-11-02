# Visual style

Use shared theme tokens for colour, type, radius and spacing, and keep density calm. These are approved targets, not claims that the code already meets them.

## Colour

- Nine built-in themes — Focus, OG, Light, System, Midnight, Paper, Dusk, Aurora, Contrast (labelled High Contrast) — plus custom themes, each with its own accent.
- One accent per theme, used sparingly: the single primary action, the selected/active tint (`--accent-dim`) and the focus ring (`--focus-ring`). Never decorative.
- Text and icons on an accent fill use `--on-accent`: the theme's own colour reaching at least 4.5:1 on its accent, falling back to black or white only when neither reaches it. Custom themes use their background or text, whichever reads better, falling back to black or white only when neither reaches it.
- Surfaces: `--bg` < `--surface` < `--surface-up` < `--surface-high`. Borders: `--border` and `--border-bright` (hairline, 1px).
- Text: `--text`, `--text-2` secondary, `--text-3` muted/meta.
- Status only: `--success`, `--success-dim`, `--warning`, `--warning-dim`, `--error`, `--error-dim`, `--info`, `--info-dim`.
- Warning and error as text use `--warning-text` and `--error-text`: the status colour mixed toward `--text` (55% and 85%), defined once in `:root` so every built-in and custom theme derives its own and the text reaches at least 4.5:1 on its surface. Fills, carets and dots keep `--warning` and `--error`; light-theme yellow alone is about 1.9:1 on white.
- Token categories for usage split bars, legends and tooltips: `--token-input`, `--token-cache-read`, `--token-cache-write`, `--token-output`, `--token-reasoning`; each stays a distinct hue clear of provider, status and accent colours, and is never reused for anything else. Light and dark themes each define calm, low-chroma values that reach at least 4.4:1 on their surfaces and differ in lightness so neighbouring categories stay apart under red-green colour blindness.

## Type

- Base 13px; UI text uses only `--fs-*` tokens, never raw pixel font sizes.
- Scale: `--fs-xs` 11, `--fs-sm` 12, `--fs-base` 13, `--fs-md` 14, `--fs-lg` 16, `--fs-xl` 20, `--fs-2xl` 24.
- Weights 400/500/600. Weight 700 is allowed only for tool-call views and elements styled like them; the `--font-mono` token is reserved for code, CLI output and identifiers (paths, IDs, SSH aliases, model ids).
- Small uppercase tracked labels (11px, letter-spacing ~0.08em, `--text-3`) for group headings.

## Radius and spacing

- Radius and spacing tokens only: `--r-sm` 5, `--r-md` 8, `--r-lg` 12, `--r-full` 999; spacing `--sp-1` through `--sp-8` at a 4px rhythm; soft elevation `--elev-1` to `--elev-3`. No raw values.

## Status

- Success/failure use the same colour and tick/cross icon everywhere, drawn by `StatusDot`; never colour alone. Only Settings boxes the mark in a rounded square (tinted fill and border); chat, sidebar, tool calls and other views keep the bare tick or cross.
- On/off is always the `Switch` control: green track (`--success`) when on, red track (`--error`) when off, white thumb. Never a black or text-coloured track, an accent or gradient variant, or a checkbox or segmented On/Off standing in for it.
- On/off states must stay visibly distinct, not only by switch position.
- Use `StatusDot` only where state matters. Custom themes let users choose semantic status colours (error/stop/cancel, success, warning) so colour-blind users can adjust them.

## Density

- Dense but not crowded; heavy progressive disclosure, showing at a glance only what matters.
- Rows are one line; a second line only when content truly needs it.
- Settings provider overview: one row per provider, grouped, readable at a glance.
- The automatic chat-import notice is one line (provider, location, chat name); a settings import list at two lines is the anti-pattern; consistent padding and spacing scale with the content.

## Quantities

Quantities such as allowances, token usage, spend and counts versus capacity use bars, meters, sparklines or segmented bars where useful, with exact numbers as secondary text. Use semantic thresholds where the value has a real limit (for example over 80% warning and over 95% error); do not invent capacity for unknown values.

Progress on work is never a thin line or bar at the bottom of a rounded box. The row or item doing the work is the progress bar: its background fills left to right in `var(--accent-dim)` by percentage when one is known, or sweeps when none is known. Text stays readable on top and the fill follows the item's border radius. Under reduced motion or Motion Off the sweep stops and a static partial fill (or no fill, when there is no percentage) is shown. Context fill, a measured quantity with a real limit, uses the same row fill, switching to the warning or error tint at its thresholds. Account allowance windows and spend limits are the exception: each is a standalone bar meter (the ProgressFill meter variant) with a neutral fill and a tick at the share of the window's time passed, so use can be read against time; the fill turns warning when the window is projected to run out before its reset and error from 95% ([Allowances](../usage/allowances.md)). Allowance bars are the 10px meter size; weekly windows add seven day notches cut in the surface colour, and an ahead window adds a run-out caret under the track in the bar's tone, labelled with the time and how long before the reset. The meter fill grows from the left when it appears and settles at once under reduced motion or Motion Off.

Usage Cost and Tokens bars use the gapped split bar (token types on Tokens, cost by provider on Cost): 2px gaps between segments, rounded ends, growing in from the left as one piece, with a width that shares one scale across rows. Their shared chart uses gridlines on rounded ticks (1, 2, 2.5 or 5 × 10ⁿ, about a quarter of headroom, at most three intervals), a total above each column for ranges up to two weeks, sparse x labels on long ranges and the open bucket labelled so far.

## Model picker

Fixed-height model picker rows keep the name and identity above compact neutral Context and Output badges with token units. Rows up to 440px use Ctx/Out and tok labels so descriptions remain readable at the default 360px picker width; descriptions hide only at 340px and below, with full labels and exact capacities in accessible row text. Enabled rows disclose descriptions and visible exact capacities through the shared Tooltip, with capacity text announced through the row rather than repeated in its tooltip description; disabled rows use their full accessible label and native hover label without also opening the shared Tooltip; unknown limits render nothing, the **Model token limits** display setting can hide all badges, and these capacity badges do not imply consumed usage or physical parameter size. [Models](../providers/models.md) owns their data and scope.

## Usage

Usage charts include reported charges and estimates; incomplete pricing must disclose excluded unpriced tokens and label the Known cost. [Allowances](../usage/allowances.md) retain unknown values and provider-reported periods rather than inventing a limit.

[Primitives](primitives.md) · [Accessibility](accessibility.md).

Icon grids (provider filters, icon pickers) split items evenly across rows: between 3 and 6 columns, choosing the count that leaves rows as equal as possible (6 items → one row of 6; 7 → 4 + 3), never a lone icon on its own row when a more even split fits. Menu rows keep their label left-aligned; a check, when present, sits in the leading icon slot.

Countdowns (for example the transient alert timer) follow the same rule: the item's own background drains, never a separate line or bar.
