import { createContext, useId, useState, type ReactNode } from 'react'
import { Copy } from 'lucide-react'
import { Tooltip } from '../ui'
import { copyTextToClipboard } from '../../utils/copySelection'
import { reportAppFailure } from '../../utils/appFailure'

export type BulkTarget = { id: string; label: string }

/** FolderTree's chat multi-selection, read when a selected row opens its menu, so selection changes never re-render rows. */
export type ChatSelection = { selectedIds: () => string[]; deleteSelected: () => void }
export const ChatSelectionContext = createContext<ChatSelection | null>(null)

/** Runs the action for every target in order; every failure is reported by name in one shared notification. Returns succeeded ids. */
export async function runForEach(targets: BulkTarget[], action: (target: BulkTarget) => Promise<string | null>, failureMessage: string): Promise<string[]> {
  const succeeded: string[] = []
  const failed: string[] = []
  for (const target of targets) {
    let error: string | null
    try { error = await action(target) }
    catch (cause) { error = cause instanceof Error && cause.message.trim() ? cause.message : 'The request failed.' }
    if (error === null) succeeded.push(target.id)
    else failed.push(`“${target.label}”: ${error}`)
  }
  if (failed.length > 0) reportAppFailure(`${failureMessage} (${failed.length} of ${targets.length})`, failed.join(' · '))
  return succeeded
}

export const singleOnlyReason = (noun: 'chat' | 'workspace') => `Not available for multiple selection — select a single ${noun}`

/** A single-target action kept visible but unavailable while several rows are selected. */
export function SingleOnlyMenuItem({ icon, label, noun }: { icon: ReactNode; label: string; noun: 'chat' | 'workspace' }) {
  const reasonId = useId()
  const reason = singleOnlyReason(noun)
  return <>
    <Tooltip label={reason} side="right">
      <button type="button" role="menuitem" aria-disabled="true" aria-describedby={reasonId} className="uam-sb-menu-item" onClick={(event) => event.preventDefault()}>
        {icon}{label}
      </button>
    </Tooltip>
    <span id={reasonId} hidden>{reason}</span>
  </>
}

export function MenuDivider() {
  return <div className="uam-sb-menu-divider" role="separator" />
}

export type CopyDetail = { label: string; value: string }

/** Copy rows live inside the Copy submenu, so their status resets when the submenu closes. */
export function CopyDetailItems({ items }: { items: CopyDetail[] }) {
  const [copying, setCopying] = useState(false)
  const [result, setResult] = useState<{ label: string; ok: boolean } | null>(null)
  const copy = async ({ label, value }: CopyDetail) => {
    if (copying) return
    setCopying(true)
    setResult(null)
    try { setResult({ label, ok: await copyTextToClipboard(value, document) }) }
    catch { setResult({ label, ok: false }) }
    finally { setCopying(false) }
  }
  return <>
    {items.map((item) => <button key={item.label} type="button" role="menuitem" disabled={copying} className="uam-sb-menu-item"
      style={{ cursor: copying ? 'wait' : 'pointer' }} onClick={() => { void copy(item) }}>
      <Copy size={13} aria-hidden />{item.label}
    </button>)}
    {result && <p role={result.ok ? 'status' : 'alert'} className="px-3 py-1 text-xs" style={{ color: result.ok ? 'var(--text-2)' : 'var(--error)' }}>
      {result.ok ? `${result.label} copied.` : 'Could not copy. Try again.'}
    </p>}
  </>
}
