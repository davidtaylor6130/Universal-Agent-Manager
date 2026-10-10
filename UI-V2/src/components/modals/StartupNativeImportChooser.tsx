import { useEffect, useRef, useState } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { parseStartupNativeImportPage, type StartupNativeImportPage, type StartupNativeImportStatus } from '../../types/startupNativeImports'
import { getTopmostModal, trapModalTab } from '../../utils/modalFocus'
import { providerMetadataForId } from '../../utils/providerMetadata'
import { Button, IconButton, Overlay } from '../ui'

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
  const publication = useRef({ start: status, refreshed: null as StartupNativeImportStatus | null, importedCount: 0, selectedIds: [] as string[] })
  const [page, setPage] = useState<StartupNativeImportPage | null>(null)
  const [offset, setOffset] = useState(0)
  const [selected, setSelected] = useState<string[]>([])
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
    if (data.importedCount - pending.importedCount >= pending.selectedIds.length) return ''
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
      setPage(data); setOffset(nextOffset); setSelected([])
      if (data.phase === 'importing') {
        if (publication.current.selectedIds.length === 0) publication.current.start = status
        setAwaitingPublication(true)
      }
      if (afterPublication && data.phase !== 'importing') {
        setAwaitingPublication(false)
        const failure = publicationFailure(data)
        if (failure) setError(failure)
        publication.current = { start: status, refreshed: null, importedCount: data.importedCount, selectedIds: [] }
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
    if (status.phase === 'importing' && publication.current.selectedIds.length > 0) setAwaitingPublication(true)
    if (!awaitingPublication || busy || !['ready', 'complete'].includes(status.phase)) return
    const pending = publication.current
    // Native pushes can coalesce importing into ready with unchanged counts.
    // Each supplied status object is a new update, even when its values match.
    if (status === pending.start || status === pending.refreshed) return
    pending.refreshed = status
    void load(0, true)
  }, [awaitingPublication, busy, status])
  async function decide(importSelected: boolean) {
    if (busyRef.current || awaitingPublication || page?.phase === 'importing' || status.phase === 'importing' || selected.length === 0) return
    const request = ++requestSequence.current
    const ids = selected.slice(0, 200)
    let resolutionMayHaveStarted = false
    if (importSelected) publication.current = { start: status, refreshed: null, importedCount: status.importedCount, selectedIds: ids }
    busyRef.current = true; setBusy(true); setBusyAction('resolve'); setError('')
    try {
      const response = await sendToCEF<unknown>({ action: 'resolveStartupNativeImports', payload: { generation, selectedIds: importSelected ? ids : [], declinedIds: importSelected ? [] : ids, folderId } })
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
        publication.current = { start: status, refreshed: null, importedCount: data.importedCount, selectedIds: [] }
        if (data.phase === 'complete' && data.totalCount === 0 && !failure) closeRef.current()
      }
      else if (importSelected) setAwaitingPublication(true)
      if (!importSelected && data.phase === 'complete' && data.totalCount === 0) closeRef.current()
    } catch (failure) {
      if (owns(request)) {
        if (importSelected) {
          publication.current = { start: status, refreshed: null, importedCount: status.importedCount, selectedIds: [] }
          setAwaitingPublication(resolutionMayHaveStarted)
        }
        setError(failure instanceof Error ? failure.message : 'History decisions could not be saved.')
      }
    }
    finally { if (owns(request)) { busyRef.current = false; setBusy(false); setBusyAction(null) } }
  }
  const importing = awaitingPublication || page?.phase === 'importing' || status.phase === 'importing'
  const candidates = page?.candidates ?? []
  return <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="Import chats" aria-busy={busy} className="w-full max-w-2xl max-h-[85vh] overflow-y-auto bg-black text-white border border-[var(--border)] p-4 text-xs">
      <div className="flex justify-between items-center mb-3"><strong>Import chats</strong><IconButton icon={<X size={16} />} label="Close import chats" disabled={busy} onClick={onClose} /></div>
      <p className="mb-2">Choose chats to add to UAM.</p>
      {candidates.map((candidate) => <label key={candidate.candidateId} className="flex gap-2 items-start border-t border-[var(--border)] py-2">
        <input type="checkbox" aria-label={`Select ${candidate.title}`} disabled={busy || importing} checked={selected.includes(candidate.candidateId)} onChange={(event) => setSelected((current) => event.target.checked ? [...current, candidate.candidateId] : current.filter((id) => id !== candidate.candidateId))} />
        <span className="min-w-0"><span className="block truncate">{candidate.title}</span><span className="block break-all" style={{ color: 'var(--text-3)' }}>{providerMetadataForId(candidate.providerId).name}{candidate.executionHostId && candidate.executionHostId !== 'local' ? ` · ${candidate.executionHostId}` : ''} · {candidate.workspaceDirectory}</span></span>
      </label>)}
      {!busy && !importing && page && candidates.length === 0 && <p>No pending chats on this page{folderId ? ' for this workspace' : ''}.</p>}
      {busy && <p role="status" style={{ color: 'var(--text-3)' }}>{busyAction === 'resolve' ? 'Saving…' : 'Loading history…'}</p>}
      {importing && !busy && <p role="status" style={{ color: 'var(--text-3)' }}>Importing selected chats…</p>}
      {error && <p role="alert" className="py-2" style={{ color: 'var(--red)' }}>{error}</p>}
      <div className="flex flex-wrap gap-2 mt-3">
        <Button type="button" size="sm" variant="primary" disabled={busy || importing || selected.length === 0} onClick={() => void decide(true)}>Import selected</Button>
        <Button type="button" size="sm" disabled={busy || importing || selected.length === 0} onClick={() => void decide(false)}>Ignore selected</Button>
        <Button type="button" size="sm" disabled={busy || importing || offset === 0} onClick={() => void load(Math.max(0, offset - 100))}>Previous</Button>
        <Button type="button" size="sm" disabled={busy || importing || !page || page.nextOffset >= page.totalCount} onClick={() => page && void load(page.nextOffset)}>Next</Button>
        {error && <Button type="button" size="sm" disabled={busy} onClick={() => void load(offset, awaitingPublication)}>Reload</Button>}
      </div>
    </div>
  </Overlay>
}
