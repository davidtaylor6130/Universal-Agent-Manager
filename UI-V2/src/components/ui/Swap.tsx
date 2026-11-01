import { useEffect, useRef, useState, type ReactNode } from 'react'
import { EXIT_MS, motionAllowed, scaledMs } from './Overlay'
import { cx } from './cx'
import './reveal.css'

/**
 * Content that swaps by key: the outgoing content is snapshotted and slides out
 * while the incoming content slides in. Both share one grid cell, so no layout jump.
 */
export function Swap({ swapKey, className, children }: { swapKey: string; className?: string; children: ReactNode }) {
  const current = useRef({ key: swapKey, node: children })
  const [outgoing, setOutgoing] = useState<{ node: ReactNode } | null>(null)
  const timer = useRef(0)
  // Runs before the live-node effect below, so it still sees the previous key and content.
  useEffect(() => {
    if (current.current.key === swapKey) return
    if (!motionAllowed()) return
    setOutgoing({ node: current.current.node })
    window.clearTimeout(timer.current)
    timer.current = window.setTimeout(() => setOutgoing(null), scaledMs(EXIT_MS))
  }, [swapKey])
  useEffect(() => { current.current = { key: swapKey, node: children } })
  useEffect(() => () => window.clearTimeout(timer.current), [])
  return <div className={cx('uam-swap', className)}>
    {outgoing && <div className="uam-swap__out" aria-hidden="true">{outgoing.node}</div>}
    <div key={swapKey} className="uam-swap__in">{children}</div>
  </div>
}
