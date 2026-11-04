# UAM design language

UAM is a dense but not crowded professional desktop tool: content leads, chrome stays quiet, and interaction is fast, keyboard-accessible and progressively disclosed. Shared UI rules apply across features rather than following the source tree; primitives own controls and presentation, so features never restyle equivalents. Motion is physical and logical: elements enter from an origin and leave back toward it, selection indicators slide to the clicked item, and nothing pops or vanishes; reduced motion and Motion Off convey state without movement. Chat status stays in the transcript; every app-wide notice, updates included, goes through one notification system.

The authorized [native phone design](../phone-native.md) uses native iOS sheets, grouped settings rows, floating navigation and a growing keyboard-aware composer, with UAM semantic colours and readable Dynamic Type. Desktop type sizes and corner radii do not constrain phone touch controls. The phone preserves UAM’s organisation: Pinned Chats stays flat, only All Chats uses collections and workspaces, and Active exposes Snoozed Chats rather than settled history. Saved branch selection belongs at its source message with previous/next controls rather than pinned dropdowns or a global Related chats button. The computer selector uses compact chrome while exposing full connection names in its menu and accessibility value. Ordinary tool summaries retain nonempty command/title labels with full details on demand. Native disclosure and branch navigation controls retain at least 44-point touch targets. This deliberate mobile design does not change desktop layouts; source/build verification does not establish visual or user acceptance.

- [Visual style](visual-style.md): colour, typography, spacing and quantitative displays.
- [Primitives](primitives.md): shared components and settings building blocks.
- [Interaction](interaction.md): common layouts, disclosure, context chips and focus.
- [Accessibility](accessibility.md): keyboard, labels, states and reduced motion.
- [Motion](motion.md): purposeful transitions and working indicators.
- [Notifications](notifications.md): chat status versus app-wide notices.
- [Settings](settings.md): the shared settings presentation contract.
- [Review](review.md): verification and scope of UI work.

Outgoing messages remain in stable transcript positions through pending or failed admission, with accessible explanations and native-authorized recovery; message navigation reserves a local padded gutter and shared transient notices sit bottom-right inside the chat area, clear of the notification/update rail. These follow the [sending](../chat/sending.md), [content navigation](../chat/tools-and-content.md) and [notification](notifications.md) agreements and retain pending verification and acceptance.

Update the owning UI rule in the same commit as an authorized deliberate design change. A documented target does not establish current implementation; [recorded decisions](../architecture/decisions.md) retain pending notification and undo work. Feature-specific behaviour lives under [Chat](../chat/chat.md), [Sidebar](../sidebar/sidebar.md) and [Settings](../settings/settings.md).

[Interface recovery](interface-recovery.md) owns failed-panel and reload behaviour.

- [Usage and limits](../usage/allowances.md).
