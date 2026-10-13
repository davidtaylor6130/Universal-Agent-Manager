# UAM design language

Approved by David during the V5 Claude UI/UX sweep (2026-10-11/12). Every React/UI change in `UI-V2/` should follow this document; settings pages additionally follow `settings.md`. Update this file in the same commit as any deliberate change to the language.

## Scope of a UI pass

- Make working UI look and feel good: user experience first, then visual design, then style.
- Presentation only: JSX structure, copy, layout, CSS, primitives, ordering, disclosure, empty/loading/error states and motion. Store logic, IPC payloads and behaviour change only in an explicitly scoped logic task.
- Small defects (a rename, a one-or-two-line fix) may be fixed in place; anything larger is recorded as Unassigned work in `V5 Improvement Change log.md`.

## Design language
Character: dense, calm, dark-first pro desktop tool (IDE side-panel feel). Content leads, chrome is quiet. Interaction quality bar: T3 Code (fast, keyboard-first, dense but legible, progressive disclosure).

Colour — tokens only, never hard-coded hex (10 themes must work: dark, light, focus, midnight, paper, dusk, aurora, contrast...):
- Surfaces: var(--bg) < var(--surface) < var(--surface-up) < var(--surface-high). Borders: var(--border), var(--border-bright) (hairline, 1px).
- Text: var(--text), var(--text-2) secondary, var(--text-3) muted/meta.
- ONE accent var(--accent) (orange), used sparingly: the single primary CTA, active/selected tint (var(--accent-dim)), focus ring. Never decorative.
- Status only: var(--success)/--success-dim, var(--warning)/--warning-dim, var(--error)/--error-dim, var(--info)/--info-dim.
- Token categories (usage split bars, legends, tooltips): var(--token-input), --token-cache-read, --token-cache-write, --token-output, --token-reasoning. Each is a distinct hue kept clear of provider, status and accent colours; never reuse them for anything else.
Type: sans UI text 13px base; scale var(--fs-xs 11) --fs-sm 12 --fs-base 13 --fs-md 14 --fs-lg 16 --fs-xl 20. Weights 400/500/600 only. var(--font-mono) ONLY for code, CLI output, identifiers (paths, IDs, SSH aliases, model ids). Small uppercase tracked labels (11px, letter-spacing ~.08em, --text-3) for group headings.
Spacing: 4px rhythm var(--sp-1..8). Radius var(--r-sm 5) --r-md 8 --r-lg 12. Elevation var(--elev-1..3), soft.
Animation is IN SCOPE (David classes it as UX): add purposeful motion in your surface — panels/popovers/menus enter-exit (fade + 4–8px translate or scale .98→1), list items appearing/removing, disclosure expand/collapse, value changes (bars growing), state changes (hover/press feedback). Smooth, physical, quick; never blocking input; no layout jank; animate transform/opacity (not width/height where avoidable); exit faster than enter.
Visuals over text: a picture is worth a thousand words. Quantities (allowances, limits, token usage, spend, progress, context usage, counts vs capacity) should be shown as bars/meters/sparklines/segmented bars with the exact number as secondary text — not plain text alone. Use semantic colour thresholds (e.g. >80% warning, >95% error).
Motion: var(--dur-fast 120ms) / --dur-base 180ms, var(--ease) / --ease-out-soft. Respect prefers-reduced-motion. Anything that is loading or working must visibly move (shimmer skeletons, progress, working indicators) so the user can see the app has not frozen; keep that motion calm and purposeful. Animate to explain state, not to decorate, and stop the motion once the work settles.
Focus: box-shadow: var(--focus-ring) on :focus-visible.

Primitives — use them, don't restyle <button> ad hoc. Import from 'components/ui' (index.ts):
Button (variant primary|secondary|ghost|danger, size sm|md|lg, leadingIcon, loading), IconButton, Tooltip, Card (elevation 0|1|2, flush), SectionHeader (title, description, icon, actions), StatusDot, MenuSelect, Notice (tone), Switch, Overlay, ConfirmDialog. Icons: lucide-react, 14–16px, stroke default.

Patterns:
- Settings page: page title + one-line muted subtitle; sections = semibold 13–14px heading + muted one-line description; label above field; actions right-aligned; secondary buttons outlined. Sections separated by spacing/hairline, not heavy boxes inside boxes.
- Lists/rows: identity line (text), detail line (mono, --text-3, for ids/paths), status line (StatusDot + semantic colour); icon-only actions on the right with Tooltip + aria-label.
- Notices: tinted, calm, plain-language consequence; collapsible/dismissible where long.
- Empty new chat: a short heading and the composer centre in the pane; the first send moves the composer down to its normal position (FLIP, transform only; reduced motion jumps).
- Context chips (`components/chat/ContextChip.tsx`): referenced context (attachments, skills) is a 24px pill with a type icon, name and kind label on var(--surface-up). Full path is a Tooltip in mono; files open the preview on click; remove is an icon button on composer chips only. Kinds include file, folder, skill, image and branch (GitBranch icon). Assistant replies chip inline code that is clearly a file path, folder or branch; in prose the chip is vertical-align middle with negative block margin so the line height does not change.

UX rules (enforce):
1. One clear primary action per section; rest secondary/ghost.
2. Status ≠ configuration: show "what is happening" (badges/dots/state text) distinctly from "what you set" (fields/switches).
3. Plain-language labels stating consequence. Remove internal jargon from UI copy ("ownership", "admission", "scope", "revision", "guarded", "durable", "hydrate", "publication"). British/neutral English is fine.
4. Every async surface: designed loading, empty and error states (not blank, not raw error dumps).
5. Fully keyboard reachable, visible focus ring, aria-labels on icon buttons.
6. Technical detail (paths, PIDs, IDs, raw errors) collapsed or secondary mono — never the headline.
7. Density: no oversized cards/padding; align to a grid; consistent heights (controls 26/32px).
8. Don't invent features or data the code does not provide.

## Notifications

- One notifications panel (the bell rail) holds every app-wide message: failures, warnings, completed system actions and available updates.
- One shared transient alert (`TransientAlertSurface` in `AppShell`, bottom-left, `aria-live="polite"`) briefly shows each new message. It auto-dismisses and the message stays in the panel.
- No feature-specific popups or toasts. Features report failures through `reportAppFailure` (`utils/appFailure.ts`).
- No undo offers or undo shortcuts. A change either saves or is reported as failed.

## Verifying UI work

- Check every changed surface visually in dark and light themes (and at least one other built-in theme for colour work), including loading, empty and error states.
- Keep existing tests' intent; when copy or structure changes on purpose, update the assertion minimally rather than deleting it.
- Run `npm --prefix UI-V2 run test` and `npm --prefix UI-V2 run build` (the build type-checks more strictly than `tsc --noEmit`).
- Dialogs rendered through `Overlay` are portalled to `document.body`; tests must query `document.body` for them.
