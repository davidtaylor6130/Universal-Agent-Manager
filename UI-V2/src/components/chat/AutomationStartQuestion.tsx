import { MessageSquare } from 'lucide-react'
import { isCompanionContext } from '../../ipc/cefBridge'
import type { Session } from '../../types/session'
import { Button } from '../ui'
import { QuestionCollapse } from './QuestionInput'
import './QuestionInput.css'

/** The approval question under a held automation prompt, in the same card as a provider question. */
export function AutomationStartQuestion({ session, open, error, onStart }: { session: Session; open: boolean; error: string; onStart: () => void }) {
  return <QuestionCollapse open={open}>
    <section className="qr-panel" data-variant="float" data-open="true" aria-label="Start this chat" data-testid="automation-start-question">
      <header className="qr-header">
        <MessageSquare size={14} aria-hidden />
        <h2>Question</h2>
      </header>
      <div className="qr-body">
        <fieldset className="qr-question">
          <legend>Start this chat?</legend>
          <p className="qr-multiple-hint">Started by {session.automationTokenName}. Up to {session.automationMaxIterations} iterations, {session.automationGitPermission === 'pr' ? 'may open a pull request' : 'commit only'}.</p>
        </fieldset>
        {error && <p role="alert" className="qr-error">{error}</p>}
      </div>
      <footer className="qr-footer">
        <span className="qr-status" role="status">{isCompanionContext() ? 'Start it on the computer.' : ''}</span>
        {!isCompanionContext() && <div className="qr-actions"><Button type="button" variant="primary" size="sm" onClick={onStart}>Start chat</Button></div>}
      </footer>
    </section>
  </QuestionCollapse>
}
