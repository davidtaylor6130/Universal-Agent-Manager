import { useEffect, useLayoutEffect, useRef, useState } from 'react'

/**
 * The current time for a live elapsed clock. While `active`, every render reads the clock, so new timing data never
 * meets a stale time, and a tick re-renders every `intervalMs`. Ticking stops while the document is hidden and the
 * clock catches up as soon as it is visible again. While inactive the last committed value is kept.
 */
export function useNow(active: boolean, intervalMs = 1000): number {
  const last = useRef(0)
  const [, setTick] = useState(0)
  const now = active || last.current === 0 ? Date.now() : last.current
  useLayoutEffect(() => { last.current = now })
  useEffect(() => {
    if (!active) return
    let timer: number | undefined
    const stop = () => {
      if (timer !== undefined) window.clearInterval(timer)
      timer = undefined
    }
    // The render that activated the clock already read the time; only a return to a visible window needs a catch-up.
    const start = (catchUp: boolean) => {
      stop()
      if (document.visibilityState === 'hidden') return
      if (catchUp) setTick((value) => value + 1)
      timer = window.setInterval(() => setTick((value) => value + 1), intervalMs)
    }
    const onVisibilityChange = () => start(true)
    start(false)
    document.addEventListener('visibilitychange', onVisibilityChange)
    return () => {
      stop()
      document.removeEventListener('visibilitychange', onVisibilityChange)
    }
  }, [active, intervalMs])
  return now
}
