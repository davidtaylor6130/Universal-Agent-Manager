import { useEffect, useRef } from 'react'
import type { ReactNode } from 'react'
import { TriangleAlert } from 'lucide-react'
import { Button } from './Button'
import { Overlay } from './Overlay'

export interface ConfirmDialogProps {
  open: boolean
  title: string
  children: ReactNode
  confirmLabel: string
  onConfirm: () => void
  onCancel: () => void
  /** Accessible name; defaults to the title. */
  label?: string
  tone?: 'danger' | 'primary'
  busy?: boolean
  error?: string
}

/** Small alert dialog for confirming a (usually destructive) action. Esc cancels. */
export function ConfirmDialog({ open, title, children, confirmLabel, onConfirm, onCancel, label, tone = 'danger', busy = false, error }: ConfirmDialogProps) {
  const cancelRef = useRef<HTMLButtonElement>(null)
  const onCancelRef = useRef(onCancel)
  onCancelRef.current = onCancel

  useEffect(() => {
    if (!open) return
    const returnFocus = document.activeElement as HTMLElement | null
    cancelRef.current?.focus()
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      event.preventDefault()
      event.stopPropagation()
      onCancelRef.current()
    }
    window.addEventListener('keydown', onKey, true)
    return () => {
      window.removeEventListener('keydown', onKey, true)
      if (returnFocus?.isConnected) returnFocus.focus()
    }
  }, [open])

  return (
    <Overlay open={open} zIndex={70} onBackdropMouseDown={() => { if (!busy) onCancel() }}>
      <div
        role="alertdialog"
        aria-modal="true"
        aria-label={label ?? title}
        className="uam-confirm-dialog w-full max-w-sm rounded-xl"
      >
        <div className="flex gap-3 p-5">
          {tone === 'danger' && (
            <span className="uam-confirm-dialog__icon" aria-hidden>
              <TriangleAlert size={16} />
            </span>
          )}
          <div className="min-w-0 flex-1">
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>{title}</div>
            <div className="mt-1.5 text-sm leading-5" style={{ color: 'var(--text-2)', overflowWrap: 'anywhere' }}>{children}</div>
            {error && <p role="alert" className="mt-3 text-sm" style={{ color: 'var(--red)' }}>{error}</p>}
          </div>
        </div>
        <div className="flex justify-end gap-2 px-5 pb-5">
          <Button ref={cancelRef} size="sm" disabled={busy} onClick={onCancel}>Cancel</Button>
          <Button size="sm" variant={tone === 'danger' ? 'danger' : 'primary'} loading={busy} onClick={onConfirm}>{confirmLabel}</Button>
        </div>
      </div>
    </Overlay>
  )
}
