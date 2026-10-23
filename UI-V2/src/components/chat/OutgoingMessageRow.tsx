import { useRef, useState } from 'react'
import { CornerUpRight, Pencil, X } from 'lucide-react'
import { IconButton } from '../ui'

/** Explicit recovery for undelivered input; nothing here retries, rearms or replays on its own. */
export function OutgoingRecoveryActions({ onEdit, onRetry, onRemove, editDisabled = false, retryDisabled = false, removeDisabled = false, retryUnavailable = false }: {
  onEdit?: () => void
  /** Shows Retry permanently disabled, for receipts whose delivery state cannot authorize a resend. */
  retryUnavailable?: boolean
  onRetry?: () => Promise<boolean>
  onRemove?: () => Promise<boolean>
  editDisabled?: boolean
  retryDisabled?: boolean
  removeDisabled?: boolean
}) {
  const [pending, setPending] = useState(false)
  const pendingRef = useRef(false)
  const [error, setError] = useState('')
  async function run(operation: () => Promise<boolean>, failure: string) {
    if (pendingRef.current) return
    pendingRef.current = true
    setPending(true)
    setError('')
    try {
      if (!await operation()) setError(failure)
    } catch (caught) {
      setError(caught instanceof Error && caught.message.trim() ? caught.message : failure)
    } finally {
      pendingRef.current = false
      setPending(false)
    }
  }
  if (!onEdit && !onRetry && !onRemove && !retryUnavailable) return null
  return <>
    <div className="conversation-turn__actions">
      {onEdit && <IconButton size="sm" icon={<Pencil size={13} aria-hidden />} label="Edit unsent message" disabled={pending || editDisabled} onClick={onEdit} />}
      {(onRetry || retryUnavailable) && <IconButton size="sm" icon={<CornerUpRight size={14} aria-hidden />} label="Retry message" disabled={pending || retryDisabled || !onRetry} onClick={() => onRetry && void run(onRetry, 'Unsent prompt could not be retried. Your input is kept.')} />}
      {onRemove && <IconButton size="sm" icon={<X size={14} aria-hidden />} label="Remove unsent prompt" disabled={pending || removeDisabled} onClick={() => void run(onRemove, 'Unsent prompt could not be removed. Your input is kept.')} />}
    </div>
    {error && <div role="alert" className="conversation-turn__delivery-error">{error}</div>}
  </>
}
