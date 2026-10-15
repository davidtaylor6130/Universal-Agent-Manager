# Visual style

Use shared theme tokens, a restrained accent, compact readable type and a consistent spacing rhythm. Quantities should have a useful visual representation alongside exact values; state must remain understandable without relying only on colour. These details preserve the existing approved design language rather than introducing a new visual system.

Colour — tokens only, never hard-coded hex (10 themes must work: dark, light, focus, midnight, paper, dusk, aurora, contrast...):
- Surfaces: var(--bg) < var(--surface) < var(--surface-up) < var(--surface-high). Borders: var(--border), var(--border-bright) (hairline, 1px).
- Text: var(--text), var(--text-2) secondary, var(--text-3) muted/meta.
- ONE accent var(--accent) (orange), used sparingly: the single primary CTA, active/selected tint (var(--accent-dim)), focus ring. Never decorative.
- Status only: var(--success)/--success-dim, var(--warning)/--warning-dim, var(--error)/--error-dim, var(--info)/--info-dim.
- Token categories (usage split bars, legends, tooltips): var(--token-input), --token-cache-read, --token-cache-write, --token-output, --token-reasoning. Each is a distinct hue kept clear of provider, status and accent colours; never reuse them for anything else.

Type: sans UI text 13px base; scale var(--fs-xs 11) --fs-sm 12 --fs-base 13 --fs-md 14 --fs-lg 16 --fs-xl 20. Weights 400/500/600 only. var(--font-mono) ONLY for code, CLI output, identifiers (paths, IDs, SSH aliases, model ids). Small uppercase tracked labels (11px, letter-spacing ~.08em, --text-3) for group headings.

Spacing: 4px rhythm var(--sp-1..8). Radius var(--r-sm 5) --r-md 8 --r-lg 12. Elevation var(--elev-1..3), soft.

Quantities such as allowances, token usage, spend, progress and counts versus capacity use bars, meters, sparklines or segmented bars where useful, with exact numbers as secondary text. Use semantic thresholds where the value has a real limit (for example, over 80% warning and over 95% error); do not invent capacity for unknown values.

[Primitives](primitives.md) · [Accessibility](accessibility.md).
