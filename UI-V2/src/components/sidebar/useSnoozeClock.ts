import { useEffect, useState } from 'react'
import type { Session } from '../../types/session'

/** One timer for the nearest wake time; focus catches clock changes and suspended windows. */
export function useSnoozeClock(sessions: Session[]): number {
  const [revision, setRevision] = useState(0)
  useEffect(() => {
    const refresh = () => setRevision((value) => value + 1)
    const now = Date.now()
    const next = sessions.reduce((earliest, session) => {
      const until = session.snoozedUntilMs ?? 0
      return until > now ? Math.min(earliest, until) : earliest
    }, Infinity)
    const timer = Number.isFinite(next) ? window.setTimeout(refresh, Math.min(next - now + 1, 2_147_483_647)) : undefined
    const visible = () => { if (!document.hidden) refresh() }
    window.addEventListener('focus', refresh)
    document.addEventListener('visibilitychange', visible)
    return () => {
      if (timer !== undefined) window.clearTimeout(timer)
      window.removeEventListener('focus', refresh)
      document.removeEventListener('visibilitychange', visible)
    }
  }, [sessions, revision])
  return Date.now()
}
