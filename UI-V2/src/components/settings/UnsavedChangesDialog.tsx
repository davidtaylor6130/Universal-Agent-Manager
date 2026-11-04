import { ConfirmDialog } from '../ui'

/** Stay / Discard / Save prompt shown when leaving a settings page with unsaved edits. Escape stays. */
export function UnsavedChangesDialog({ title, label, detail, busy = false, saveDisabled = false, error, onStay, onDiscard, onSave }: {
  title: string
  /** Accessible name; defaults to the title. */
  label?: string
  detail?: string
  busy?: boolean
  saveDisabled?: boolean
  error?: string
  onStay: () => void
  onDiscard: () => void
  onSave?: () => void
}) {
  return <ConfirmDialog
    open
    settingsOwned
    tone="primary"
    title={title}
    label={label}
    error={error}
    busy={busy}
    cancelLabel="Keep editing"
    confirmLabel={onSave ? 'Save and leave' : 'Discard'}
    confirmDisabled={onSave ? saveDisabled : false}
    secondaryAction={onSave ? { label: 'Discard', onClick: onDiscard } : undefined}
    onConfirm={onSave ?? onDiscard}
    onCancel={onStay}
  >
    {detail ?? 'Your changes have not been saved.'}
  </ConfirmDialog>
}
