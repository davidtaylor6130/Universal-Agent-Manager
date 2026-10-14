import { useEffect, useState } from 'react'

/** Wake when the countdown becomes visible, then tick only until its deadline. */
export function useCountdownClock(enabled: boolean, startsAt: number | undefined, deadline: number | undefined): number {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!enabled || !startsAt || !deadline) return
    let timer: ReturnType<typeof setTimeout> | undefined
    const tick = () => {
      clearTimeout(timer)
      const current = Date.now()
      setNow(current)
      if (current >= deadline) return
      const delay = current < startsAt ? startsAt - current : Math.min(1000, deadline - current)
      timer = setTimeout(tick, Math.min(delay, 2_147_483_647))
    }
    tick()
    const visible = () => { if (!document.hidden) tick() }
    window.addEventListener('focus', tick)
    document.addEventListener('visibilitychange', visible)
    return () => {
      clearTimeout(timer)
      window.removeEventListener('focus', tick)
      document.removeEventListener('visibilitychange', visible)
    }
  }, [enabled, startsAt, deadline])
  return now
}
