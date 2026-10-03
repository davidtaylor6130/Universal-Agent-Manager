import { useEffect, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'

/** Uses the backend's shared grace/deadline across Chat and CLI views. */
export function RuntimeTimeout({ chatId }: { chatId: string }) {
  const binding = useAppStore(useShallow((state) => {
    const cli = state.cliBindingBySessionId[chatId]
    const runtime = cli?.running ? cli : state.acpBindingBySessionId[chatId]
    return { running: Boolean(runtime?.idleShutdownAtMs && runtime.running), idleCountdownStartsAtMs: runtime?.idleCountdownStartsAtMs, idleShutdownAtMs: runtime?.idleShutdownAtMs }
  }))
  const [now, setNow] = useState(Date.now)
  useEffect(() => {
    if (!binding?.idleShutdownAtMs) return
    setNow(Date.now())
    const timer = setInterval(() => setNow(Date.now()), 1000)
    return () => clearInterval(timer)
  }, [binding?.idleCountdownStartsAtMs, binding?.idleShutdownAtMs])
  if (!binding?.running || !binding.idleCountdownStartsAtMs || !binding.idleShutdownAtMs || now < binding.idleCountdownStartsAtMs) return null
  const seconds = Math.max(0, Math.ceil((binding.idleShutdownAtMs - now) / 1000))
  return <span className="shrink-0 text-xs tabular-nums" title="Activity restarts the one-minute grace period" aria-label="Runtime idle timeout">
    Stops in {Math.floor(seconds / 60)}:{String(seconds % 60).padStart(2, '0')}
  </span>
}
