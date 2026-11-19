# Custom icons

Chats, workspaces, hosts and collections can have a persistent custom icon: a searchable Lucide symbol, emoji or supported PNG image. An icon changes presentation, not provider, execution host or resource identity; provider/host glyphs remain fallbacks where no custom choice applies. A local workspace without a custom icon shows its project's own website or app PNG icon when one is found in a common location (for example `public/favicon.png`, `app/icon.png` or an Android launcher icon) within the same 256 by 256 pixel, 256 KB limit; it is looked up once per app run, never stored, and a chosen icon always takes priority. Changes and reset/removal target the intended resource and must preserve the existing value if saving fails. Keep controls and icon-only states accessible without relying on the chosen picture as the only identity label.

[Sidebar actions](management.md) · [Appearance](../settings/appearance.md) · [Shared interaction](../ui/interaction.md).
