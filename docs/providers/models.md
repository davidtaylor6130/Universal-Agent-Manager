# Models

Model discovery and selection belong to the chosen provider and execution environment. Show real advertised or explicitly configured models, retain the selected model during refresh, and distinguish cached choices from confirmed availability. A selection must reach the running or next-started session as appropriate; unavailable models produce an actionable error instead of silently selecting another. Provider/model changes must not leak another chat’s selection. In an existing chat’s model picker, Refresh or Retry requests models for the provider and environment being viewed while retaining cached choices, the current selection and the search. Request acceptance does not establish a completed catalogue refresh. Older discovery replies must not replace the selector state of a changed chat or environment; retiring a selector does not cancel a request or authorize retrying input.

[Chat defaults](../settings/chat-defaults.md) · [Provider settings](../settings/providers.md).
