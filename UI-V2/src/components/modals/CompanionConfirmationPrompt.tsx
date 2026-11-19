import { useEffect, useState } from 'react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import type { CompanionConfirmation } from '../../store/cpp/types'
import { reportAppFailure } from '../../utils/appFailure'
import { ConfirmDialog } from '../ui'

export const COMPANION_CONFIRMATIONS_EVENT = 'uam-companion-confirmations'

/** Asks the desktop user to allow or deny a phone request that would loosen what an agent may do. */
export function CompanionConfirmationPrompt() {
  const [items, setItems] = useState<CompanionConfirmation[]>([])
  const [now, setNow] = useState(() => Date.now())
  const [busy, setBusy] = useState(false)
  useEffect(() => {
    if (isCompanionContext()) return
    const onItems = (event: Event) => setItems((event as CustomEvent<{ items: CompanionConfirmation[] }>).detail.items)
    window.addEventListener(COMPANION_CONFIRMATIONS_EVENT, onItems)
    return () => window.removeEventListener(COMPANION_CONFIRMATIONS_EVENT, onItems)
  }, [])
  const item = items.find((candidate) => candidate.expiresAt > now)
  // Hide a request when it expires on the desktop; the main device refuses it after that anyway.
  useEffect(() => {
    if (!item) return
    const timer = window.setTimeout(() => setNow(Date.now()), Math.max(0, item.expiresAt - Date.now()))
    return () => window.clearTimeout(timer)
  }, [item])
  if (!item) return null

  const resolve = async (allow: boolean) => {
    setBusy(true)
    const response = await sendToCEF({ action: 'resolveCompanionConfirmation', payload: { id: item.id, allow } })
    setBusy(false)
    // The main device consumes the request either way, so the next one (if any) takes its place.
    setItems((current) => current.filter((candidate) => candidate.id !== item.id))
    if (!response.ok && allow) reportAppFailure('The phone change was not applied.', response.error || 'Ask again from the phone.')
  }

  return (
    <ConfirmDialog
      open
      title="Allow this change from your phone?"
      confirmLabel="Allow"
      cancelLabel="Deny"
      busy={busy}
      onConfirm={() => void resolve(true)}
      onCancel={() => void resolve(false)}
    >
      <p>{item.description}{item.chatTitle ? <> in “{item.chatTitle}”</> : null}.</p>
      <p className="mt-2">Phone access asked for this. It gives the agent more freedom, so it only applies once you allow it here.</p>
    </ConfirmDialog>
  )
}
