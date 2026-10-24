import { useEffect, useState } from 'react'
import { retainNotificationTimes, type NotificationTimeSource } from '../utils/notificationTime'

/** Shell-owned first observations survive closing the panel, but not source removal or app restart. */
export function useNotificationTimes(sources: readonly NotificationTimeSource[]): ReadonlyMap<string, number> {
  const key = JSON.stringify(sources)
  const [snapshot, setSnapshot] = useState(() => ({ key, times: retainNotificationTimes(new Map(), sources, Date.now()) }))
  // Reconcile before rendering children so new notices have a time on their first panel render.
  if (snapshot.key !== key) {
    const next = { key, times: retainNotificationTimes(snapshot.times, sources, Date.now()) }
    setSnapshot(next)
    return next.times
  }
  return snapshot.times
}

/** One local clock for the open, nonempty panel; no provider polling or live announcements. */
export function useNotificationClock(enabled: boolean): number {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!enabled) return
    const tick = () => setNow(Date.now())
    const visible = () => { if (!document.hidden) tick() }
    tick()
    const timer = window.setInterval(tick, 30_000)
    window.addEventListener('focus', tick)
    document.addEventListener('visibilitychange', visible)
    return () => {
      window.clearInterval(timer)
      window.removeEventListener('focus', tick)
      document.removeEventListener('visibilitychange', visible)
    }
  }, [enabled])
  return now
}
