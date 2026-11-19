import { Timer } from 'lucide-react'
import { useCountdownClock } from '../../hooks/useCountdownClock'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Tooltip } from '../ui'

/** Uses the backend's shared grace/deadline across Chat and CLI views. */
export function RuntimeTimeout({ chatId }: { chatId: string }) {
  const binding = useAppStore(useShallow((state) => {
    const cli = state.cliBindingBySessionId[chatId]
    const runtime = cli?.running ? cli : state.acpBindingBySessionId[chatId]
    return { running: Boolean(runtime?.idleShutdownAtMs && runtime.running), idleCountdownStartsAtMs: runtime?.idleCountdownStartsAtMs, idleShutdownAtMs: runtime?.idleShutdownAtMs }
  }))
  const now = useCountdownClock(binding.running, binding.idleCountdownStartsAtMs, binding.idleShutdownAtMs)
  if (!binding?.running || !binding.idleCountdownStartsAtMs || !binding.idleShutdownAtMs || now < binding.idleCountdownStartsAtMs) return null
  const seconds = Math.max(0, Math.ceil((binding.idleShutdownAtMs - now) / 1000))
  const remaining = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`
  const ending = seconds <= 60
  // Same timer icon and final-minute warning tone as the chat header control; the name carries the tooltip's
  // grace-period sentence because this non-focusable status cannot open the tooltip from the keyboard.
  return <Tooltip label="Activity restarts the one-minute grace period">
    <span role="timer" data-phase={ending ? 'ending' : 'idle'} aria-label={`Runtime idle timeout · stops in ${remaining}. Activity restarts the one-minute grace period.`}
      className="inline-flex shrink-0 items-center gap-1 text-xs tabular-nums" style={{ color: ending ? 'var(--warning-text)' : 'var(--text-2)' }}>
      <Timer size={14} aria-hidden />
      Stops in {remaining}
    </span>
  </Tooltip>
}
