export type ElapsedOptions = {
  /** 'under-hour' (default) shows seconds below an hour; 'under-minute' drops them from one minute on, for dense one-line rows. */
  seconds?: 'under-hour' | 'under-minute'
}

/**
 * The one elapsed-time text: `45s`, `1m 5s`, `1h 2m`. Rounds to the nearest whole second, so a live clock and
 * the saved duration of the same work read the same; the smaller unit is kept at zero (`2m 0s`, `1h 0m`).
 */
export function formatElapsed(ms: number, { seconds = 'under-hour' }: ElapsedOptions = {}): string {
  const total = Number.isFinite(ms) ? Math.max(0, Math.round(ms / 1000)) : 0
  if (total < 60) return `${total}s`
  const minutes = Math.floor(total / 60)
  if (minutes < 60) return seconds === 'under-minute' ? `${minutes}m` : `${minutes}m ${total % 60}s`
  return `${Math.floor(minutes / 60)}h ${minutes % 60}m`
}
