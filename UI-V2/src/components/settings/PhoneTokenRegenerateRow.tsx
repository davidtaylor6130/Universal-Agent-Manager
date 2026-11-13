import { useState } from 'react'
import { RotateCcw } from 'lucide-react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { reportAppNotice } from '../../utils/appFailure'
import { Button, ConfirmDialog } from '../ui'
import { SettingRow } from './SettingsPrimitives'

interface PhoneTokenRegenerateRowProps {
  configured: boolean
  disabled?: boolean
}

/** Desktop-only: replaces the shared phone credential, so every paired device must pair again. */
export function PhoneTokenRegenerateRow({ configured, disabled = false }: PhoneTokenRegenerateRowProps) {
  const [open, setOpen] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  if (isCompanionContext()) return null

  const regenerate = async () => {
    if (busy) return
    setBusy(true)
    setError('')
    try {
      const response = await sendToCEF({ action: 'regenerateCompanionToken' })
      if (!response.ok) { setError(response.error || 'Could not regenerate the phone access token. Try again.'); return }
      setOpen(false)
      reportAppNotice('New phone access token issued. Pair your phones and browsers again.')
    } catch {
      setError('Could not regenerate the phone access token. Try again.')
    } finally {
      setBusy(false)
    }
  }

  return <>
    <SettingRow title="Regenerate token" description="Replace the access token. Paired phones and browsers disconnect until they pair again."
      control={<Button size="sm" variant="secondary" leadingIcon={<RotateCcw size={14} aria-hidden />} disabled={!configured || disabled || busy}
        onClick={() => { setError(''); setOpen(true) }}>Regenerate token</Button>} />
    <ConfirmDialog open={open} settingsOwned title="Regenerate phone access token?" confirmLabel="Regenerate token" busy={busy} error={error}
      onConfirm={() => void regenerate()} onCancel={() => { if (!busy) setOpen(false) }}>
      Every paired phone and browser disconnects right away and must pair again with the new token or pairing code.
    </ConfirmDialog>
  </>
}
