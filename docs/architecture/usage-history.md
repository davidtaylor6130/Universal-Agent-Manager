# Usage history

Usage history combines provider-native logs, transcript scans and UAM's own ledger into host-scoped summaries. The UI requests bounded pages and assembles them for display; it can show local, remote or all-machine results.

## Flow and invariants

Collectors and scanners write normalized records to the usage store. `MultiHostHistory` gathers host results; the CEF handler serves a snapshot and continuations. The frontend validates each page's host, time window, snapshot and offset before assembly. Keep a page set on one immutable snapshot, reject stale continuations and preserve the distinction between observed usage and estimated prices.

Large history must remain paged across the bridge. Avoid changing response shapes without updating both native transport and frontend assembly. Price overrides do not rewrite collected token counts.

## Ownership and checks

- Collection, store and aggregation: `src/common/usage/`.
- Multi-host transport: `src/common/usage/multi_host_history.*`, `src/common/usage/history_transport.h`, `src/cef/usage_handlers.cpp`.
- Frontend parser and hook: `UI-V2/src/services/usageHistory.ts`, `UI-V2/src/hooks/useUsageHistory.ts`, `UI-V2/src/components/usage/`.
- Focused checks: `tests/usage_history_service_tests.cpp`, `tests/usage_history_transport_tests.cpp`, `tests/usage_store_tests.cpp`, `UI-V2/src/services/usageHistory.test.tsx`, `UI-V2/src/services/usageHistoryTransport.test.ts`.

Page size and snapshot expiry are transport implementation details in source. No user-visible latency target is recorded; keep paging bounded and avoid loading all records into one bridge message.
