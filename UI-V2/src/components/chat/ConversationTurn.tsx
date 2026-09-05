import type { ReactNode } from 'react'
import { ChevronLeft, ChevronRight, Pencil, RotateCcw } from 'lucide-react'
import { IconButton } from '../ui'
import { CopyTextButton } from './StatusHelpers'
import './conversation.css'

/** A conversation turn using the chat's existing message and branch actions. */
export function ConversationTurn({
  role, children, assistantLabel, copyText = '', branchLabel, branchNavigation,
  onEdit, onRevert, actionsDisabled = false, streaming = false, goalReview = false,
}: {
  role: 'user' | 'assistant'
  children: ReactNode
  assistantLabel: string
  copyText?: string
  branchLabel?: string
  branchNavigation?: { current: number; total: number; onPrevious: () => void; onNext: () => void }
  onEdit?: () => void
  onRevert?: () => void
  actionsDisabled?: boolean
  streaming?: boolean
  goalReview?: boolean
}) {
  return <article className={`conversation-turn conversation-turn--${role}`} aria-label={role === 'user' ? 'You' : goalReview ? 'Goal Reviewer' : assistantLabel} data-streaming={streaming || undefined}>
    <div className="conversation-turn__body">{children}</div>
    <footer className="conversation-turn__footer">
      {role === 'assistant' && <span className="conversation-turn__attribution">{goalReview ? 'Goal Reviewer' : assistantLabel}</span>}
      {branchLabel && <span>{branchLabel}</span>}
      <div className="conversation-turn__actions">
        {copyText.trim() && <CopyTextButton text={copyText} label="Copy message" title="Copy message" />}
        {onEdit && <IconButton icon={<Pencil size={13} aria-hidden />} label="Edit message in new branch" size="sm" disabled={actionsDisabled} onClick={onEdit} />}
        {onRevert && <IconButton icon={<RotateCcw size={13} aria-hidden />} label="Revert to message in new branch" size="sm" disabled={actionsDisabled} onClick={onRevert} />}
      </div>
      {branchNavigation && <div className="conversation-turn__branches" role="group" aria-label="Message branches">
        <IconButton icon={<ChevronLeft size={13} aria-hidden />} label="Previous message branch" size="sm" disabled={branchNavigation.current <= 1} onClick={branchNavigation.onPrevious} />
        <span>{branchNavigation.current} / {branchNavigation.total}</span>
        <IconButton icon={<ChevronRight size={13} aria-hidden />} label="Next message branch" size="sm" disabled={branchNavigation.current >= branchNavigation.total} onClick={branchNavigation.onNext} />
      </div>}
    </footer>
  </article>
}
