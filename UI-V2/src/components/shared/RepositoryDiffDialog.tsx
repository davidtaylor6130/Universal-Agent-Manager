import { FilePreviewDialog } from './FilePreviewDialog'
import { useEffect, useRef, useState } from 'react'
import { Columns2, FileText, Rows3, X } from 'lucide-react'
import type { VcsChangedFile, VcsType } from '../../store/useAppStore'
import { CopyTextButton } from '../chat/StatusHelpers'
import { Button, IconButton, Overlay } from '../ui'
import './review.css'
import { UnifiedDiff } from './UnifiedDiff'
import { getTopmostModal } from '../../utils/modalFocus'

export function RepositoryDiffDialog({
  chatId,
  file,
  vcsType,
  comparisonRef,
  getDiff,
  onClose,
  sourcePath,
  comparisonLabel,
  zIndex = 1000,
  showFileStats = true,
}: {
  chatId: string
  file: VcsChangedFile
  vcsType: VcsType
  comparisonRef?: string
  getDiff: (chatId: string, path: string, vcsType: VcsType, comparisonRef?: string) => Promise<string>
  onClose: () => void
  sourcePath?: string
  comparisonLabel?: string
  zIndex?: number
  showFileStats?: boolean
}) {
  const dialogRef = useRef<HTMLElement>(null)
  const [diff, setDiff] = useState<string | null>(file.binary ? '' : null)
  const [error, setError] = useState('')
  const [inspection, setInspection] = useState<{ chatId: string; path: string } | null>(null)
  const [layout, setLayout] = useState<'unified' | 'split'>(() => {
    try { return localStorage.getItem('uam-diff-layout') === 'unified' ? 'unified' : 'split' } catch { return 'split' }
  })
  const chooseLayout = (next: 'unified' | 'split') => {
    setLayout(next)
    try { localStorage.setItem('uam-diff-layout', next) } catch { /* The view still works when storage is unavailable. */ }
  }

  useEffect(() => {
    const previouslyFocused = document.activeElement as HTMLElement | null
    const dialog = dialogRef.current
    dialog?.focus()
    return () => {
      if (previouslyFocused?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) previouslyFocused.focus()
    }
  }, [])

  useEffect(() => {
    const closeOnEscape = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape' || event.defaultPrevented || getTopmostModal() !== dialogRef.current) return
      event.preventDefault()
      onClose()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [onClose])

  useEffect(() => {
    if (file.binary) {
      setDiff('')
      setError('')
      return
    }
    let cancelled = false
    setDiff(null)
    setError('')
    void (comparisonRef ? getDiff(chatId, file.path, vcsType, comparisonRef) : getDiff(chatId, file.path, vcsType))
      .then((nextDiff) => {
        if (!cancelled) setDiff(nextDiff)
      })
      .catch((reason) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Failed to load this diff.')
      })
    return () => { cancelled = true }
  }, [chatId, comparisonRef, file.binary, file.path, getDiff, vcsType])


  return <>
    <Overlay open zIndex={zIndex} onBackdropMouseDown={onClose}>
      <section
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={`Changes in ${file.path}`}
        tabIndex={-1}
        className="uam-rv-dialog flex h-[calc(100dvh-1rem)] w-full max-w-5xl sm:h-auto sm:max-h-[88vh]"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="uam-rv-dialog__header">
          <FileText size={16} aria-hidden className="shrink-0" style={{ color: 'var(--accent)' }} />
          <div className="min-w-0 flex-1">
            <div className="truncate text-sm font-semibold" title={file.path} style={{ color: 'var(--text)' }}>{file.path}</div>
            {(comparisonLabel || comparisonRef) && <div className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{comparisonLabel || `Against ${comparisonRef}`}</div>}
            {showFileStats && <div className="uam-rv-stat mt-0.5">
              <span style={{ color: 'var(--text-3)' }}>{file.status.trim() || 'M'}</span>
              {!file.binary && <><span style={{ color: 'var(--success)' }}>+{file.additions}</span><span style={{ color: 'var(--error)' }}>-{file.deletions}</span></>}
            </div>}
          </div>
          <Button type="button" size="sm" variant="ghost" onClick={() => setInspection({ chatId, path: sourcePath ?? file.path })}>Inspect file</Button>
          {!file.binary && <div role="group" aria-label="Diff layout" className="uam-rv-segmented">
            <button type="button" aria-label="Unified diff view" aria-pressed={layout === 'unified'} title="Unified diff view" onClick={() => chooseLayout('unified')}><Rows3 size={14} aria-hidden />Unified</button>
            <button type="button" aria-label="Split diff view" aria-pressed={layout === 'split'} title="Split diff view" onClick={() => chooseLayout('split')}><Columns2 size={14} aria-hidden />Split</button>
          </div>}
          {diff && <CopyTextButton text={diff} label="Copy diff" title="Copy file diff" />}
          <IconButton icon={<X size={16} />} label="Close file changes" onClick={onClose} />
        </header>
        <div className="min-h-0 flex-1 flex flex-col overflow-auto" style={{ background: 'var(--bg)' }}>
          {file.binary ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>Binary changes cannot be previewed.</div>
          ) : error ? (
            <div role="alert" className="uam-rv-alert m-4" style={{ background: 'var(--error-dim)', color: 'var(--error)' }}>{error}</div>
          ) : diff === null ? (
            <div role="status" className="p-6 text-center text-sm" style={{ color: 'var(--text-3)' }}>Loading file changes…</div>
          ) : diff.length === 0 ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>No textual diff is available for this file.</div>
          ) : (
            <UnifiedDiff key={`${chatId}:${file.path}:${comparisonRef ?? ""}`} diff={diff} path={file.path} layout={layout} />
          )}
        </div>
      </section>
    </Overlay>
  {inspection?.chatId === chatId && inspection.path === (sourcePath ?? file.path) && <FilePreviewDialog zIndex={zIndex + 100} chatId={chatId} reference={{ path: inspection.path }} onClose={() => setInspection(null)} />}</>
}
