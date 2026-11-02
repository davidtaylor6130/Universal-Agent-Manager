import { useState, type ReactNode } from 'react'
import { RotateCcw } from 'lucide-react'
import type { AcpTimeoutStop } from '../../store/cpp/types'
import { Button, Notice } from '../ui'

/** One click raises the adjustable turn timeout by this much and restarts the turn. */
export const TIMEOUT_RESTART_STEP_SECONDS = 15 * 60

export function formatTimeoutDuration(seconds: number) {
  const minutes = Math.round(seconds / 60)
  if (minutes < 60) return `${minutes} min`
  const hours = Math.floor(minutes / 60)
  return minutes % 60 ? `${hours} h ${minutes % 60} min` : `${hours} h`
}

/** A timeout stop explained in plain words, replacing the raw provider error banner. */
export function TimeoutStopNotice({ timeoutStop, onRestart, onDismiss, copyAction }: {
  timeoutStop: AcpTimeoutStop
  /** Resolves with a follow-up message to show, or nothing when the turn restarted. */
  onRestart: () => Promise<string | undefined>
  onDismiss: () => void
  copyAction?: ReactNode
}) {
  const [restarting, setRestarting] = useState(false)
  const [outcome, setOutcome] = useState('')
  const value = timeoutStop.seconds > 0 ? ` (${formatTimeoutDuration(timeoutStop.seconds)})` : ''
  return (
    <Notice
      tone="warning"
      title="Turn timed out"
      dismissLabel="Dismiss timeout notice"
      onDismiss={onDismiss}
      actions={(
        <>
          {copyAction}
          <Button
            size="sm"
            variant="primary"
            leadingIcon={<RotateCcw size={13} aria-hidden />}
            loading={restarting}
            onClick={() => {
              setRestarting(true)
              setOutcome('')
              void onRestart()
                .then((message) => setOutcome(message ?? ''))
                .catch(() => setOutcome('Could not restart the turn. Check the chat before trying again.'))
                .finally(() => setRestarting(false))
            }}
          >
            Restart with +15 min
          </Button>
        </>
      )}
    >
      <p>{timeoutStop.label}{value} expired, so this turn was stopped. Nothing was lost: your messages and any output so far are saved.</p>
      <p className="mt-1" style={{ color: 'var(--text-3)' }}>You can also change this limit in Settings, Goal Loops, Provider turn timeout.</p>
      {outcome && <p className="mt-1" role="status">{outcome}</p>}
    </Notice>
  )
}
