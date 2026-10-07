import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { FileText, X } from 'lucide-react'
import type { VcsChangedFile, VcsType } from '../../store/useAppStore'
import { CopyTextButton } from '../chat/StatusHelpers'
import { IconButton } from '../ui'
import { getTopmostModal } from '../../utils/modalFocus'

export function RepositoryDiffDialog({
  chatId,
  file,
  vcsType,
  comparisonRef,
  getDiff,
  onClose,
}: {
  chatId: string
  file: VcsChangedFile
  vcsType: VcsType
  comparisonRef?: string
  getDiff: (chatId: string, path: string, vcsType: VcsType, comparisonRef?: string) => Promise<string>
  onClose: () => void
}) {
  const dialogRef = useRef<HTMLElement>(null)
  const [diff, setDiff] = useState<string | null>(file.binary ? '' : null)
  const [error, setError] = useState('')

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
    if (file.binary) return
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

  const lines = diff?.split('\n') ?? []

  return createPortal(
    <div
      className="fixed inset-0 z-[1000] flex items-center justify-center p-2 sm:p-4"
      style={{ background: 'rgba(0, 0, 0, 0.48)', backdropFilter: 'blur(3px)' }}
      onMouseDown={onClose}
    >
      <section
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={`Changes in ${file.path}`}
        tabIndex={-1}
        className="flex h-[calc(100dvh-1rem)] w-full max-w-5xl flex-col overflow-hidden rounded-lg sm:h-auto sm:max-h-[88vh]"
        style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="flex min-h-12 items-center gap-3 px-3 sm:px-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <FileText size={17} aria-hidden className="shrink-0" style={{ color: 'var(--accent)' }} />
          <div className="min-w-0 flex-1">
            <div className="truncate text-sm font-semibold" title={file.path} style={{ color: 'var(--text)' }}>{file.path}</div>
            <div className="flex gap-3 font-mono text-[11px]">
              <span style={{ color: 'var(--text-3)' }}>{file.status.trim() || 'M'}</span>
              {!file.binary && <><span style={{ color: 'var(--green)' }}>+{file.additions}</span><span style={{ color: 'var(--red)' }}>-{file.deletions}</span></>}
            </div>
          </div>
          {diff && <CopyTextButton text={diff} label="Copy diff" title="Copy file diff" />}
          <IconButton icon={<X size={16} />} label="Close file changes" onClick={onClose} />
        </header>
        <div className="min-h-0 flex-1 overflow-auto" style={{ background: 'var(--bg)' }}>
          {file.binary ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>Binary changes cannot be previewed.</div>
          ) : error ? (
            <div role="alert" className="m-4 rounded-md p-3 text-sm" style={{ border: '1px solid var(--red)', color: 'var(--red)' }}>{error}</div>
          ) : diff === null ? (
            <div role="status" className="p-6 text-center text-sm" style={{ color: 'var(--text-3)' }}>Loading file changes…</div>
          ) : diff.length === 0 ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>No textual diff is available for this file.</div>
          ) : (
            <pre className="min-w-full w-max py-2 font-mono text-[11px] leading-5 sm:text-xs" aria-label={`Unified diff for ${file.path}`}>
              {lines.map((line, index) => {
                const added = line.startsWith('+') && !line.startsWith('+++')
                const removed = line.startsWith('-') && !line.startsWith('---')
                const hunk = line.startsWith('@@')
                const header = line.startsWith('diff ') || line.startsWith('index ') || line.startsWith('---') || line.startsWith('+++')
                return (
                  <span
                    key={`${index}-${line}`}
                    className="block min-h-5 whitespace-pre px-3 sm:px-4"
                    style={{
                      color: added ? 'var(--green)' : removed ? 'var(--red)' : hunk ? 'var(--accent)' : header ? 'var(--text-2)' : 'var(--text)',
                      background: added
                        ? 'color-mix(in srgb, var(--green) 9%, transparent)'
                        : removed
                          ? 'color-mix(in srgb, var(--red) 9%, transparent)'
                          : hunk
                            ? 'var(--accent-dim)'
                            : 'transparent',
                    }}
                  >
                    {line || ' '}
                  </span>
                )
              })}
            </pre>
          )}
        </div>
      </section>
    </div>,
    document.body
  )
}
