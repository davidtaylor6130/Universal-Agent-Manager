import { lazy, Suspense, useEffect, useRef, useState, type KeyboardEvent, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { useAppStore } from '../../store/useAppStore'
import { isSingleIconText, lucideIconName, lucideIconValue, resolveCustomIcon, type CustomIcon as Icon, type CustomIconInput, type CustomIconTarget } from '../../types/customIcon'
import { rememberRecentLucideIcon } from '../../utils/lucideIconLibrary'
import { Button } from '../ui'

// The Lucide grid and per-icon loaders are a separate chunk; they load on first use only.
const LazyLucideGlyph = lazy(() => import('./LucideIcons').then((module) => ({ default: module.LucideGlyph })))
const LazyLucideIconLibrary = lazy(() => import('./LucideIcons').then((module) => ({ default: module.LucideIconLibrary })))

type PickerTab = 'library' | 'emoji' | 'upload'
const TABS: Array<{ id: PickerTab; label: string }> = [{ id: 'library', label: 'Library' }, { id: 'emoji', label: 'Emoji' }, { id: 'upload', label: 'Upload' }]

export function CustomIcon({ icon, secondary, fallback }: { icon?: Icon; secondary?: Icon; fallback: ReactNode }) {
  const [failed, setFailed] = useState<string[]>([])
  const usable = resolveCustomIcon(icon, secondary, new Set(failed))
  if (usable?.type === 'text') {
    const libraryName = lucideIconName(usable.value)
    return (
      <span aria-hidden className="inline-flex shrink-0 items-center justify-center" style={{ width: 16, height: 16 }}>
        {libraryName === null ? usable.value : (
          <Suspense fallback={<span className="uam-icon-skeleton block" style={{ width: 16, height: 16 }} />}>
            <LazyLucideGlyph name={libraryName} fallback={fallback} />
          </Suspense>
        )}
      </span>
    )
  }
  return usable?.type === 'png' ? <img alt="" width={16} height={16} className="shrink-0 object-contain" src={usable.dataUrl} onError={() => setFailed((current) => [...current, usable.value])} /> : fallback
}

function IconLibrarySkeleton() {
  return (
    <div role="status" className="grid gap-1" style={{ gridTemplateColumns: 'repeat(8, 32px)' }}>
      <span className="sr-only">Loading icons</span>
      {Array.from({ length: 24 }, (_, index) => <span key={index} aria-hidden className="uam-icon-skeleton block h-8 w-8" />)}
    </div>
  )
}

/** Library icons are the primary choice; emoji/character and PNG upload are secondary tabs. */
export function useCustomIconPicker() {
  const [target, setTarget] = useState<{ type: CustomIconTarget; id: string; label: string } | null>(null)
  const [tab, setTab] = useState<PickerTab>('library')
  const [libraryName, setLibraryName] = useState<string | null>(null)
  const [text, setText] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const dialogRef = useRef<HTMLDialogElement>(null)
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([])
  const generation = useRef(0)
  const close = () => { generation.current++; setTarget(null) }
  useEffect(() => {
    if (!target) return
    const focus = document.activeElement as HTMLElement | null
    const dialog = dialogRef.current
    if (dialog?.showModal) dialog.showModal()
    else dialog?.setAttribute('open', '')
    dialog?.querySelector<HTMLElement>('[role="tab"][aria-selected="true"]')?.focus()
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
  const saveSelection = () => {
    if (tab === 'library' && libraryName) {
      rememberRecentLucideIcon(libraryName)
      void save({ type: 'text', value: lucideIconValue(libraryName) })
    } else if (tab === 'emoji' && isSingleIconText(text)) {
      void save({ type: 'text', value: text })
    }
  }
  const selectTab = (next: PickerTab, focus = false) => {
    setTab(next); setError('')
    if (focus) tabRefs.current[TABS.findIndex((entry) => entry.id === next)]?.focus()
  }
  const onTabKeyDown = (event: KeyboardEvent<HTMLButtonElement>, index: number) => {
    const step = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : 0
    if (!step) return
    event.preventDefault()
    selectTab(TABS[(index + step + TABS.length) % TABS.length].id, true)
  }
  const previewIcon: Icon | undefined = tab === 'library' && libraryName ? { type: 'text', value: lucideIconValue(libraryName) }
    : tab === 'emoji' && isSingleIconText(text) ? { type: 'text', value: text } : undefined
  const canSave = tab === 'library' ? Boolean(libraryName) : tab === 'emoji' && isSingleIconText(text)
  const dialog = target && createPortal(<dialog ref={dialogRef} role="dialog" aria-modal="true" aria-label={`Icon for ${target.label}`} onCancel={(event) => { event.preventDefault(); if (!busy) close() }} className="fixed inset-0 z-[90] m-auto w-80 rounded-xl p-4 backdrop:bg-black/50" style={{ boxShadow: 'var(--elev-3)', background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border-bright)' }} onKeyDown={(event) => {
    if (event.key === 'Escape' && !busy) close()
    if (event.key === 'Tab') {
      const nodes = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('button:not(:disabled),input:not(:disabled)'))
      if (event.shiftKey && document.activeElement === nodes[0]) { event.preventDefault(); nodes[nodes.length - 1]?.focus() }
      if (!event.shiftKey && document.activeElement === nodes[nodes.length - 1]) { event.preventDefault(); nodes[0]?.focus() }
    }
  }}>
    <div className="mb-3 flex items-center gap-3">
      <span aria-hidden className="grid h-10 w-10 shrink-0 place-items-center rounded-lg" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
        {previewIcon ? <CustomIcon icon={previewIcon} fallback={null} /> : <span className="text-xs" style={{ color: 'var(--text-3)' }}>?</span>}
      </span>
      <div className="min-w-0">
        <div className="truncate text-sm font-semibold">Icon for {target.label}</div>
        <div className="text-xs" style={{ color: 'var(--text-3)' }}>Pick a Lucide icon, or use an emoji or PNG.</div>
      </div>
    </div>
    <div role="tablist" aria-label="Icon source" className="mb-3 flex gap-1 rounded-lg p-0.5" style={{ background: 'var(--bg)', border: '1px solid var(--border)' }}>
      {TABS.map((entry, index) => (
        <button key={entry.id} ref={(element) => { tabRefs.current[index] = element }} type="button" role="tab" id={`icon-tab-${entry.id}`} aria-selected={tab === entry.id} aria-controls={`icon-panel-${entry.id}`} tabIndex={tab === entry.id ? 0 : -1} disabled={busy} onClick={() => selectTab(entry.id)} onKeyDown={(event) => onTabKeyDown(event, index)}
          className="flex-1 rounded-md px-2 py-1 text-xs font-medium disabled:opacity-50" style={{ background: tab === entry.id ? 'var(--surface-high)' : 'transparent', color: tab === entry.id ? 'var(--text)' : 'var(--text-2)' }}>{entry.label}</button>
      ))}
    </div>
    <div role="tabpanel" id={`icon-panel-${tab}`} aria-labelledby={`icon-tab-${tab}`}>
      {tab === 'library' && <Suspense fallback={<IconLibrarySkeleton />}><LazyLucideIconLibrary selected={libraryName} onSelect={setLibraryName} /></Suspense>}
      {tab === 'emoji' && <input aria-label="Icon character" maxLength={64} value={text} disabled={busy} onChange={(event) => setText(event.target.value)} onKeyDown={(event) => { if (event.key === 'Enter') saveSelection() }} placeholder="Emoji or character" className="w-full rounded-lg px-3 py-2 text-sm outline-none" style={{ background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)' }} />}
      {tab === 'upload' && <label className="flex cursor-pointer items-center justify-center gap-2 rounded-lg px-3 py-6 text-xs" style={{ border: '1px dashed var(--border-bright)', color: 'var(--text-2)', opacity: busy ? 0.5 : 1 }}>Choose a PNG up to 256 KiB<input aria-label="PNG icon" type="file" accept="image/png" disabled={busy} className="sr-only" onChange={async (event) => {
        const file = event.target.files?.[0]
        if (!file) return
        const request = ++generation.current
        if (file.size > 256 * 1024 || file.type !== 'image/png') { setError('Choose a PNG up to 256 KiB.'); return }
        const reader = new FileReader()
        reader.onload = () => { if (request === generation.current) void save({ type: 'png', base64: String(reader.result).split(',')[1] ?? '' }) }
        reader.onerror = () => { if (request === generation.current) setError('Image could not be read.') }
        reader.readAsDataURL(file)
      }} /></label>}
    </div>
    {error && <p role="alert" className="mt-2 text-xs" style={{ color: 'var(--red)' }}>{error}</p>}
    <div className="mt-4 flex items-center gap-2"><Button size="sm" variant="ghost" disabled={busy} onClick={() => void save(null)}>Reset to default</Button><span className="flex-1" /><Button size="sm" variant="secondary" disabled={busy} onClick={close}>Cancel</Button>{tab !== 'upload' && <Button size="sm" variant="primary" disabled={busy || !canSave} onClick={saveSelection}>Save</Button>}</div>
  </dialog>, document.body)
  return { dialog, open: (type: CustomIconTarget, id: string, label: string) => { generation.current++; setTab('library'); setLibraryName(null); setText(''); setError(''); setTarget({ type, id, label }) } }
}
