# Accessibility

Every control is keyboard reachable with visible focus and an accessible name; icon-only actions need meaningful labels and tooltips. Dialogs and menus must manage focus so actions reach the intended surface and returning preserves the user’s place. Loading, errors, progress and selection must remain understandable to assistive technology and without colour alone. Honour reduced motion without hiding working state, and keep text legible in every built-in theme.

Settings rows expose their consequence description and status line to their control (or control group) through `aria-describedby`; pending saves mark the save button busy and announce the pending label in a polite live region. Text and icons on an accent fill use `--on-accent`, one of the theme's own colours chosen to reach WCAG AA 4.5:1, rather than fixed white. Interactive elements are never nested inside another interactive role, and controls revealed on hover are also revealed while keyboard focus is in their row; workspace headers stay in the Tab order while searching and in the phone view.

Known allowance values, including zero, use numeric meter semantics; unknown tracks are decorative and must not announce a fabricated number. Reset announcements distinguish future, provider-reported past and stale cached values. [Allowance behaviour](../usage/allowances.md) owns the display rules.

Preserve existing roles, labels and test intent when reorganising a surface. An intentional wording change can update a matching assertion, but removing an assertion does not make an inaccessible control acceptable. [Primitives](primitives.md), [motion](motion.md) and [UI review](review.md) apply to each state.
