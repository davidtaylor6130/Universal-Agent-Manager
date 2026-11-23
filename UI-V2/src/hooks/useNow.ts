import { useEffect, useRef, useState } from 'react'

/**
 * The current time for a live elapsed clock. While `active`, every render reads the clock, so new timing data never
 * meets a stale time, and a tick re-renders every `intervalMs`. Ticking stops while the document is hidden and the
 * clock catches up as soon as it is visible again. While inactive the last value is kept.
 */
export function useNow(active: boolean, intervalMs = 1000): number {
  const now = useRef(0)
  const [, setTick] = useState(0)
  if (active || now.current === 0) now.current = Date.now()
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
      setTick((value) => value + 1)
      timer = window.setInterval(() => setTick((value) => value + 1), intervalMs)
    }
    start()
    document.addEventListener('visibilitychange', start)
    return () => {
      stop()
      document.removeEventListener('visibilitychange', start)
    }
  }, [active, intervalMs])
  return now.current
}
