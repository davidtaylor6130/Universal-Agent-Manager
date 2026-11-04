import { useEffect, useRef, useState } from 'react'
import { EXIT_MS, motionAllowed, scaledMs } from '../ui'

/** Rows animate out (collapse + fade over --dur-exit) before `commit` removes them. Instant under reduced motion or Motion Off. */
export function useRowLeave() {
  const [leaving, setLeaving] = useState<ReadonlySet<string>>(() => new Set())
  const timers = useRef(new Set<number>())
  useEffect(() => () => timers.current.forEach((timer) => window.clearTimeout(timer)), [])
  const leave = (ids: string[], commit: () => void) => {
    if (ids.length === 0) return
    if (!motionAllowed()) { commit(); return }
    setLeaving((current) => new Set([...current, ...ids]))
    const timer = window.setTimeout(() => {
      timers.current.delete(timer)
      setLeaving((current) => { const next = new Set(current); ids.forEach((id) => next.delete(id)); return next })
      commit()
    }, scaledMs(EXIT_MS))
    timers.current.add(timer)
  }
  return { leaving, leave }
}
