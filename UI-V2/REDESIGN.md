# UI Redesign — Design System Spec (frozen)

Dashboard-style redesign. This is the single source of truth for every component
change. Do not deviate.

## Hard rules (apply to every file)
1. **Presentation only.** Never change logic, handlers, store calls, props, IPC,
   or behavior. Only markup + styling. If a change would alter behavior, stop.
2. **Compose the primitives** in `src/components/ui/`: `Button`, `IconButton`,
   `Tooltip`, `Card`, `SectionHeader`, `StatusDot`. Import from `../ui` (adjust depth).
3. **Style with Tailwind utilities + the primitives.** Do NOT edit `src/index.css`
   or `tailwind.config.js` — the token layer is frozen. Use the token-backed
   utilities below.
4. **No emoji.** Replace every emoji with a `lucide-react` icon.
5. **Tooltips on everything** interactive that is icon-only or terminal-technical.
   `IconButton` already tooltips via its required `label`. For other icon-only
   controls wrap with `<Tooltip label="...">`.
6. **No new dependencies.** Only `lucide-react` and `@radix-ui/react-tooltip` (already in).
7. **Keep tests green.** Update assertions that reference changed text/markup, but
   never weaken a behavioral test. Run `npm --prefix UI-V2 run build` and
   `npm --prefix UI-V2 run test` before declaring done.

## Tokens → Tailwind utilities
- **Type:** `text-xs`(11) `text-sm`(12) `text-base`(13) `text-md`(14) `text-lg`(16)
  `text-xl`(20) `text-2xl`(24). Weights `font-normal/medium/semibold`.
  UI text = sans (default). Code/CLI/ids = `font-mono`.
- **Color:** `text-app-text` / `text-app-text-2` (secondary) / `text-app-text-3` (meta).
  Surfaces `bg-app-surface` / `bg-app-surface-up` / `bg-app-surface-high`.
  Borders `border-app-border` / `border-app-border-bright`.
  Accent `text-app-accent` / `bg-app-accent-dim`.
  Semantic `app-success|warning|error|info` (+ `-dim` fills).
- **Radius:** `rounded-sm/md/lg/full`.
- **Elevation:** `shadow-elev-1` (card) `shadow-elev-2` (popover) `shadow-elev-3` (modal).
- **Spacing:** default Tailwind 4px scale (`p-2`=8, `gap-3`=12, etc). Use the rhythm 4/8/12/16/24.
- **Motion:** `transition duration-fast ease-app` (or `duration-base`). Keep <200ms, never decorative.

## Visual hierarchy
- Primary heading: `text-md font-semibold text-app-text`.
- Section label: `text-xs font-medium uppercase tracking-wide text-app-text-3`.
- Body: `text-base text-app-text`. Meta/timestamps: `text-xs text-app-text-3`.
- One primary action per surface (`Button variant="primary"`); everything else
  secondary/ghost.

## Icons (lucide-react)
- Import named: `import { Plus, Search, Settings, ... } from 'lucide-react'`.
- Default size 16 (`size={16}`), 14 in dense rows, stroke inherits `currentColor`.
- Common mappings: new/add→`Plus`, search→`Search`, settings→`Settings2`,
  folder→`Folder`/`FolderOpen`, chat→`MessageSquare`, send→`ArrowUp`/`SendHorizontal`,
  stop→`Square`, delete→`Trash2`, pin→`Pin`, edit→`Pencil`, copy→`Copy`,
  close→`X`, expand→`ChevronDown`/`ChevronRight`, terminal→`SquareTerminal`,
  git→`GitBranch`, memory→`Brain`, goal→`Target`, theme→`Sun`/`Moon`,
  attach→`Paperclip`, model→`Cpu`, warning→`TriangleAlert`, success→`Check`,
  info→`Info`, error→`CircleAlert`, running→`Loader2` (spin).

## Progressive disclosure
- Show essentials at rest; hide advanced/metadata behind expanders, popovers, hover.
- Timestamps, provider metadata, secondary actions → reveal on hover or in a
  `...` overflow menu, not always-on.
- Big modals (Settings) → grouped sections, collapsed by default where sensible.

## Feedback & states (every interactive element)
hover, `focus-visible` ring (primitives already have it), active, disabled,
loading (spinner), empty state, error state. Icon-only busy → `Loader2` spin.

## Reference implementation
Match `src/components/layout/` + `src/components/sidebar/` once redesigned — they
are the exemplar for spacing, hierarchy, icon usage, and tooltips.
