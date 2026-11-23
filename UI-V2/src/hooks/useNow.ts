import { useEffect, useState } from 'react'

/**
 * The current time for a live elapsed clock, refreshed every `intervalMs` while `active`. Ticking stops while the
 * document is hidden and the clock catches up as soon as it is visible again.
 */
export function useNow(active: boolean, intervalMs = 1000): number {
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!active) return
    let timer: number | undefined
    const stop = () => {
      if (timer !== undefined) window.clearInterval(timer)
      timer = undefined
    }
    const start = () => {
      stop()
      if (document.visibilityState === 'hidden') return
      setNow(Date.now())
      timer = window.setInterval(() => setNow(Date.now()), intervalMs)
    }
    start()
    document.addEventListener('visibilitychange', start)
    return () => {
      stop()
      document.removeEventListener('visibilitychange', start)
    }
  }, [active, intervalMs])
  return now
}
