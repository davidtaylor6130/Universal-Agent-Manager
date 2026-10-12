import { Button } from '../ui'

/** Stay / Discard / Save prompt shown when leaving a settings page with unsaved edits. Escape stays. */
export function UnsavedChangesDialog({ title, label, detail, busy = false, error, onStay, onDiscard, onSave }: {
  title: string
  /** Accessible name; defaults to the title. */
  label?: string
  detail?: string
  busy?: boolean
  error?: string
  onStay: () => void
  onDiscard: () => void
  onSave?: () => void
}) {
  return <div className="uam-overlay fixed inset-0 z-[80] flex items-center justify-center p-4" style={{ background: 'rgba(0,0,0,.5)' }} onClick={event => { if (event.target === event.currentTarget && !busy) onStay() }}>
    <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label={label ?? title} className="w-full max-w-sm rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
      onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); if (!busy) onStay() } }}>
      <div className="px-5 pt-5 pb-3">
        <h3 className="text-sm font-semibold" style={{ color: 'var(--text)' }}>{title}</h3>
        <p className="mt-1.5 text-xs" style={{ color: 'var(--text-2)' }}>{detail ?? 'Your changes have not been saved.'}</p>
        {error && <p role="alert" className="mt-2 text-xs" style={{ color: 'var(--red)' }}>{error}</p>}
      </div>
      <div className="flex justify-end gap-2 px-5 pb-5 pt-2">
        <Button size="sm" autoFocus disabled={busy} onClick={onStay}>Keep editing</Button>
        <Button size="sm" variant="danger" disabled={busy} onClick={onDiscard}>Discard</Button>
        {onSave && <Button size="sm" variant="primary" loading={busy} onClick={onSave}>Save and leave</Button>}
      </div>
    </div>
  </div>
}
