import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Button, IconButton } from '../ui'
import { MarkdownContent } from '../markdown/Markdown'
import { ProviderLogo } from '../shared/ProviderLogo'
import { SelectionGrid } from '../shared/SelectionGrid'
import { normalizeCliProviderIdAlias, providerMetadataForId } from '../../utils/providerMetadata'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../../types/markdownStore'
import { Download, ExternalLink, FileInput, Folder, FolderInput, FolderOpen, Paperclip, Pencil, Pin, Plus, RefreshCw, Search, X } from 'lucide-react'

const EMPTY_DRAFT: MarkdownStoreDraft = { title: '', maker: '', review: '', body: '', group: '' }
const PROVIDER_OPTIONS = ['gemini-cli', 'codex-cli', 'claude-cli', 'opencode-cli', 'copilot-cli'].map((id) => ({
  id, label: providerMetadataForId(id).name, icon: <ProviderLogo providerId={id} size={24} />,
}))

/** Skills uses the same store actions in Settings and in the standalone library. */
export function MarkdownStoreModal({ embedded = false }: { embedded?: boolean } = {}) {
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
  const entries = useAppStore(useShallow((s) => s.markdownStoreEntries))
  const loading = useAppStore((s) => s.markdownStoreLoading)
  const error = useAppStore((s) => s.markdownStoreError)
  const clearError = useAppStore((s) => s.clearMarkdownStoreError)
  const close = useAppStore((s) => s.closeMarkdownStore)
  const refresh = useAppStore((s) => s.refreshMarkdownStore)
  const browseDirectory = useAppStore((s) => s.browseMarkdownStoreDirectory)
  const setDirectory = useAppStore((s) => s.setMarkdownStoreDirectory)
  const createEntry = useAppStore((s) => s.createMarkdownStoreEntry)
  const updateEntry = useAppStore((s) => s.updateMarkdownStoreEntry)
  const setFavorite = useAppStore((s) => s.setMarkdownStoreFavorite)
  const browseImport = useAppStore((s) => s.browseMarkdownStoreImport)
  const previewImports = useAppStore((s) => s.previewMarkdownStoreImports)
  const importEntries = useAppStore((s) => s.importMarkdownStoreEntries)
  const revealEntry = useAppStore((s) => s.revealMarkdownStoreEntry)
  const editEntry = useAppStore((s) => s.editMarkdownStoreEntry)
  const attachEntry = useAppStore((s) => s.attachMarkdownStoreEntry)
  const [search, setSearch] = useState('')
  const [filter, setFilter] = useState('all')
  const [selectedPath, setSelectedPath] = useState('')
  const [editing, setEditing] = useState<'new' | 'existing' | null>(null)
  const [draft, setDraft] = useState<MarkdownStoreDraft>(EMPTY_DRAFT)
  const [showEditorPreview, setShowEditorPreview] = useState(false)
  const [confirmation, setConfirmation] = useState<'discard' | 'overwrite' | null>(null)
  const [submitting, setSubmitting] = useState(false)
  const submittingRef = useRef(false)
  const initialDraftRef = useRef(EMPTY_DRAFT)
  const editingEntryRef = useRef<MarkdownStoreEntry | null>(null)
  const draftDirty = JSON.stringify(draft) !== JSON.stringify(initialDraftRef.current)
  const [addView, setAddView] = useState<'choose' | 'provider' | null>(null)
  const [importProvider, setImportProvider] = useState('')
  const [previewing, setPreviewing] = useState(false)
  const [importNotice, setImportNotice] = useState('')
  const [imports, setImports] = useState<MarkdownStoreImportCandidate[]>([])
  const [selectedImports, setSelectedImports] = useState<Set<string>>(new Set())
  const [conflicts, setConflicts] = useState<Record<string, MarkdownStoreConflictAction>>({})
  const [importResults, setImportResults] = useState<MarkdownStoreImportResult[]>([])
  const [importing, setImporting] = useState(false)
  const importingRef = useRef(false)
  const importPreviewRequestRef = useRef(0)
  const [operationError, setOperationError] = useState('')
  const [refreshing, setRefreshing] = useState(false)
  const libraryRef = useRef<HTMLDivElement | null>(null)
  const childDialogRef = useRef<HTMLDivElement | null>(null)
  const confirmationRef = useRef<HTMLDivElement | null>(null)
  const editorTriggerRef = useRef<HTMLElement | null>(null)
  const addTriggerRef = useRef<HTMLButtonElement | null>(null)
  const childOpen = Boolean(editing || addView || imports.length)
  const previousChildOpenRef = useRef(false)
  const visibleError = error || operationError

  function resetError() {
    clearError()
    setOperationError('')
  }

  /** Keep native error details; rejected promises also leave a visible failure. */
  async function runAction(action: () => Promise<boolean>, fallback: string) {
    resetError()
    try {
      const ok = await action()
      if (!ok) setOperationError(fallback)
      return ok
    } catch (failure) {
      setOperationError(failure instanceof Error ? failure.message : fallback)
      return false
    }
  }

  function cancelEdit() {
    if (submittingRef.current) return
    resetError()
    setConfirmation(null)
    setEditing(null)
    setDraft(EMPTY_DRAFT)
    setShowEditorPreview(false)
    editorTriggerRef.current?.focus()
  }

  function requestEditorClose() {
    if (submittingRef.current) return
    if (draftDirty) setConfirmation('discard')
    else cancelEdit()
  }

  function closeAdd() {
    ++importPreviewRequestRef.current
    setPreviewing(false)
    setAddView(null)
    setImportNotice('')
    resetError()
    addTriggerRef.current?.focus()
  }

  function closeImportReview() {
    if (importingRef.current) return
    ++importPreviewRequestRef.current
    resetError()
    setImports([])
    addTriggerRef.current?.focus()
  }

  useEffect(() => {
    libraryRef.current?.toggleAttribute('inert', childOpen)
    if (confirmation) confirmationRef.current?.focus()
    else if (childOpen) childDialogRef.current?.focus()
    else if (previousChildOpenRef.current) editorTriggerRef.current?.focus()
    previousChildOpenRef.current = childOpen
  }, [editing, addView, imports.length, confirmation, childOpen])

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if (event.defaultPrevented || (embedded && !childOpen)) return
      const dialog = confirmation ? confirmationRef.current : childOpen ? childDialogRef.current : libraryRef.current
      if (event.key === 'Tab') {
        const controls = Array.from(dialog?.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), summary, [tabindex="0"]') ?? []).filter((element) => {
          const closed = element.closest('details:not([open])')
          return !closed || (element.tagName === 'SUMMARY' && element.parentElement === closed && !closed.parentElement?.closest('details:not([open])'))
        })
        const first = controls[0], last = controls[controls.length - 1]
        if (event.shiftKey && (document.activeElement === first || !controls.includes(document.activeElement as HTMLElement))) { event.preventDefault(); last?.focus() }
        else if (!event.shiftKey && (document.activeElement === last || !controls.includes(document.activeElement as HTMLElement))) { event.preventDefault(); first?.focus() }
        return
      }
      if (event.key !== 'Escape') return
      event.preventDefault()
      event.stopImmediatePropagation()
      if (submittingRef.current || importingRef.current) return
      if (confirmation) setConfirmation(null)
      else if (editing) requestEditorClose()
      else if (imports.length) closeImportReview()
      else if (addView) closeAdd()
      else close()
    }
    window.addEventListener('keydown', handler, true)
    return () => window.removeEventListener('keydown', handler, true)
  }, [embedded, childOpen, editing, addView, imports.length, confirmation, draftDirty, clearError, close])

  useEffect(() => {
    if (!editing || !draftDirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [editing, draftDirty])

  useEffect(() => () => { ++importPreviewRequestRef.current }, [])

  useEffect(() => {
    if (!embedded) return
    let mounted = true
    setRefreshing(true)
    void runAction(refresh, 'Could not load Skills.').then(() => { if (mounted) setRefreshing(false) })
    return () => { mounted = false }
  }, [embedded, refresh])

  const sourceProviders = useMemo(() => Array.from(new Set(entries.map((entry) => entry.sourceProvider).filter(Boolean))).sort(), [entries])
  const groups = useMemo(() => Array.from(new Set(entries.map((entry) => entry.group?.trim()).filter(Boolean))).sort(), [entries])
  const filtered = useMemo(() => {
    const query = search.trim().toLowerCase()
    return entries.filter((entry) => {
      if (filter === 'favorites' && !entry.favorite) return false
      if (filter.startsWith('source:') && entry.sourceProvider !== filter.slice(7)) return false
      if (filter.startsWith('group:') && entry.group !== filter.slice(6)) return false
      return !query || [entry.title, entry.maker, entry.review, entry.preview, entry.sourceProvider ?? '', entry.group ?? '', entry.filePath]
        .some((value) => value.toLowerCase().includes(query))
    })
  }, [entries, filter, search])
  const selected = filtered.find((entry) => entry.filePath === selectedPath) ?? filtered[0] ?? null

  function beginNew() {
    ++importPreviewRequestRef.current
    editorTriggerRef.current = addTriggerRef.current
    resetError()
    setAddView(null)
    editingEntryRef.current = null
    initialDraftRef.current = EMPTY_DRAFT
    setDraft(EMPTY_DRAFT)
    setEditing('new')
    setShowEditorPreview(false)
  }

  function beginEdit(entry: MarkdownStoreEntry) {
    editorTriggerRef.current = document.activeElement as HTMLElement | null
    resetError()
    setSelectedPath(entry.filePath)
    editingEntryRef.current = entry
    initialDraftRef.current = { title: entry.title, maker: entry.maker, review: entry.review, body: entry.body ?? entry.preview, group: entry.group ?? '' }
    setDraft(initialDraftRef.current)
    setEditing('existing')
    setShowEditorPreview(false)
  }

  async function saveDraft(overwriteConfirmed = false) {
    if (submittingRef.current || !draft.title.trim() || !draft.body.trim()) return
    if (editing === 'existing' && !overwriteConfirmed) { setConfirmation('overwrite'); return }
    submittingRef.current = true
    setSubmitting(true)
    const clean = { title: draft.title.trim(), maker: draft.maker.trim(), review: draft.review.trim(), body: draft.body.trim(), group: draft.group.trim() }
    const target = editingEntryRef.current
    const ok = await runAction(() => editing === 'existing'
      ? target ? updateEntry(target, clean) : Promise.resolve(false)
      : createEntry(clean), 'Could not save. Your edits are still here.')
    submittingRef.current = false
    setSubmitting(false)
    setConfirmation(null)
    if (ok) cancelEdit()
  }

  function attach(entry: MarkdownStoreEntry) {
    if (!activeSessionId) return
    attachEntry(activeSessionId, entry)
    if (!embedded) close()
  }

  async function chooseDirectory() {
    await runAction(async () => {
      const chosen = await browseDirectory(markdownStoreDirectory)
      return chosen ? await setDirectory(chosen) && await refresh() : true
    }, 'Could not set the Skills folder.')
  }

  async function loadImports(options: { includeProviders?: boolean; paths?: string[] }, providerId = '') {
    resetError()
    setImportNotice('')
    setPreviewing(true)
    const requestId = ++importPreviewRequestRef.current
    try {
      const allCandidates = await previewImports(options)
      if (requestId !== importPreviewRequestRef.current) return
      const candidates = providerId ? allCandidates.filter((candidate) => normalizeCliProviderIdAlias(candidate.sourceProvider) === providerId) : allCandidates
      setImports(candidates)
      setSelectedImports(new Set(candidates.filter((candidate) => candidate.supported).map((candidate) => candidate.id)))
      setConflicts(Object.fromEntries(candidates.filter((candidate) => candidate.collisionPath).map((candidate) => [candidate.id, 'skip'])))
      setImportResults([])
      if (candidates.length) setAddView(null)
      else if (!useAppStore.getState().markdownStoreError) setImportNotice('No skills found.')
    } catch (failure) {
      if (requestId === importPreviewRequestRef.current) setOperationError(failure instanceof Error ? failure.message : 'Could not read skills.')
    } finally {
      if (requestId === importPreviewRequestRef.current) setPreviewing(false)
    }
  }

  async function chooseImport(kind: 'file' | 'folder') {
    resetError()
    setImportNotice('')
    setPreviewing(true)
    const requestId = ++importPreviewRequestRef.current
    try {
      const path = await browseImport(kind)
      if (requestId !== importPreviewRequestRef.current) return
      if (path) await loadImports({ paths: [path] })
    } catch (failure) {
      if (requestId === importPreviewRequestRef.current) setOperationError(failure instanceof Error ? failure.message : 'Could not open the file picker.')
    } finally {
      if (requestId === importPreviewRequestRef.current) setPreviewing(false)
    }
  }

  async function runImport() {
    if (importingRef.current) return
    const selectedCandidates = imports.filter((candidate) => candidate.supported && selectedImports.has(candidate.id))
    if (!selectedCandidates.length) return
    importingRef.current = true
    setImporting(true)
    resetError()
    try {
      const results = await importEntries(selectedCandidates.map((candidate) => ({
        sourceProvider: candidate.sourceProvider, sourcePath: candidate.sourcePath, conflictAction: conflicts[candidate.id] ?? 'skip',
      })))
      setImportResults(results)
      if (results.length) {
        // Successful items leave the review; failed or missing results stay available for retry.
        const completed = new Set(results.filter((result) => result.status !== 'error').map((result) => result.sourcePath))
        const remaining = imports.filter((candidate) => !completed.has(candidate.sourcePath))
        if (selectedCandidates.every((candidate) => completed.has(candidate.sourcePath))) { setImports([]); addTriggerRef.current?.focus() }
        else setImports(remaining)
      } else setOperationError('No import results were returned. Try again.')
    } catch (failure) {
      setOperationError(failure instanceof Error ? failure.message : 'Could not import skills.')
    } finally {
      importingRef.current = false
      setImporting(false)
    }
  }

  const resultsView = importResults.length > 0 && <div role="status" className="shrink-0 max-h-24 overflow-y-auto flex items-start gap-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}><div className="min-w-0 flex-1">{importResults.map((result, index) => <div key={`${result.sourcePath}-${index}`} style={{ color: result.status === 'error' ? 'var(--red)' : undefined }}>{result.status}: {result.message}</div>)}</div><IconButton size="sm" icon={<X size={13} />} label="Dismiss import results" onClick={() => setImportResults([])} /></div>

  return <>
    <div className={embedded ? 'h-full min-h-0 min-w-0 flex flex-1 flex-col overflow-hidden' : 'fixed inset-0 z-50 flex items-center justify-center p-4'} style={embedded ? undefined : { background: 'rgba(0,0,0,.55)' }}>
      <div ref={libraryRef} role={embedded ? 'region' : 'dialog'} aria-modal={embedded ? undefined : true} aria-hidden={childOpen || undefined} aria-label="Skills" tabIndex={-1} className={embedded ? 'h-full min-h-0 w-full flex flex-1 flex-col overflow-hidden' : 'w-full max-w-6xl h-[min(780px,90vh)] flex flex-col overflow-hidden rounded-xl'} style={{ background: 'var(--surface)', color: 'var(--text)', ...(embedded ? {} : { border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }) }}>
        {!embedded && <div className="shrink-0 flex items-center justify-between px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}><h2 className="text-base font-semibold">Skills</h2><IconButton icon={<X size={16} />} label="Close Skills" onClick={close} /></div>}
        <div className={`shrink-0 flex flex-wrap items-center gap-2 py-3${embedded ? '' : ' px-5'}`} style={{ borderBottom: '1px solid var(--border)' }}>
          <label className="uam-search-field flex min-w-0 flex-1 items-center gap-2 px-2.5" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)' }}>
            <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
            <input type="search" aria-label="Search Skills" autoFocus value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search skills" className="min-w-0 flex-1 text-sm" style={{ border: 0, background: 'transparent', color: 'var(--text)', padding: '8px 0', outline: 'none', boxShadow: 'none' }} />
          </label>
          <select aria-label="Filter Skills" value={filter} onChange={(event) => setFilter(event.target.value)} className="text-xs" style={{ border: '1px solid var(--border)', borderRadius: 7, background: 'var(--bg)', color: 'var(--text)', padding: '7px 9px' }}>
            <option value="all">All entries</option><option value="favorites">Pinned</option>
            {groups.map((group) => <option key={group} value={`group:${group}`}>Group: {group}</option>)}
            {sourceProviders.map((provider) => <option key={provider} value={`source:${provider}`}>Source: {provider}</option>)}
          </select>
          <IconButton icon={<RefreshCw size={15} />} label="Refresh Skills" disabled={loading || refreshing} onClick={async () => { setRefreshing(true); await runAction(refresh, 'Could not refresh Skills.'); setRefreshing(false) }} />
          <IconButton ref={addTriggerRef} icon={<Plus size={16} />} label="Add skill" disabled={!markdownStoreDirectory} onClick={() => { editorTriggerRef.current = addTriggerRef.current; resetError(); setImportNotice(''); setAddView('choose') }} />
        </div>
        {!markdownStoreDirectory && <div role="status" className="shrink-0 flex flex-wrap items-center justify-between gap-3 px-3 py-2 text-xs"><span>Choose a Skills folder before creating or importing entries.</span><Button variant="primary" size="sm" onClick={() => void chooseDirectory()}>Choose folder</Button></div>}
        {!childOpen && visibleError && <div role="alert" className="shrink-0 max-h-24 overflow-y-auto px-3 py-2 text-xs" style={{ color: 'var(--red)' }}>{visibleError}</div>}
        {!imports.length && resultsView}
        <div className={`flex-1 min-h-0 grid grid-rows-[minmax(0,1fr)_minmax(0,1fr)] md:grid-rows-1 md:grid-cols-[minmax(220px,.8fr)_minmax(0,1.2fr)] gap-4 py-4${embedded ? '' : ' px-5'}`}>
          <div aria-label="Skill folders" className="min-h-0 overflow-y-auto">
            {loading ? <p role="status" className="text-sm">Loading Skills...</p> : filtered.length === 0 ? <p role="status" className="text-sm">{search.trim() || filter !== 'all' ? 'No entries match this view' : 'No skills yet'}</p> : <SkillFolders key={`${filter}:${search.trim()}`} entries={filtered} selectedPath={selected?.filePath ?? ''} onSelect={setSelectedPath} onPin={(entry) => void runAction(() => setFavorite(entry, !entry.favorite), 'Could not update the pinned skill.')} />}
          </div>
          <section aria-label="Skill preview" className="flex flex-col min-w-0 min-h-0 overflow-hidden" style={{ border: '1px solid var(--border)' }}>
            {selected ? <>
              <div className="shrink-0 flex flex-wrap items-center justify-between gap-2 px-3 py-2" style={{ borderBottom: '1px solid var(--border)' }}>
                <h3 className="min-w-0 flex-1 truncate text-sm font-semibold">{selected.title}</h3>
                <div className="flex shrink-0 gap-1">
                  <IconButton icon={<Pencil size={15} />} label={`Edit ${selected.title} in app`} onClick={() => beginEdit(selected)} />
                  <IconButton icon={<Paperclip size={15} />} label="Attach to message" disabled={!activeSessionId} onClick={() => attach(selected)} />
                  <IconButton icon={<ExternalLink size={15} />} label="Open in external editor" onClick={() => void runAction(() => editEntry(selected), 'Could not open the external editor.')} />
                  <IconButton icon={<FolderOpen size={15} />} label="Reveal file" onClick={() => void runAction(() => revealEntry(selected), 'Could not reveal the skill file.')} />
                </div>
              </div>
              <div className="flex-1 min-h-0 overflow-auto p-3" style={{ overflowWrap: 'anywhere' }}><MarkdownContent content={selected.body ?? selected.preview} /></div>
            </> : <p className="p-3 text-sm">Select a skill.</p>}
          </section>
        </div>
      </div>
    </div>
    {childOpen && createPortal(<div className="fixed inset-0 z-[60] flex items-center justify-center p-4" style={{ background: 'rgba(0,0,0,.55)' }}>
      <div ref={childDialogRef} role="dialog" aria-modal="true" aria-label={editing ? editing === 'new' ? 'Create skill' : 'Edit skill' : imports.length ? 'Import skills' : addView === 'provider' ? 'Import from provider' : 'Add skill'} tabIndex={-1} className={`flex flex-col max-h-[calc(100vh-2rem)] w-full ${editing || imports.length ? 'max-w-3xl' : 'max-w-xl'} min-h-0 overflow-y-auto rounded-xl p-4 gap-3`} style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', color: 'var(--text)', boxShadow: 'var(--elev-3)' }}>
        {visibleError && <div role="alert" className="shrink-0 text-xs" style={{ color: 'var(--red)' }}>{visibleError}</div>}
        {editing ? <>
          <fieldset disabled={submitting || Boolean(confirmation)} className="grid min-w-0 gap-3" style={{ border: 0, padding: 0, margin: 0 }}>
            <div className="flex items-center justify-between"><strong className="text-sm">{editing === 'new' ? 'New entry' : 'Edit entry'}</strong><Button variant="secondary" size="sm" onClick={() => setShowEditorPreview((value) => !value)}>{showEditorPreview ? 'Edit Markdown' : 'Preview Markdown'}</Button></div>
            <input aria-label="Entry title" value={draft.title} onChange={(event) => setDraft({ ...draft, title: event.target.value })} placeholder="Title" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
            <div className="grid grid-cols-1 md:grid-cols-2 gap-3"><input aria-label="Entry maker" value={draft.maker} onChange={(event) => setDraft({ ...draft, maker: event.target.value })} placeholder="Maker" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} /><input aria-label="Entry group" value={draft.group} onChange={(event) => setDraft({ ...draft, group: event.target.value })} placeholder="Group, for example Coding / Safety" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} /></div>
            <input aria-label="Entry summary" value={draft.review} onChange={(event) => setDraft({ ...draft, review: event.target.value })} placeholder="Summary" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
            {showEditorPreview ? <div className="min-h-48 rounded-md p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text)' }}><MarkdownContent content={draft.body || '_Nothing to preview._'} /></div> : <textarea aria-label="Entry Markdown body" value={draft.body} onChange={(event) => setDraft({ ...draft, body: event.target.value })} placeholder="Markdown body" rows={10} className="text-sm resize-vertical" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px', fontFamily: 'var(--font-mono)' }} />}
          </fieldset>
          {confirmation ? <div ref={confirmationRef} role="alertdialog" aria-modal="true" aria-label={confirmation === 'discard' ? 'Unsaved skill changes' : 'Overwrite skill'} tabIndex={-1} className="p-3" style={{ border: '1px solid var(--border-bright)' }}>
            <strong className="text-sm">{confirmation === 'discard' ? 'Discard your changes?' : `Overwrite ${editingEntryRef.current?.title}?`}</strong>
            {confirmation === 'overwrite' && <p className="text-xs mt-1">This replaces the saved entry.</p>}
            <div className="mt-3 flex flex-wrap justify-end gap-2">
              <Button size="sm" disabled={submitting} onClick={() => setConfirmation(null)}>Keep editing</Button>
              {confirmation === 'discard' ? <><Button variant="danger" size="sm" disabled={submitting} onClick={cancelEdit}>Discard</Button><Button variant="primary" size="sm" disabled={submitting || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>Save &amp; close</Button></> : <Button variant="danger" size="sm" disabled={submitting} onClick={() => void saveDraft(true)}>{submitting ? 'Saving...' : 'Overwrite'}</Button>}
            </div>
          </div> : <div className="flex justify-end gap-2"><Button variant="secondary" size="sm" disabled={submitting} onClick={requestEditorClose}>Cancel</Button><Button variant="primary" size="sm" disabled={submitting || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>{submitting ? 'Saving...' : 'Save'}</Button></div>}
        </> : imports.length ? <>
          <strong className="text-sm shrink-0">Import preview</strong>
          {resultsView}
          <div data-import-candidates className="grid min-h-0 gap-2 overflow-y-auto">{imports.map((candidate) => <div key={candidate.id} className="flex flex-wrap items-start gap-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
            <label className="flex min-w-0 flex-1 items-start gap-2">
              <input type="checkbox" disabled={!candidate.supported || importing} checked={candidate.supported && selectedImports.has(candidate.id)} onChange={(event) => setSelectedImports((current) => { const next = new Set(current); if (event.target.checked) next.add(candidate.id); else next.delete(candidate.id); return next })} />
              <span className="min-w-0 flex-1"><strong style={{ color: 'var(--text)' }}>{candidate.title || candidate.sourcePath.split(/[\\/]/).pop()}</strong><span className="block truncate" title={candidate.sourcePath} style={{ color: 'var(--text-3)' }}>{candidate.sourcePath}</span>{candidate.validationError && <span className="block" style={{ color: 'var(--red)' }}>{candidate.validationError}</span>}</span>
            </label>
            {candidate.collisionPath && <select disabled={importing} aria-label={`Collision action for ${candidate.title}`} value={conflicts[candidate.id] ?? 'skip'} onChange={(event) => setConflicts({ ...conflicts, [candidate.id]: event.target.value as MarkdownStoreConflictAction })} style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 5, color: 'var(--text)', padding: 4 }}><option value="skip">Skip existing</option><option value="replace">Replace existing</option><option value="separate">Keep separate</option></select>}
          </div>)}</div>
          <div className="shrink-0 flex justify-end gap-2"><Button variant="secondary" size="sm" disabled={importing} onClick={closeImportReview}>Cancel</Button><Button variant="primary" size="sm" disabled={importing || !imports.some((candidate) => candidate.supported && selectedImports.has(candidate.id))} onClick={() => void runImport()}>{importing ? 'Importing...' : 'Import selected'}</Button></div>
        </> : <>
          <div className="flex items-center justify-between"><strong>{addView === 'provider' ? 'Import from provider' : 'Add skill'}</strong><IconButton icon={<X size={16} />} label="Close add skill" onClick={closeAdd} /></div>
          {addView === 'choose' ? <div aria-label="Add skill choices" className="grid gap-2" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 112px), 1fr))' }}>
            {[
              { label: 'Create', icon: <Plus size={24} />, action: beginNew },
              { label: 'Import from provider', icon: <Download size={24} />, action: () => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setImportNotice(''); setAddView('provider') } },
              { label: 'Import file', icon: <FileInput size={24} />, action: () => void chooseImport('file') },
              { label: 'Import folder', icon: <FolderInput size={24} />, action: () => void chooseImport('folder') },
            ].map((choice) => <Button key={choice.label} aria-label={choice.label} leadingIcon={choice.icon} disabled={previewing} onClick={choice.action} style={{ aspectRatio: '1 / 1', height: 'auto', flexDirection: 'column', whiteSpace: 'normal', padding: 8 }}>{choice.label}</Button>)}
          </div> : <>
            <SelectionGrid label="Provider" options={PROVIDER_OPTIONS} value={importProvider} onChange={(id) => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setImportNotice(''); setImportProvider(id) }} />
            <div className="flex justify-end gap-2"><Button onClick={() => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setImportNotice(''); setAddView('choose') }}>Back</Button><Button variant="primary" disabled={!importProvider || previewing} onClick={() => void loadImports({ includeProviders: true }, importProvider)}>Review imports</Button></div>
          </>}
          {(previewing || importNotice) && !visibleError && <p role="status" className="text-xs">{previewing ? 'Reading skills...' : importNotice}</p>}
        </>}
      </div>
    </div>, document.body)}
  </>
}

/** Slash-separated groups are folders; pinning continues to use the persisted favorite flag. */
function SkillFolders({ entries, selectedPath, onSelect, onPin }: { entries: MarkdownStoreEntry[]; selectedPath: string; onSelect: (path: string) => void; onPin: (entry: MarkdownStoreEntry) => void }) {
  type FolderNode = { entries: MarkdownStoreEntry[]; folders: Map<string, FolderNode> }
  const tree = useMemo(() => {
    const root: FolderNode = { entries: [], folders: new Map() }
    for (const entry of entries) {
      let folder = root
      for (const segment of (entry.group ?? '').split('/').map((part) => part.trim()).filter(Boolean)) {
        if (!folder.folders.has(segment)) folder.folders.set(segment, { entries: [], folders: new Map() })
        folder = folder.folders.get(segment)!
      }
      folder.entries.push(entry)
    }
    return root
  }, [entries])
  function renderFolder(node: FolderNode): ReactNode {
    return <>
      {Array.from(node.folders).sort(([a], [b]) => a.localeCompare(b)).map(([name, folder]) => <details key={name} open className="min-w-0">
        <summary className="cursor-pointer py-1 text-sm truncate"><Folder size={14} className="inline mr-1.5" aria-hidden />{name}</summary>
        <div className="ml-2 pl-2" style={{ borderLeft: '1px solid var(--border-bright)' }}>{renderFolder(folder)}</div>
      </details>)}
      {node.entries.map((entry) => <div key={entry.filePath} className="relative flex min-w-0 items-center gap-1 py-1.5 pl-2 pr-1" style={{ borderLeft: `2px solid ${entry.filePath === selectedPath ? 'var(--accent)' : 'transparent'}`, background: entry.filePath === selectedPath ? 'var(--accent-dim)' : 'transparent' }}>
        {entry.group?.trim() && <span aria-hidden className="absolute left-[-10px] top-1/2 w-2" style={{ borderTop: '1px solid var(--border-bright)' }} />}
        <button type="button" aria-current={entry.filePath === selectedPath ? 'true' : undefined} onClick={() => onSelect(entry.filePath)} className="min-w-0 flex-1 text-left" style={{ background: 'transparent', border: 0, color: 'var(--text)', padding: 0 }}>
          <div className="text-sm truncate">{[entry.title, entry.maker].filter(Boolean).join(' · ')}</div>
          <div className="text-xs truncate" style={{ color: 'var(--text-2)' }}>{entry.review || entry.preview}</div>
        </button>
        <IconButton size="sm" icon={<Pin size={14} fill={entry.favorite ? 'currentColor' : 'none'} />} active={Boolean(entry.favorite)} label={`${entry.favorite ? 'Unpin' : 'Pin'} ${entry.title}`} onClick={() => onPin(entry)} />
      </div>)}
    </>
  }
  return renderFolder(tree)
}
