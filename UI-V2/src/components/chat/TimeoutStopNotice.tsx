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
 * Raises the turn timeout, then restarts the turn in place. A refused restart puts the previous timeout back,
 * so Restart never leaves a changed setting behind a turn that did not run.
 */
export async function restartWithRaisedTimeout({ previous, raised, queued, retryable, saveTimeout, retry, limitName = 'Timeout' }: {
  previous: number
  raised: number
  queued: boolean
  retryable: boolean
  saveTimeout: (seconds: number) => Promise<boolean>
  retry: () => Promise<{ ok: boolean; error?: string }>
  /** Which limit Restart raises, as it reads at the start of a sentence. */
  limitName?: string
}): Promise<TimeoutRestartOutcome | undefined> {
  if (!await saveTimeout(raised)) return { tone: 'error', message: `Could not save the new ${limitName.toLowerCase()}. Nothing was restarted.` }
  const raisedText = `${limitName} raised to ${formatTimeoutDuration(raised)}.`
  if (queued) return { tone: 'info', message: `${raisedText} Queued messages are sent first, so this turn was not restarted; send a follow-up afterwards to continue it.` }
  if (!retryable) return { tone: 'info', message: `${raisedText} This turn already produced output, so send a follow-up to continue it.` }
  const result = await retry()
  if (result.ok) return undefined
  const refused = `The turn did not restart: ${result.error || 'retry was refused.'}`
  return { tone: 'error', message: await saveTimeout(previous) ? refused : `${raisedText} ${refused}` }
}

/**
 * A timeout stop explained in plain words, replacing the raw provider error banner. Restart is offered
 * only for a limit UAM owns (a known value); another provider limit is explained without it. A stream-watch
 * loop stop (#636) uses the same notice with the repeated content and a plain restart instead of +15 min.
 * A tool-call limit stop names the tool call, and its restart raises that limit instead of the turn timeout.
 */
export function TimeoutStopNotice({ timeoutStop, onRestart, onDismiss, copyAction, restartLabel = 'Restart' }: {
  timeoutStop: AcpTimeoutStop
  /** Loop stops only: what the restart action does (in place, or in a new branch chat). */
  restartLabel?: string
  /** Resolves with what happened instead, or nothing when the turn restarted. */
  onRestart: () => Promise<TimeoutRestartOutcome | undefined>
  onDismiss: () => void
  copyAction?: ReactNode
}) {
  const [restarting, setRestarting] = useState(false)
  const [outcome, setOutcome] = useState<TimeoutRestartOutcome>()
  const loop = timeoutStop.kind === 'loop'
  const tool = timeoutStop.kind === 'tool'
  const owned = loop || timeoutStop.seconds > 0
  const value = !loop && owned ? ` (${formatTimeoutDuration(timeoutStop.seconds)})` : ''
  return (
    <>
      <Notice
        tone="warning"
        title={loop ? 'Turn stopped: repeating output' : 'Turn timed out'}
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
                {loop ? restartLabel : 'Restart with +15 min'}
              </Button>
            )}
          </>
        )}
      >
        {loop ? (
          <>
            <p>The provider repeated the same {timeoutStop.channel || 'output'} {timeoutStop.count} times, so this turn was stopped. Nothing was lost: your messages and any output so far are saved.</p>
            {timeoutStop.example && (
              <pre aria-label="Repeated content" className="mt-1 max-h-24 overflow-auto whitespace-pre-wrap rounded px-2 py-1 font-mono" style={{ background: 'var(--surface)', color: 'var(--text-2)', overflowWrap: 'anywhere' }}>{timeoutStop.example}</pre>
            )}
          </>
        ) : (
          <p>{tool ? `A tool call exceeded its ${formatTimeoutDuration(timeoutStop.seconds)} limit with no result` : `${timeoutStop.label}${value} expired`}, so this turn was stopped. Nothing was lost: your messages and any output so far are saved.</p>
        )}
        <p className="mt-1" style={{ color: 'var(--text-3)' }}>
          {loop
            ? `${restartLabel === 'Restart' ? '' : 'Continue in new chat re-runs your message in a branch chat; queued messages stay in this chat. '}You can change or turn off this limit in Settings, Goal Loops, Repeated output limit.`
            : owned ? `You can also change this limit in Settings, Goal Loops, ${tool ? 'Tool call limit' : 'Provider turn timeout'}.` : 'This limit is set in the provider’s own configuration, not by UAM.'}
        </p>
        {outcome?.tone === 'info' && <p className="mt-1">{outcome.message}</p>}
      </Notice>
      {outcome?.tone === 'error' && (
        <p role="alert" className="mb-2 px-2 text-xs" style={{ color: 'var(--red)' }}>{outcome.message}</p>
      )}
    </>
  )
}
