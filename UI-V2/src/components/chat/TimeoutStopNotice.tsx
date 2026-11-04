import { useState, type ReactNode } from 'react'
import { RotateCcw } from 'lucide-react'
import type { AcpTimeoutStop } from '../../store/cpp/types'
import { Button, Notice } from '../ui'

/** One click raises the adjustable turn timeout by this much and restarts the turn. */
export const TIMEOUT_RESTART_STEP_SECONDS = 15 * 60

/** What Restart did when it did not simply restart the turn. */
export interface TimeoutRestartOutcome {
  tone: 'info' | 'error'
  message: string
}

export function formatTimeoutDuration(seconds: number) {
  const minutes = Math.round(seconds / 60)
  if (minutes < 60) return `${minutes} min`
  const hours = Math.floor(minutes / 60)
  return minutes % 60 ? `${hours} h ${minutes % 60} min` : `${hours} h`
}

/**
 * A timeout stop explained in plain words, replacing the raw provider error banner. Restart is offered
 * only for a limit UAM owns (a known value); another provider limit is explained without it.
 */
export function TimeoutStopNotice({ timeoutStop, onRestart, onDismiss, copyAction }: {
  timeoutStop: AcpTimeoutStop
  /** Resolves with what happened instead, or nothing when the turn restarted. */
  onRestart: () => Promise<TimeoutRestartOutcome | undefined>
  onDismiss: () => void
  copyAction?: ReactNode
}) {
  const [restarting, setRestarting] = useState(false)
  const [outcome, setOutcome] = useState<TimeoutRestartOutcome>()
  const owned = timeoutStop.seconds > 0
  const value = owned ? ` (${formatTimeoutDuration(timeoutStop.seconds)})` : ''
  return (
    <>
      <Notice
        tone="warning"
        title="Turn timed out"
        dismissLabel="Dismiss timeout notice"
        onDismiss={onDismiss}
        actions={(
          <>
            {copyAction}
            {owned && (
              <Button
                size="sm"
                variant="primary"
                leadingIcon={<RotateCcw size={13} aria-hidden />}
                loading={restarting}
                onClick={() => {
                  setRestarting(true)
                  setOutcome(undefined)
                  void onRestart()
                    .then(setOutcome)
                    .catch(() => setOutcome({ tone: 'error', message: 'Could not restart the turn. Check the chat before trying again.' }))
                    .finally(() => setRestarting(false))
                }}
              >
                Restart with +15 min
              </Button>
            )}
          </>
        )}
      >
        <p>{timeoutStop.label}{value} expired, so this turn was stopped. Nothing was lost: your messages and any output so far are saved.</p>
        <p className="mt-1" style={{ color: 'var(--text-3)' }}>
          {owned ? 'You can also change this limit in Settings, Goal Loops, Provider turn timeout.' : 'This limit is set in the provider’s own configuration, not by UAM.'}
        </p>
        {outcome?.tone === 'info' && <p className="mt-1">{outcome.message}</p>}
      </Notice>
      {outcome?.tone === 'error' && (
        <p role="alert" className="mb-2 px-2 text-xs" style={{ color: 'var(--red)' }}>{outcome.message}</p>
      )}
    </>
  )
}
