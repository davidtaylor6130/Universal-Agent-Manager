import { useEffect, useRef, useState } from 'react'
import { Download, EyeOff, Loader2, RefreshCw, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { parseStartupNativeImportPage, type StartupNativeImportPage, type StartupNativeImportStatus } from '../../types/startupNativeImports'
import { getTopmostModal, trapModalTab } from '../../utils/modalFocus'
import { providerMetadataForId } from '../../utils/providerMetadata'
import { Button, IconButton, Overlay, Tooltip } from '../ui'
import { ProviderLogo } from '../shared/ProviderLogo'
import '../shared/review.css'

function lastDirectoryName(path: string): string {
  return path.split(/[\\/]/).filter(Boolean).pop() ?? path
}

/** Reviews only cached startup metadata. Dismissal never records a native decision. */
export function StartupNativeImportChooser() {
  const status = useAppStore((state) => state.startupNativeImports)
  const sequence = useAppStore((state) => state.startupNativeImportReviewSequence)
  const folderId = useAppStore((state) => state.startupNativeImportFolderId)
  const seen = useRef(new Set<string>())
  const reviewedSequence = useRef(sequence)
  const [review, setReview] = useState<{ generation: string; sequence: number; folderId: string | null } | null>(null)
  useEffect(() => {
    if (isCompanionContext() || !status.generation) return
    const explicit = reviewedSequence.current !== sequence
    reviewedSequence.current = sequence
    if (explicit || (status.phase === 'ready' && status.totalCount > 0 && !seen.current.has(status.generation))) {
      seen.current.add(status.generation)
      setReview({ generation: status.generation, sequence, folderId: explicit ? folderId : null })
    }
  }, [status.generation, status.phase, status.totalCount, sequence, folderId])
  if (!review || review.generation !== status.generation) return null
  return <ImportReview key={`${review.generation}:${review.sequence}`} {...review} onClose={() => setReview(null)} />
}

function ImportReview({ generation, folderId, onClose }: { generation: string; folderId: string | null; onClose: () => void }) {
  const status = useAppStore((state) => state.startupNativeImports)
  const [awaitingPublication, setAwaitingPublication] = useState(false)
  const publication = useRef({ start: status, refreshed: null as StartupNativeImportStatus | null, importedCount: 0, expectedCount: 0 })
  const [page, setPage] = useState<StartupNativeImportPage | null>(null)
  const [offset, setOffset] = useState(0)
  const [selected, setSelected] = useState<string[]>([])
  const selectionAnchor = useRef<string | null>(null)
  const [busy, setBusy] = useState(false)
  const [busyAction, setBusyAction] = useState<'load' | 'resolve' | null>(null)
  const [error, setError] = useState('')
  // Set when a started import finishes short of its expected count.
  const [outcome, setOutcome] = useState<{ failed: number; total: number } | null>(null)
  const requestSequence = useRef(0)
  const loadedFolderId = useRef(folderId)
  const busyRef = useRef(false)
  const mounted = useRef(false)
  const panel = useRef<HTMLDivElement>(null)
  const closeRef = useRef(onClose); closeRef.current = onClose
  const owns = (request: number) => mounted.current && requestSequence.current === request && useAppStore.getState().startupNativeImports.generation === generation
  useEffect(() => {
    mounted.current = true
    const previous = document.activeElement as HTMLElement | null
    const dialog = panel.current
    dialog?.focus()
    const key = (event: KeyboardEvent) => {
      if (event.defaultPrevented || getTopmostModal() !== panel.current) return
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); if (!busyRef.current) closeRef.current() }
      else trapModalTab(event)
    }
    window.addEventListener('keydown', key, true)
    return () => {
      mounted.current = false; ++requestSequence.current
      busyRef.current = false
      window.removeEventListener('keydown', key, true)
      if (previous?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) previous.focus()
    }
  }, [])
  function publicationFailure(data: StartupNativeImportPage): { message: string; failed: number; total: number } | null {
    const pending = publication.current
    const imported = data.importedCount - pending.importedCount
    if (imported >= pending.expectedCount) return null
    const total = pending.expectedCount
    const failed = total - Math.max(0, imported)
    const gone = data.totalCount === 0
    if (failed >= total) return { message: `None of the selected chats could be imported.${gone ? ' They are no longer available.' : ''}`, failed, total }
    return { message: `${failed} ${failed === 1 ? 'chat' : 'chats'} couldn't be imported. ${gone ? 'They are no longer available.' : 'They stay in the list so you can retry.'}`, failed, total }
  }
  function reportFailure(failure: ReturnType<typeof publicationFailure>) {
    setOutcome(failure ? { failed: failure.failed, total: failure.total } : null)
    setError(failure?.message ?? '')
  }
  async function load(nextOffset: number, afterPublication = false) {
    if (busyRef.current) return
    const request = ++requestSequence.current
    busyRef.current = true; setBusy(true); setBusyAction('load'); setError(''); setOutcome(null)
    try {
      const response = await sendToCEF<unknown>({ action: 'getStartupNativeImports', payload: { offset: nextOffset, limit: 100, folderId } })
      if (!owns(request)) return
      if (!response.ok || !response.data) throw new Error(response.error || 'History could not be loaded.')
      const data = parseStartupNativeImportPage(response.data)
      if (!data) throw new Error('Invalid history response. Reload to try again.')
      if (data.generation !== generation) throw new Error('History changed. Open the review again.')
      data.errors.forEach((message) => console.warn(`[Startup history] ${message}`))
      setPage(data); setOffset(nextOffset); setSelected([]); selectionAnchor.current = null
      if (data.phase === 'importing') {
        if (publication.current.expectedCount === 0) publication.current.start = status
        setAwaitingPublication(true)
      }
      if (afterPublication && data.phase !== 'importing') {
        setAwaitingPublication(false)
        const failure = publicationFailure(data)
        reportFailure(failure)
        publication.current = { start: status, refreshed: null, importedCount: data.importedCount, expectedCount: 0 }
        if (!failure && data.phase === 'complete' && data.totalCount === 0) closeRef.current()
      }
    } catch (failure) { if (owns(request)) setError(failure instanceof Error ? failure.message : 'History could not be loaded.') }
    finally { if (owns(request)) { busyRef.current = false; setBusy(false); setBusyAction(null) } }
  }
  useEffect(() => { void load(0) }, [])
  useEffect(() => {
    if (loadedFolderId.current === folderId) return
    loadedFolderId.current = folderId
    ++requestSequence.current
    busyRef.current = false
    setBusy(false); setBusyAction(null); setPage(null); setOffset(0); setSelected([]); setError(''); setOutcome(null)
    void load(0)
  }, [folderId])
  useEffect(() => {
    if (status.phase === 'importing' && publication.current.expectedCount > 0) setAwaitingPublication(true)
    if (!awaitingPublication || busy || !['ready', 'complete'].includes(status.phase)) return
    const pending = publication.current
    // Native pushes can coalesce importing into ready with unchanged counts.
    // Each supplied status object is a new update, even when its values match.
    if (status === pending.start || status === pending.refreshed) return
    pending.refreshed = status
    void load(0, true)
  }, [awaitingPublication, busy, status])
  async function decide(importSelected: boolean, importAll = false) {
    if (busyRef.current || awaitingPublication || page?.phase === 'importing' || status.phase === 'importing' || (importAll ? !page?.totalCount : selected.length === 0)) return
    const request = ++requestSequence.current
    const ids = selected.slice(0, 200)
    let resolutionMayHaveStarted = false
    if (importSelected) publication.current = { start: status, refreshed: null, importedCount: status.importedCount, expectedCount: importAll ? page!.totalCount : ids.length }
    busyRef.current = true; setBusy(true); setBusyAction('resolve'); setError(''); setOutcome(null)
    try {
      const response = await sendToCEF<unknown>({ action: 'resolveStartupNativeImports', payload: { generation, selectedIds: importSelected && !importAll ? ids : [], declinedIds: importSelected ? [] : ids, folderId, ...(importAll ? { importAll: true } : {}) } })
      if (!owns(request)) return
      if (!response.ok || !response.data) {
        resolutionMayHaveStarted = Boolean(response.error?.includes('may have reached UAM'))
        throw new Error(response.error || 'History decisions could not be saved.')
      }
      resolutionMayHaveStarted = true
      const data = parseStartupNativeImportPage(response.data)
      if (!data) throw new Error('Invalid history response. Reload to try again.')
      if (data.generation !== generation) throw new Error('History changed. Open the review again.')
      data.errors.forEach((message) => console.warn(`[Startup history] ${message}`))
      setPage(data); setOffset(0); setSelected([])
      if (importSelected && data.phase === 'complete') {
        setAwaitingPublication(false)
        const failure = publicationFailure(data)
        reportFailure(failure)
        publication.current = { start: status, refreshed: null, importedCount: data.importedCount, expectedCount: 0 }
        if (data.phase === 'complete' && data.totalCount === 0 && !failure) closeRef.current()
      }
      else if (importSelected) setAwaitingPublication(true)
      if (!importSelected && data.phase === 'complete' && data.totalCount === 0) closeRef.current()
    } catch (failure) {
      if (owns(request)) {
        if (importSelected) {
          publication.current = { start: status, refreshed: null, importedCount: status.importedCount, expectedCount: 0 }
          setAwaitingPublication(resolutionMayHaveStarted)
        }
        setError(failure instanceof Error ? failure.message : 'History decisions could not be saved.')
      }
    }
    finally { if (owns(request)) { busyRef.current = false; setBusy(false); setBusyAction(null) } }
  }
  function selectCandidate(candidateId: string, checked: boolean, shift: boolean) {
    const candidates = page?.candidates ?? []
    const anchor = candidates.findIndex((candidate) => candidate.candidateId === selectionAnchor.current)
    const target = candidates.findIndex((candidate) => candidate.candidateId === candidateId)
    const ids = shift && anchor >= 0 && target >= 0
      ? candidates.slice(Math.min(anchor, target), Math.max(anchor, target) + 1).map((candidate) => candidate.candidateId)
      : [candidateId]
    if (!shift || anchor < 0) selectionAnchor.current = candidateId
    setSelected((current) => checked ? [...new Set([...current, ...ids])] : current.filter((id) => !ids.includes(id)))
  }
  const importing = awaitingPublication || page?.phase === 'importing' || status.phase === 'importing'
  const candidates = page?.candidates ?? []
  const pageIds = candidates.map((candidate) => candidate.candidateId)
  const allOnPage = pageIds.length > 0 && pageIds.every((id) => selected.includes(id))
  const selectPage = (checked: boolean) => {
    setSelected((current) => checked ? [...new Set([...current, ...pageIds])] : current.filter((id) => !pageIds.includes(id)))
    selectionAnchor.current = null
  }
  const pageNumber = Math.floor(offset / 100) + 1
  const pageCount = Math.max(1, Math.ceil((page?.totalCount ?? 0) / 100))
  // Progress counts only this batch: expectedCount is set when the user starts the import.
  const batch = publication.current
  const batchDone = Math.max(0, Math.min(batch.expectedCount, status.importedCount - batch.importedCount))
  const partial = outcome !== null && outcome.failed < outcome.total
  return <Overlay open zIndex={50} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="Import chats" aria-busy={busy} className="uam-rv-dialog w-full max-w-2xl max-h-[85vh] text-xs">
      <header className="uam-rv-dialog__header">
        <div className="min-w-0 flex-1">
          <h2 className="uam-rv-title">Import chats from other tools</h2>
          <p className="uam-rv-muted">UAM found chats it hasn't imported yet. Select the ones you want.</p>
        </div>
        <IconButton icon={<X size={16} />} label="Close import chats" disabled={busy} onClick={onClose} />
      </header>
      <div className="uam-rv-dialog__body">
        {candidates.length > 0 && <div className="uam-rv-selectbar">
          <label className="uam-rv-selectall">
            <input type="checkbox" className="uam-rv-check" aria-label="Select all chats on this page" checked={allOnPage} disabled={busy || importing} onChange={(event) => selectPage(event.target.checked)} />
            <span>Select all</span>
          </label>
          <span className="uam-rv-muted">{selected.length} selected</span>
        </div>}
        <div className="uam-rv-list" aria-label="Pending chats">
          {candidates.map((candidate) => {
            const providerName = providerMetadataForId(candidate.providerId).name
            const host = candidate.executionHostId && candidate.executionHostId !== 'local' ? candidate.executionHostId : ''
            const providerLabel = host ? `${providerName} · ${host}` : providerName
            return <label key={candidate.candidateId} className="uam-rv-row" data-selected={selected.includes(candidate.candidateId)}>
              <input className="uam-rv-check" type="checkbox" aria-label={`Select ${candidate.title}`} disabled={busy || importing} checked={selected.includes(candidate.candidateId)} onChange={(event) => selectCandidate(candidate.candidateId, event.target.checked, 'shiftKey' in event.nativeEvent && event.nativeEvent.shiftKey === true)} />
              <span className="uam-rv-row__title">{candidate.title}</span>
              <Tooltip label={providerLabel}>
                <span role="img" aria-label={providerLabel} className="uam-rv-row__provider"><ProviderLogo providerId={candidate.providerId} size={14} /></span>
              </Tooltip>
              <Tooltip label={candidate.workspaceDirectory}>
                <span className="uam-rv-row__meta" style={{ color: 'var(--text-3)' }}>{lastDirectoryName(candidate.workspaceDirectory)}</span>
              </Tooltip>
            </label>
          })}
        </div>
        <div className="flex flex-col gap-2 px-4 py-2">
          {!busy && !importing && page && candidates.length === 0 && <p style={{ color: 'var(--text-2)' }}>No pending chats on this page{folderId ? ' for this workspace' : ''}.</p>}
          {busy && !importing && <p role="status" style={{ color: 'var(--text-3)' }}>{busyAction === 'resolve' ? 'Saving…' : 'Loading history…'}</p>}
          {importing && <div className="flex flex-col gap-1">
            <div className="uam-rv-importing">
              <Loader2 size={14} className="uam-rv-spin" aria-hidden style={{ color: 'var(--text-2)' }} />
              <p role="status" className="m-0" style={{ color: 'var(--text-2)' }}>{batch.expectedCount > 0 ? `Importing ${batchDone} of ${batch.expectedCount}…` : 'Importing selected chats…'}</p>
            </div>
            <div role="progressbar" aria-label="Import progress" aria-valuemin={0} aria-valuemax={batch.expectedCount || undefined} aria-valuenow={batch.expectedCount ? batchDone : undefined} className={batch.expectedCount > 0 ? 'uam-rv-progress' : 'uam-rv-progress uam-rv-progress--indeterminate'}>
              <span style={batch.expectedCount > 0 ? { transform: `scaleX(${batchDone / batch.expectedCount})` } : undefined} />
            </div>
          </div>}
          {error && <p role="alert" className="uam-rv-alert" style={outcome ? { background: partial ? 'var(--warning-dim)' : 'var(--error-dim)', color: partial ? 'var(--warning)' : 'var(--error)' } : { background: 'var(--error-dim)', color: 'var(--error)' }}>
            {error}
            {outcome && outcome.failed > 0 && <strong className="uam-rv-alert__count">{outcome.failed} failed</strong>}
          </p>}
        </div>
        <div className="uam-rv-actions">
          <Button type="button" size="sm" variant="primary" leadingIcon={<Download size={14} />} aria-label="Import selected" disabled={busy || importing || selected.length === 0} onClick={() => void decide(true)}>Import</Button>
          <IconButton size="sm" icon={<EyeOff size={14} />} label="Ignore selected" disabled={busy || importing || selected.length === 0} onClick={() => void decide(false)} />
          <span className="uam-rv-actions__spacer" />
          {error && <IconButton size="sm" icon={<RefreshCw size={14} />} label="Reload" disabled={busy} onClick={() => void load(offset, awaitingPublication)} />}
          {page && pageCount > 1 && <>
            <Button type="button" size="sm" variant="ghost" disabled={busy || importing || offset === 0} onClick={() => void load(Math.max(0, offset - 100))}>Previous</Button>
            <span className="uam-rv-muted">Page {pageNumber} of {pageCount}</span>
            <Button type="button" size="sm" variant="ghost" disabled={busy || importing || page.nextOffset >= page.totalCount} onClick={() => void load(page.nextOffset)}>Next</Button>
          </>}
        </div>
      </div>
    </div>
  </Overlay>
}
