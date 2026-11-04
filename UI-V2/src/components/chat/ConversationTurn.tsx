import type { ReactNode } from 'react'
import { ChevronLeft, ChevronRight, Pencil, RotateCcw } from 'lucide-react'
import { IconButton, Tooltip } from '../ui'
import './conversation.css'

/** Outgoing input not yet confirmed as delivered; `label` explains the state and its recovery.
 * `unconfirmed` is native uncertainty and is never presented as a definite failure. */
export type ConversationDelivery = { state: 'sending' | 'unsent' | 'unconfirmed'; label: string }
const DELIVERY_TEXT = { sending: 'Sending', unsent: 'Not sent', unconfirmed: 'Delivery not confirmed' } as const

/** A conversation turn using the chat's existing message and branch actions. */
export function ConversationTurn({
  role, children, assistantLabel, branchLabel, branchNavigation,
  onEdit, onRevert, revertLabel = 'Revert to message in new branch', actionsDisabled = false, streaming = false, goalReview = false,
  delivery, deliveryActions,
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
  delivery?: ConversationDelivery
  /** Recovery actions replace branch actions while the input is undelivered. */
  deliveryActions?: ReactNode
}) {
  const branchActions = !delivery && (onEdit || onRevert)
  return <article className={`conversation-turn conversation-turn--${role}`} aria-label={role === 'user' ? 'You' : goalReview ? 'Goal Reviewer' : assistantLabel} data-streaming={streaming || undefined} data-delivery={delivery?.state}>
    <div className="conversation-turn__body">{children}</div>
    {(branchLabel || branchActions || branchNavigation || delivery) && <footer className="conversation-turn__footer">
      {delivery && <Tooltip label={delivery.label}>
        <span className="conversation-turn__delivery" tabIndex={0} role="status" aria-label={`${DELIVERY_TEXT[delivery.state]}. ${delivery.label}`}>
          {DELIVERY_TEXT[delivery.state]}
        </span>
      </Tooltip>}
      {delivery && deliveryActions}
      {branchLabel && <span>{branchLabel}</span>}
      {branchActions && <div className="conversation-turn__actions">
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
