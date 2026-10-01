import { useEffect, useRef, useState, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { useAppStore } from '../../store/useAppStore'
import { isSingleIconText, resolveCustomIcon, type CustomIcon as Icon, type CustomIconInput, type CustomIconTarget } from '../../types/customIcon'
import { Button } from '../ui'

export function CustomIcon({ icon, secondary, fallback }: { icon?: Icon; secondary?: Icon; fallback: ReactNode }) {
  const [failed, setFailed] = useState<string[]>([])
  const usable = resolveCustomIcon(icon, secondary, new Set(failed))
  return usable?.type === 'text' ? <span aria-hidden className="inline-flex shrink-0 items-center justify-center" style={{ width: 16, height: 16 }}>{usable.value}</span> : usable?.type === 'png' ? <img alt="" width={16} height={16} className="shrink-0 object-contain" src={usable.dataUrl} onError={() => setFailed((current) => [...current, usable.value])} /> : fallback
}

/** The native asset store validates PNG dimensions, bytes and portable references. */
export function useCustomIconPicker() {
  const [target, setTarget] = useState<{ type: CustomIconTarget; id: string; label: string } | null>(null)
  const [text, setText] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const dialogRef = useRef<HTMLDialogElement>(null)
  const generation = useRef(0)
  const close = () => { generation.current++; setTarget(null) }
  useEffect(() => {
    if (!target) return
    const focus = document.activeElement as HTMLElement | null
    const dialog = dialogRef.current
    if (dialog?.showModal) dialog.showModal()
    else dialog?.setAttribute('open', '')
    dialog?.querySelector<HTMLInputElement>('input')?.focus()
    return () => { dialog?.close?.(); focus?.focus() }
  }, [target])
  const save = async (icon: CustomIconInput) => {
    if (!target || busy) return
    const request = generation.current
    setBusy(true); setError('')
    try {
      const ok = await useAppStore.getState().setCustomIcon(target.type, target.id, icon)
      if (generation.current !== request) return
      if (ok) close()
      else setError('Icon could not be saved.')
    } catch { if (generation.current === request) setError('Icon could not be saved.') }
    finally { setBusy(false) }
  }
  const dialog = target && createPortal(<dialog ref={dialogRef} role="dialog" aria-modal="true" aria-label={`Icon for ${target.label}`} onCancel={(event) => { event.preventDefault(); if (!busy) close() }} className="fixed inset-0 z-[90] m-auto w-80 p-4" style={{ background: '#000', color: '#fff', border: '1px solid var(--border-bright)' }} onKeyDown={(event) => {
    if (event.key === 'Escape' && !busy) close()
    if (event.key === 'Tab') {
      const nodes = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('button:not(:disabled),input:not(:disabled)'))
      if (event.shiftKey && document.activeElement === nodes[0]) { event.preventDefault(); nodes[nodes.length - 1]?.focus() }
      if (!event.shiftKey && document.activeElement === nodes[nodes.length - 1]) { event.preventDefault(); nodes[0]?.focus() }
    }
  }}>
    <div className="mb-3 text-sm font-semibold">Icon for {target.label}</div>
    <input aria-label="Icon character" maxLength={64} value={text} disabled={busy} onChange={(event) => setText(event.target.value)} placeholder="Emoji or character" className="w-full border px-2 py-1" style={{ background: '#000', color: '#fff', borderColor: 'var(--border)' }} />
    <label className="mt-3 block text-xs">PNG image<input aria-label="PNG icon" type="file" accept="image/png" disabled={busy} className="mt-1 block w-full" onChange={async (event) => {
      const file = event.target.files?.[0]
      if (!file) return
      if (file.size > 256 * 1024 || file.type !== 'image/png') { setError('Choose a PNG up to 256 KiB.'); return }
      const request = generation.current
      const reader = new FileReader()
      reader.onload = () => { if (request === generation.current) void save({ type: 'png', base64: String(reader.result).split(',')[1] ?? '' }) }
      reader.onerror = () => setError('Image could not be read.')
      reader.readAsDataURL(file)
    }} /></label>
    {error && <p role="alert" className="mt-2 text-xs">{error}</p>}
    <div className="mt-4 flex justify-end gap-2"><Button size="sm" disabled={busy} onClick={() => void save(null)}>Reset</Button><Button size="sm" disabled={busy} onClick={close}>Cancel</Button><Button size="sm" disabled={busy || !isSingleIconText(text)} onClick={() => void save({ type: 'text', value: text })}>Save</Button></div>
  </dialog>, document.body)
  return { dialog, open: (type: CustomIconTarget, id: string, label: string) => { generation.current++; setText(''); setError(''); setTarget({ type, id, label }) } }
}
