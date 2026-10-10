import { useEffect, useMemo, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { FileText, RefreshCw, X } from 'lucide-react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import type { WorkspaceFilePreview } from '../../types/filePreview'
import type { FileReference } from '../../utils/fileReferences'
import { getTopmostModal } from '../../utils/modalFocus'
import { CopyTextButton } from '../chat/StatusHelpers'
import { IconButton } from '../ui'

const ROW_HEIGHT = 20

export function FilePreviewDialog({ chatId, reference, onClose, onShowChanges, zIndex = 1100 }: {
  chatId: string; reference: FileReference; onClose: () => void; onShowChanges?: (file: WorkspaceFilePreview | null) => Promise<void>; zIndex?: number
}) {
  const read = useAppStore((state) => state.getWorkspaceFilePreview)
  const context = useAppStore(useShallow((state) => {
    const session = state.sessions.find((candidate) => candidate.id === chatId)
    return [session?.workspaceDirectory, session?.workspaceWorktreeDirectory, state.folders.find((folder) => folder.id === session?.folderId)?.directory, session?.executionHostId, session?.importedReadOnly]
  }))
  const dialogRef = useRef<HTMLElement>(null)
  const viewportRef = useRef<HTMLDivElement>(null)
  const requestKey = JSON.stringify([chatId, reference.path, context])
  const [loaded, setLoaded] = useState<{ key: string; file: WorkspaceFilePreview } | null>(null)
  const file = loaded?.key === requestKey ? loaded.file : null
  const [error, setError] = useState('')
  const [refresh, setRefresh] = useState(0)
  const [changesLoading, setChangesLoading] = useState(false)
  const [changesError, setChangesError] = useState('')
  const [viewport, setViewport] = useState({ top: 0, height: 600 })
  const lines = useMemo(() => file ? file.text.replace(/\r\n/g, '\n').split('\n') : [], [file])
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    const dialog = dialogRef.current
    dialog?.focus()
    const escape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || event.defaultPrevented || getTopmostModal() !== dialog) return
      event.preventDefault(); onClose()
    }
    window.addEventListener('keydown', escape)
    return () => {
      window.removeEventListener('keydown', escape)
      if (previous?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) previous.focus()
    }
  }, [onClose])
  useEffect(() => {
    let cancelled = false
    setLoaded(null); setError(''); setChangesError('')
    void read(chatId, reference.path).then((next) => { if (!cancelled) setLoaded({ key: requestKey, file: next }) }).catch((reason) => {
      if (!cancelled) setError(reason instanceof Error ? reason.message : 'Could not load this file.')
    })
    return () => { cancelled = true }
  }, [chatId, requestKey, read, reference.path, refresh])
  useEffect(() => {
    const element = viewportRef.current
    if (!element || !file) return
    element.scrollTop = Math.max(0, Math.min(lines.length - 1, (reference.line ?? 1) - 1) * ROW_HEIGHT - element.clientHeight / 3)
    const measure = () => setViewport({ top: element.scrollTop, height: element.clientHeight || 600 })
    measure()
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(measure)
    observer?.observe(element)
    return () => observer?.disconnect()
  }, [file, lines.length, reference.line])
  const start = Math.max(0, Math.floor(viewport.top / ROW_HEIGHT) - 12)
  const end = Math.min(lines.length, Math.ceil((viewport.top + viewport.height) / ROW_HEIGHT) + 12)
  const width = useMemo(() => lines.reduce((longest, line) => Math.max(longest, line.length), 40), [lines])
  return createPortal(<div className="fixed inset-0 z-[1100] flex items-center justify-center p-2 sm:p-4" style={{ zIndex, background: 'rgba(0,0,0,.6)' }} onMouseDown={onClose}>
    <section ref={dialogRef} role="dialog" aria-modal="true" aria-label={`Inspect ${reference.path}`} tabIndex={-1}
      className="flex h-[85vh] w-full max-w-5xl flex-col overflow-hidden" style={{ background: '#000', border: '1px solid var(--border-bright)' }} onMouseDown={(event) => event.stopPropagation()}>
      <header className="flex min-h-12 items-center gap-3 px-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <FileText size={17} aria-hidden style={{ color: 'var(--accent)' }} />
        <div className="min-w-0 flex-1"><div className="truncate text-sm" title={reference.path} style={{ color: 'var(--text)' }}>{file?.path ?? reference.path}</div>
          {file && <div className="truncate text-xs" title={`${file.executionHostId}: ${file.workspace}`} style={{ color: 'var(--text-2)' }}>{file.executionHostId} · {file.workspace}</div>}</div>
        {onShowChanges && <button type="button" className="text-xs" disabled={changesLoading} style={{ color: 'var(--accent)', background: 'transparent', border: 'none' }} onClick={() => {
          setChangesLoading(true); setChangesError('')
          void onShowChanges(file).catch((reason) => setChangesError(reason instanceof Error ? reason.message : 'Could not load changes.')).finally(() => setChangesLoading(false))
        }}>{changesLoading ? 'Loading changes…' : 'Changes'}</button>}
        {file && <CopyTextButton text={file.text} label="Copy file" title="Copy file contents" />}
        <IconButton icon={<RefreshCw size={15} />} label="Reload file" onClick={() => setRefresh((value) => value + 1)} />
        <IconButton icon={<X size={16} />} label="Close file inspection" onClick={onClose} />
      </header>
      {file && reference.line !== undefined && reference.line > lines.length && <p role="status" className="px-3 py-1 text-xs" style={{ color: 'var(--text-2)' }}>Line {reference.line} is past the end of this file.</p>}
      {changesError && <p role="alert" className="px-3 py-2 text-sm" style={{ color: 'var(--red)' }}>{changesError}</p>}
      {error ? <div role="alert" className="p-4 text-sm" style={{ color: 'var(--red)' }}>{error}</div> : !file ? <div role="status" className="p-4 text-sm" style={{ color: 'var(--text-2)' }}>Loading file…</div> :
        <div ref={viewportRef} role="region" aria-label={`Source of ${file.path}`} tabIndex={0} className="min-h-0 flex-1 overflow-auto"
          style={{ color: 'var(--text)', fontFamily: 'var(--font-mono)', fontSize: 'var(--fs-code, 12px)' }}
          onScroll={(event) => setViewport({ top: event.currentTarget.scrollTop, height: event.currentTarget.clientHeight })}>
          <div style={{ height: lines.length * ROW_HEIGHT, minWidth: `calc(${width}ch + 70px)`, position: 'relative' }}>
            <div style={{ position: 'absolute', top: start * ROW_HEIGHT, width: '100%' }}>
              {lines.slice(start, end).map((line, offset) => {
                const number = start + offset + 1
                const selected = reference.line !== undefined && number >= reference.line && number <= (reference.endLine ?? reference.line)
                return <div key={number} data-source-line={number} style={{ display: 'flex', height: ROW_HEIGHT, lineHeight: `${ROW_HEIGHT}px`, whiteSpace: 'pre', background: selected ? 'var(--accent-dim)' : 'transparent' }}>
                  <span aria-label="Line number" style={{ width: 52, paddingRight: 12, flexShrink: 0, textAlign: 'right', color: 'var(--text-2)', userSelect: 'none' }}>{number}</span><span style={{ paddingRight: 12, tabSize: 4 }}>{line || ' '}</span>
                </div>
              })}
            </div>
          </div>
        </div>}
    </section>
  </div>, document.body)
}
