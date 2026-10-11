import { useEffect, useRef, useState } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { parseStartupNativeImportPage, type StartupNativeImportPage, type StartupNativeImportStatus } from '../../types/startupNativeImports'
import { getTopmostModal, trapModalTab } from '../../utils/modalFocus'
import { providerMetadataForId } from '../../utils/providerMetadata'
import { Button, IconButton, Overlay } from '../ui'
import '../shared/review.css'

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
  function publicationFailure(data: StartupNativeImportPage): string {
    const pending = publication.current
    if (data.importedCount - pending.importedCount >= pending.expectedCount) return ''
    if (data.totalCount === 0) return 'Some selected chats could not be imported. They are no longer available.'
    return 'Some selected chats could not be imported. Review the remaining history to retry.'
  }
  async function load(nextOffset: number, afterPublication = false) {
    if (busyRef.current) return
    const request = ++requestSequence.current
    busyRef.current = true; setBusy(true); setBusyAction('load'); setError('')
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
        if (failure) setError(failure)
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
    setBusy(false); setBusyAction(null); setPage(null); setOffset(0); setSelected([]); setError('')
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
    busyRef.current = true; setBusy(true); setBusyAction('resolve'); setError('')
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
        if (failure) setError(failure)
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
  return <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="Import chats" aria-busy={busy} className="uam-rv-dialog w-full max-w-2xl max-h-[85vh] text-xs">
      <header className="uam-rv-dialog__header">
        <div className="min-w-0 flex-1">
          <h2 className="uam-rv-title">Import chats</h2>
          <p className="uam-rv-muted">Choose chats to add to UAM. Shift-click to select a range.</p>
        </div>
        <IconButton icon={<X size={16} />} label="Close import chats" disabled={busy} onClick={onClose} />
      </header>
      <div className="uam-rv-dialog__body">
        {candidates.length > 0 && <div className="flex items-center gap-3 px-4 py-2" style={{ borderBottom: '1px solid var(--border)' }}>
          <input type="checkbox" className="uam-rv-check" aria-label="Select all chats on this page" checked={allOnPage} disabled={busy || importing} onChange={(event) => selectPage(event.target.checked)} />
          <span style={{ color: 'var(--text-2)' }}>Select page</span>
          <span className="ml-auto" style={{ color: 'var(--text-3)' }}>{candidates.length} on this page</span>
        </div>}
        <div className="uam-rv-list" aria-label="Pending chats">
          {candidates.map((candidate) => <label key={candidate.candidateId} className="uam-rv-row" data-selected={selected.includes(candidate.candidateId)}>
            <input className="uam-rv-check" type="checkbox" aria-label={`Select ${candidate.title}`} disabled={busy || importing} checked={selected.includes(candidate.candidateId)} onChange={(event) => selectCandidate(candidate.candidateId, event.target.checked, 'shiftKey' in event.nativeEvent && event.nativeEvent.shiftKey === true)} />
            <span className="min-w-0 flex-1"><span className="uam-rv-row__title">{candidate.title}</span><span className="uam-rv-row__meta" style={{ color: 'var(--text-3)' }}>{providerMetadataForId(candidate.providerId).name}{candidate.executionHostId && candidate.executionHostId !== 'local' ? ` · ${candidate.executionHostId}` : ''} · {candidate.workspaceDirectory}</span></span>
          </label>)}
        </div>
        <div className="flex flex-col gap-2 px-4 py-2">
          {!busy && !importing && page && candidates.length === 0 && <p style={{ color: 'var(--text-2)' }}>No pending chats on this page{folderId ? ' for this workspace' : ''}.</p>}
          {busy && <p role="status" style={{ color: 'var(--text-3)' }}>{busyAction === 'resolve' ? 'Saving…' : 'Loading history…'}</p>}
          {importing && !busy && <>
            <p role="status" style={{ color: 'var(--text-2)' }}>{batch.expectedCount > 0 ? `Importing ${batchDone} of ${batch.expectedCount} chats…` : 'Importing selected chats…'}</p>
            <div role="progressbar" aria-label="Import progress" aria-valuemin={0} aria-valuemax={batch.expectedCount || undefined} aria-valuenow={batch.expectedCount ? batchDone : undefined} className="uam-rv-progress">
              <span style={{ transform: `scaleX(${batch.expectedCount > 0 ? batchDone / batch.expectedCount : 0.05})` }} />
            </div>
          </>}
          {error && <p role="alert" className="uam-rv-alert" style={{ background: 'var(--error-dim)', color: 'var(--error)' }}>{error}</p>}
        </div>
        <div className="uam-rv-summary">
          {page ? <p role="status" className="min-w-0 flex-1 m-0">Page {pageNumber} of {pageCount} · {page.totalCount} pending · {selected.length} selected</p> : <span className="flex-1" />}
        </div>
        <div className="uam-rv-actions">
          <Button type="button" size="sm" variant="primary" disabled={busy || importing || selected.length === 0} onClick={() => void decide(true)}>Import selected</Button>
          <Button type="button" size="sm" disabled={busy || importing || !page?.totalCount} onClick={() => void decide(true, true)}>Import all</Button>
          <Button type="button" size="sm" variant="ghost" disabled={busy || importing || selected.length === 0} onClick={() => void decide(false)}>Ignore selected</Button>
          <span className="uam-rv-actions__spacer" />
          {error && <Button type="button" size="sm" disabled={busy} onClick={() => void load(offset, awaitingPublication)}>Reload</Button>}
          <Button type="button" size="sm" variant="ghost" disabled={busy || importing || offset === 0} onClick={() => void load(Math.max(0, offset - 100))}>Previous</Button>
          <Button type="button" size="sm" variant="ghost" disabled={busy || importing || !page || page.nextOffset >= page.totalCount} onClick={() => page && void load(page.nextOffset)}>Next</Button>
        </div>
      </div>
    </div>
  </Overlay>
}
