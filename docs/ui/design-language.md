# UAM design language

UAM is a dense, calm, dark-first professional desktop tool: content leads, chrome stays quiet, and interaction is fast, keyboard-accessible and progressively disclosed. Shared UI rules apply across features rather than following the source tree. Preserve the approved V5 visual and interaction language; new behaviour follows its owning feature agreement. The rules below were approved during the V5 UI sweep, while this reorganisation is still awaiting review and user testing.

- [Visual style](visual-style.md): colour, typography, spacing and quantitative displays.
- [Primitives](primitives.md): shared components and settings building blocks.
- [Interaction](interaction.md): common layouts, disclosure, context chips and focus.
- [Accessibility](accessibility.md): keyboard, labels, states and reduced motion.
- [Motion](motion.md): purposeful transitions and working indicators.
- [Notifications](notifications.md): chat status versus app-wide notices.
- [Settings](settings.md): the shared settings presentation contract.
- [Review](review.md): verification and scope of UI work.

Update the owning UI rule in the same commit as an authorized deliberate design change. A documented target does not establish current implementation; [recorded decisions](../architecture/decisions.md) retain pending notification and undo work. Feature-specific behaviour lives under [Chat](../chat/chat.md), [Sidebar](../sidebar/sidebar.md) and [Settings](../settings/settings.md).
