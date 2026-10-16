# Reviewing UI work

A UI change includes user experience, visual design, states and motion, not just CSS. A presentation-only task covers structure, copy, layout, primitives and disclosure; store or bridge changes need authorized logic scope. Small isolated fixes can be made directly, while larger newly discovered work is recorded in the V5 log rather than silently expanding a UI pass. Preserve the existing release review and ownership rules in [AGENTS.md](../../AGENTS.md).

- Check every changed surface visually in dark and light themes (and at least one other built-in theme for colour work), including loading, empty and error states.
- Keep existing tests' intent; when copy or structure changes on purpose, update the assertion minimally rather than deleting it.
- Run `npm --prefix UI-V2 run test` and `npm --prefix UI-V2 run build` (the build type-checks more strictly than `tsc --noEmit`).
- Dialogs rendered through `Overlay` are portalled to `document.body`; tests must query `document.body` for them.

React/frontend changes require recorded Claude review, and David’s specific approval is separate from tests, builds and installation. A documentation-only reorganisation does not establish that the described UI has been visually checked.
