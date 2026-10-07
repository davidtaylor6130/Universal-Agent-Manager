import type { ReactNode } from 'react'
import { ChevronLeft, ChevronRight, Pencil, RotateCcw } from 'lucide-react'
import { IconButton } from '../ui'
import './conversation.css'

/** A conversation turn using the chat's existing message and branch actions. */
export function ConversationTurn({
  role, children, assistantLabel, branchLabel, branchNavigation,
  onEdit, onRevert, revertLabel = 'Revert to message in new branch', actionsDisabled = false, streaming = false, goalReview = false,
}: {
  role: 'user' | 'assistant'
  children: ReactNode
  assistantLabel: string
  branchLabel?: string
  branchNavigation?: { current: number; total: number; onPrevious: () => void; onNext: () => void }
  onEdit?: () => void
  onRevert?: () => void
  revertLabel?: string
  actionsDisabled?: boolean
  streaming?: boolean
  goalReview?: boolean
}) {
  return <article className={`conversation-turn conversation-turn--${role}`} aria-label={role === 'user' ? 'You' : goalReview ? 'Goal Reviewer' : assistantLabel} data-streaming={streaming || undefined}>
    <div className="conversation-turn__body">{children}</div>
    {(branchLabel || onEdit || onRevert || branchNavigation) && <footer className="conversation-turn__footer">
      {branchLabel && <span>{branchLabel}</span>}
      {(onEdit || onRevert) && <div className="conversation-turn__actions">
        {onEdit && <IconButton icon={<Pencil size={13} aria-hidden />} label="Edit message in new branch" size="sm" disabled={actionsDisabled} onClick={onEdit} />}
        {onRevert && <IconButton icon={<RotateCcw size={13} aria-hidden />} label={revertLabel} size="sm" disabled={actionsDisabled} onClick={onRevert} />}
      </div>}
      {branchNavigation && <div className="conversation-turn__branches" role="group" aria-label="Message branches">
        <IconButton icon={<ChevronLeft size={13} aria-hidden />} label="Previous message branch" size="sm" disabled={branchNavigation.current <= 1} onClick={branchNavigation.onPrevious} />
        <span>{branchNavigation.current} / {branchNavigation.total}</span>
        <IconButton icon={<ChevronRight size={13} aria-hidden />} label="Next message branch" size="sm" disabled={branchNavigation.current >= branchNavigation.total} onClick={branchNavigation.onNext} />
      </div>}
    </footer>}
  </article>
}
