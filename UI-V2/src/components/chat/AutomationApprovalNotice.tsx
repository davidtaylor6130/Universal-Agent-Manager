import { useState } from 'react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { reportAppFailure } from '../../utils/appFailure'
import type { Session } from '../../types/session'
import { Button } from '../ui'

/** The hold on an automation chat: the prompt is shown as plain text so the user reads it before anything launches. */
export function AutomationApprovalNotice({ session }: { session: Session }) {
  const [busy, setBusy] = useState(false)
  const start = async () => {
    setBusy(true)
    const response = await sendToCEF({ action: 'approveAutomationChat', payload: { chatId: session.id } })
    setBusy(false)
    if (!response.ok) reportAppFailure(response.error || 'Could not start this chat.', 'Try again.')
  }
  return <section aria-label="Automation approval" className="grid gap-2 px-4 py-3 text-xs" style={{ borderBottom: '1px solid var(--border)' }}>
    <div className="flex items-center justify-between gap-3">
      <span style={{ color: 'var(--text-2)' }}>Waiting for your approval. Up to {session.automationMaxIterations} iterations, {session.automationGitPermission === 'pr' ? 'may open a pull request' : 'commit only'}.</span>
      {isCompanionContext()
        ? <span style={{ color: 'var(--text-3)' }}>Start it on the computer.</span>
        : <Button size="sm" disabled={busy} onClick={() => void start()}>{busy ? 'Starting…' : 'Start chat'}</Button>}
    </div>
    <pre className="m-0 max-h-40 overflow-auto whitespace-pre-wrap" style={{ color: 'var(--text)', fontFamily: 'var(--font-mono)' }}>{session.automationPendingPrompt}</pre>
  </section>
}
