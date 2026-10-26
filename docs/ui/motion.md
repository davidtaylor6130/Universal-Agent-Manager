# Motion

Motion explains state and continuity: panels, menus, list changes, disclosure and value changes use purposeful, calm transitions without blocking input or causing layout jumps. Prefer transform and opacity, with exits faster than entrances. Working/loading state should visibly indicate progress while animation is enabled and settle when work ends; reduced-motion or Off must still communicate that state without requiring movement. Provider fast mode is unrelated to UI motion speed.

Use `--dur-fast` (120 ms), `--dur-base` (180 ms), `--ease` and `--ease-out-soft`; respect `prefers-reduced-motion`. Appearance → Motion selects Default, Fast, Slow or Off through `data-motion` and shared `--dur-*` tokens, persisted as `uam-motion`. New transitions use those tokens rather than fixed timings. Motion Off and the OS reduced-motion preference share one global stop that also settles fixed-duration animations and transitions (spinners and working rings remain visible but still), and the shared `motionAllowed` check used for overlay and dialog exits honours both. Fallbacks that keep a still working picture apply to Motion Off as well as the OS preference. The empty-chat composer moves with a transform-based FLIP transition; reduced motion changes position immediately.

[Appearance](../settings/appearance.md) · [Compaction](../providers/compaction.md).
