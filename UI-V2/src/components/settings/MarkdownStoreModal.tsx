import { cloneElement, useEffect, useMemo, useRef, useState, type ReactElement, type ReactNode } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Button, Disclosure, IconButton, MenuSelect, Notice, Overlay, Reveal, RowButton, SegmentedControl, Switch, usePresence, Swap } from '../ui'
import type { MenuSelectOption } from '../ui'
import { MarkdownContent } from '../markdown/Markdown'
import { ProviderLogo } from '../shared/ProviderLogo'
import { SelectionGrid } from '../shared/SelectionGrid'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { SaveButton, SettingRow, SettingsLoading } from './SettingsPrimitives'
import { normalizeCliProviderIdAlias, providerMetadataForId, SUPPORTED_TEXT_WORKER_PROVIDER_IDS } from '../../utils/providerMetadata'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate } from '../../types/markdownStore'
import { ChevronRight, Download, ExternalLink, FileInput, FileText, Folder, FolderInput, FolderOpen, Paperclip, Pencil, Pin, Plus, RefreshCw, Search, X } from 'lucide-react'
import './stores.css'
import './provider-settings.css'

const EMPTY_DRAFT: MarkdownStoreDraft = { title: '', maker: '', review: '', body: '', group: '' }
const PROVIDER_OPTIONS = SUPPORTED_TEXT_WORKER_PROVIDER_IDS.map((id) => ({
  id, label: providerMetadataForId(id).name, icon: <ProviderLogo providerId={id} size={24} />,
}))

/** Skills uses the same store actions in Settings and in the standalone library. Add, import and edit steps replace the list inside this dialog. */
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
  const [imports, setImports] = useState<MarkdownStoreImportCandidate[]>([])
  const [selectedImports, setSelectedImports] = useState<Set<string>>(new Set())
  const [conflicts, setConflicts] = useState<Record<string, MarkdownStoreConflictAction>>({})
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
  // The step stays mounted while it slides out to the right; the snapshot keeps its last content during that exit.
  const stepPresence = usePresence(childOpen)
  // Confirmation and preview lines swap in place; the last confirmation stays readable while it animates out.
  const confirmPresence = usePresence(Boolean(confirmation))
  const idleActionsPresence = usePresence(!confirmation)
  const previewPresence = usePresence(previewing)
  const lastConfirmation = useRef<'discard' | 'overwrite' | null>(null)
  if (confirmation) lastConfirmation.current = confirmation
  const childSnapshot = useRef<ReactElement | null>(null)
  const openAddChoices = () => { editorTriggerRef.current = addTriggerRef.current; resetError(); setAddView('choose') }
  const previousChildOpenRef = useRef(false)
  const visibleError = error || operationError
  useEffect(() => { if (visibleError) reportAppFailure('Skills action failed', visibleError) }, [visibleError])

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
    if (confirmation) confirmationRef.current?.querySelector<HTMLElement>('button')?.focus()
    else if (childOpen) childDialogRef.current?.focus()
    else if (previousChildOpenRef.current) editorTriggerRef.current?.focus()
    previousChildOpenRef.current = childOpen
  }, [editing, addView, imports.length, confirmation, childOpen])

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if (event.defaultPrevented || (embedded && !childOpen)) return
      const dialog = confirmation ? confirmationRef.current : childOpen ? childDialogRef.current : libraryRef.current
      if (event.key === 'Tab') {
        // Closed disclosure panels are inert, so their controls are skipped.
        const controls = Array.from(dialog?.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), [tabindex="0"]') ?? []).filter((element) => !element.closest('[inert]'))
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
    // Strict-mode double mount supersedes the first request; the newer refresh owns loading and error state.
    void refresh().catch(() => false).then(() => { if (mounted) setRefreshing(false) })
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
  const filterOptions: MenuSelectOption[] = [
    { value: 'all', label: 'All entries' },
    { value: 'favorites', label: 'Pinned' },
    ...groups.map((group) => ({ value: `group:${group}`, label: `Group: ${group}` })),
    ...sourceProviders.map((provider) => ({ value: `source:${provider}`, label: `Source: ${provider}` })),
  ]

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
    if (submittingRef.current || !draftDirty || !draft.title.trim() || !draft.body.trim()) return
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
    if (ok) {
      reportAppNotice(`Saved “${clean.title}”.`, 'success')
      cancelEdit()
    }
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
    setPreviewing(true)
    const requestId = ++importPreviewRequestRef.current
    try {
      const allCandidates = await previewImports(options)
      if (requestId !== importPreviewRequestRef.current) return
      const candidates = providerId ? allCandidates.filter((candidate) => normalizeCliProviderIdAlias(candidate.sourceProvider) === providerId) : allCandidates
      setImports(candidates)
      setSelectedImports(new Set(candidates.filter((candidate) => candidate.supported).map((candidate) => candidate.id)))
      setConflicts(Object.fromEntries(candidates.filter((candidate) => candidate.collisionPath).map((candidate) => [candidate.id, 'skip'])))
      if (candidates.length) setAddView(null)
      else if (!useAppStore.getState().markdownStoreError) reportAppNotice('No skills found.')
    } catch (failure) {
      if (requestId === importPreviewRequestRef.current) setOperationError(failure instanceof Error ? failure.message : 'Could not read skills.')
    } finally {
      if (requestId === importPreviewRequestRef.current) setPreviewing(false)
    }
  }

  async function chooseImport(kind: 'file' | 'folder') {
    resetError()
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
      if (results.length) {
        for (const result of results) {
          if (result.status === 'error') reportAppFailure(`Could not import ${result.sourcePath.split(/[\\/]/).pop()}.`, result.message)
          else reportAppNotice(result.message, result.status === 'imported' ? 'success' : 'info')
        }
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

  const childTitle = editing ? (editing === 'new' ? 'Create skill' : 'Edit skill') : imports.length ? 'Import skills' : addView === 'provider' ? 'Import from provider' : 'Add skill'
  // The add, import and editor steps replace the library list inside this dialog; they never stack a second dialog.
  const childPanel = <div ref={childDialogRef} role="group" aria-label={childTitle} tabIndex={-1} className={`uam-skills-step flex-1 min-h-0 flex flex-col gap-3 overflow-y-auto py-4 outline-none${embedded ? '' : ' px-5'}`}>
    {editing ? <>
      <fieldset disabled={submitting || Boolean(confirmation)} className="grid min-w-0 gap-3" style={{ border: 0, padding: 0, margin: 0 }}>
        <div className="flex justify-end"><SegmentedControl aria-label="Entry view" options={[{ value: 'edit', label: 'Edit' }, { value: 'preview', label: 'Preview' }]} value={showEditorPreview ? 'preview' : 'edit'} onChange={(value) => setShowEditorPreview(value === 'preview')} /></div>
        <input aria-label="Entry title" value={draft.title} onChange={(event) => setDraft({ ...draft, title: event.target.value })} placeholder="Title" className="uam-field text-sm" />
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3"><input aria-label="Entry maker" value={draft.maker} onChange={(event) => setDraft({ ...draft, maker: event.target.value })} placeholder="Maker" className="uam-field text-sm" /><input aria-label="Entry group" value={draft.group} onChange={(event) => setDraft({ ...draft, group: event.target.value })} placeholder="Group" className="uam-field text-sm" /></div>
        <input aria-label="Entry summary" value={draft.review} onChange={(event) => setDraft({ ...draft, review: event.target.value })} placeholder="Summary" className="uam-field text-sm" />
        <Swap swapKey={showEditorPreview ? 'preview' : 'edit'}>{showEditorPreview ? <div className="min-h-48 rounded-md p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text)' }}><MarkdownContent content={draft.body || '_Nothing to preview._'} /></div> : <textarea aria-label="Entry Markdown body" value={draft.body} onChange={(event) => setDraft({ ...draft, body: event.target.value })} placeholder="Markdown body" rows={10} className="uam-field text-sm resize-vertical font-mono" />}</Swap>
      </fieldset>
      {confirmPresence.mounted && <div ref={confirmationRef} role="group" aria-label={lastConfirmation.current === 'discard' ? 'Unsaved skill changes' : 'Overwrite skill'} tabIndex={-1} data-state={confirmPresence.closing ? 'closed' : 'open'} className="uam-presence">
        <Notice tone="warning" dismissible={false} dismissLabel="Dismiss" title={lastConfirmation.current === 'discard' ? 'Discard your changes?' : `Overwrite ${editingEntryRef.current?.title}?`}
          actions={<>
            <Button size="sm" disabled={submitting} onClick={() => setConfirmation(null)}>Keep editing</Button>
            {lastConfirmation.current === 'discard' ? <><Button variant="danger" size="sm" disabled={submitting} onClick={cancelEdit}>Discard</Button><Button variant="primary" size="sm" disabled={submitting || !draftDirty || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>Save &amp; close</Button></> : <SaveButton variant="danger" saving={submitting} pendingLabel="Saving..." disabled={submitting} onClick={() => void saveDraft(true)}>Overwrite</SaveButton>}
          </>}>
          {lastConfirmation.current === 'overwrite' && 'This replaces the saved entry.'}
        </Notice>
      </div>}
      {idleActionsPresence.mounted && <div className="flex justify-end gap-2 uam-presence" data-state={idleActionsPresence.closing ? 'closed' : 'open'}><Button variant="secondary" size="sm" disabled={submitting} onClick={requestEditorClose}>Cancel</Button><SaveButton saving={submitting} pendingLabel="Saving..." disabled={submitting || !draftDirty || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>Save</SaveButton></div>}
    </> : imports.length ? <>
      <div className="uam-set-section__title shrink-0 px-1">Import preview</div>
      <div data-import-candidates className="grid min-h-0 gap-2">{imports.map((candidate) => <Reveal key={candidate.id} open><div title={candidate.sourcePath} className="flex flex-wrap items-start gap-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
        <div className="flex min-w-0 flex-1 flex-col gap-1">
          <Switch label={candidate.title || candidate.sourcePath.split(/[\\/]/).pop() || candidate.sourcePath} disabled={!candidate.supported || importing} checked={candidate.supported && selectedImports.has(candidate.id)} onChange={(event) => setSelectedImports((current) => { const next = new Set(current); if (event.target.checked) next.add(candidate.id); else next.delete(candidate.id); return next })} />
          {candidate.validationError && <span className="flex items-center gap-1" style={{ color: 'var(--error)' }}><X size={14} aria-hidden className="shrink-0" />{candidate.validationError}</span>}
        </div>
        {candidate.collisionPath && <div className="w-40 shrink-0"><MenuSelect label="If a skill with this name exists" disabled={importing} value={conflicts[candidate.id] ?? 'skip'} onChange={(value) => setConflicts({ ...conflicts, [candidate.id]: value as MarkdownStoreConflictAction })} options={[{ value: 'skip', label: 'Skip' }, { value: 'replace', label: 'Replace' }, { value: 'separate', label: 'Keep both' }]} /></div>}
      </div></Reveal>)}</div>
      <div className="shrink-0 flex justify-end gap-2"><Button variant="secondary" size="sm" disabled={importing} onClick={closeImportReview}>Cancel</Button><Button variant="primary" size="sm" disabled={importing || !imports.some((candidate) => candidate.supported && selectedImports.has(candidate.id))} onClick={() => void runImport()}>{importing ? 'Importing...' : 'Import selected'}</Button></div>
    </> : <>
      <div className="flex justify-end"><IconButton icon={<X size={16} />} label="Close add skill" onClick={closeAdd} /></div>
      <Swap swapKey={addView ?? ''}>{addView === 'choose' ? <div aria-label="Add skill choices" className="grid gap-2">
        {[
          { label: 'Create', icon: <Plus size={14} />, action: beginNew },
          { label: 'Import from provider', icon: <Download size={14} />, action: () => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setAddView('provider') } },
          { label: 'Import file', icon: <FileInput size={14} />, action: () => void chooseImport('file') },
          { label: 'Import folder', icon: <FolderInput size={14} />, action: () => void chooseImport('folder') },
        ].map((choice) => <RowButton key={choice.label} className="uam-prov-option" aria-label={choice.label} disabled={previewing} onClick={choice.action}>
          <span className="uam-prov-option__icon" aria-hidden>{choice.icon}</span>
          <span className="uam-prov-option__label min-w-0 text-left">{choice.label}</span>
          <ChevronRight size={14} aria-hidden className="uam-prov-option__chevron" />
        </RowButton>)}
      </div> : <>
        <SelectionGrid label="Provider" options={PROVIDER_OPTIONS} value={importProvider} onChange={(id) => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setImportProvider(id) }} />
        <div className="flex justify-end gap-2"><Button onClick={() => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setAddView('choose') }}>Back</Button><Button variant="primary" disabled={!importProvider || previewing} onClick={() => void loadImports({ includeProviders: true }, importProvider)}>Review imports</Button></div>
      </>}</Swap>
      {previewPresence.mounted && <p role="status" data-state={previewPresence.closing ? 'closed' : 'open'} className="uam-presence text-xs">Reading skills...</p>}
    </>}
  </div>

  const library = <div ref={libraryRef} className="flex-1 min-h-0 flex flex-col py-4">
    <div className="grid min-h-0 flex-1 grid-rows-[minmax(0,1fr)_minmax(0,1fr)] md:grid-rows-1 md:grid-cols-[minmax(220px,.8fr)_minmax(0,1.2fr)] gap-4">
      <div className="min-h-0 flex flex-col">
        <div className="uam-set-section__title shrink-0 px-1 pb-2">Library</div>
        <div aria-label="Skill folders" className="min-h-0 flex-1 overflow-y-auto">
          <Swap swapKey={loading ? 'loading' : filtered.length === 0 ? 'empty' : 'list'}>{loading ? <SettingsLoading label="Loading skills" rows={5} /> : filtered.length === 0 ? (search.trim() || filter !== 'all'
            ? <p className="px-1 text-sm" style={{ color: 'var(--text-2)' }}>No skills match this view.</p>
            : visibleError ? null
            : <div className="grid justify-items-start gap-2 px-1 py-2">
              <p className="text-sm" style={{ color: 'var(--text-3)' }}>No skills yet.</p>
              <Button size="sm" onClick={openAddChoices}>Add skill</Button>
            </div>) : <SkillFolders key={`${filter}:${search.trim()}`} entries={filtered} selectedPath={selected?.filePath ?? ''} onSelect={setSelectedPath} onPin={(entry) => void runAction(() => setFavorite(entry, !entry.favorite), 'Could not update the pinned skill.')} />}</Swap>
        </div>
      </div>
      <section aria-label="Skill preview" className="uam-skill-preview flex flex-col min-w-0 min-h-0 overflow-hidden">
        <Swap swapKey={selected ? selected.filePath : ''} className="shrink-0">{selected ? <>
          <div className="shrink-0 flex items-center justify-between gap-2 px-4 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
            <div className="min-w-0 flex-1">
              <h3 className="truncate text-sm font-semibold">{selected.title}</h3>
              <div className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{[selected.maker, selected.group, selected.sourceProvider && providerMetadataForId(normalizeCliProviderIdAlias(selected.sourceProvider)).name].filter(Boolean).join(' · ')}</div>
            </div>
            <div className="flex shrink-0 gap-1">
              <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${selected.title} in app`} onClick={() => beginEdit(selected)} />
              <IconButton size="sm" icon={<Paperclip size={14} />} label="Attach to message" disabled={!activeSessionId} onClick={() => attach(selected)} />
              <IconButton size="sm" icon={<ExternalLink size={14} />} label="Open in external editor" onClick={() => void runAction(() => editEntry(selected), 'Could not open the external editor.')} />
              <IconButton size="sm" icon={<FolderOpen size={14} />} label="Reveal file" onClick={() => void runAction(() => revealEntry(selected), 'Could not reveal the skill file.')} />
            </div>
          </div>
        </> : null}</Swap>
        <Swap swapKey={selected ? selected.filePath : ''} className="flex-1 min-h-0 overflow-auto">{selected ?           <div className="flex-1 min-h-0 overflow-auto px-5 py-4" style={{ overflowWrap: 'anywhere' }}><div className="uam-store-markdown"><MarkdownContent content={selected.body ?? selected.preview} /></div></div>
        : <p className="flex-1 px-5 py-4 text-sm" style={{ color: 'var(--text-3)' }}>Select a skill to preview it.</p>}</Swap>
      </section>
    </div>
  </div>

  if (childOpen) childSnapshot.current = childPanel
  const exitProps: Record<string, string | true | undefined> = stepPresence.closing ? { 'data-closing': 'true', inert: '', 'aria-hidden': 'true' } : {}

  // Overlay has no attribute passthrough, so the dialog role stays on the panel.
  const panel = <div role={embedded ? 'region' : 'dialog'} aria-modal={embedded ? undefined : true} aria-label="Skills" tabIndex={-1} className={embedded ? 'h-full min-h-0 w-full flex flex-1 flex-col overflow-hidden' : 'uam-skills-dialog w-full max-w-6xl h-[min(780px,90vh)] flex flex-col overflow-hidden rounded-xl'} style={{ background: 'var(--surface)', color: 'var(--text)' }}>
      {!embedded && <div className="shrink-0 flex items-center justify-between px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}><h2 className="text-base font-semibold">Skills</h2><IconButton icon={<X size={16} />} label="Close skills" onClick={close} /></div>}
      <div className={`shrink-0 pt-3${embedded ? '' : ' px-5'}`}>
        <div className="uam-set-card">
          <SettingRow
            icon={<FolderOpen size={15} aria-hidden />}
            title="Skills folder"
            description={markdownStoreDirectory ? <span className="uam-store-path">{markdownStoreDirectory}</span> : 'Not set'}
            control={<Button variant="secondary" size="sm" onClick={() => void chooseDirectory()}>Change…</Button>}
          />
        </div>
      </div>
      <div className={`shrink-0 flex flex-wrap items-center gap-2 py-3${embedded ? '' : ' px-5'}`} style={{ borderBottom: '1px solid var(--border)' }}>
        <label className="uam-search-field uam-store-search flex min-w-0 flex-1 items-center gap-2 px-2.5">
          <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
          <input type="search" aria-label="Search skills" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search skills" className="min-w-0 flex-1 text-sm" style={{ border: 0, background: 'transparent', color: 'var(--text)', padding: 0, outline: 'none', boxShadow: 'none' }} />
        </label>
        <div className="w-40 shrink-0">
          <MenuSelect label="Filter skills" value={filter} onChange={setFilter} options={filterOptions} />
        </div>
        <IconButton icon={<RefreshCw size={15} />} label="Refresh skills" disabled={loading || refreshing} onClick={async () => { setRefreshing(true); await runAction(refresh, 'Could not refresh Skills.'); setRefreshing(false) }} />
        <IconButton ref={addTriggerRef} className={markdownStoreDirectory && !entries.length ? 'uam-store-add--primary' : undefined} icon={<Plus size={16} />} label="Add skill" disabled={!markdownStoreDirectory} onClick={openAddChoices} />
      </div>
      {/* Stage: the library and the step share one area, so the step slides over the library. */}
      <div className="relative flex-1 min-h-0 flex flex-col overflow-hidden">
      <Swap swapKey={markdownStoreDirectory ? 'library' : 'empty'} className="flex-1 min-h-0 flex flex-col">{!markdownStoreDirectory ? <div className="grid justify-items-start gap-2 px-1 py-4">
        <p className="text-sm" style={{ color: 'var(--text-3)' }}>Choose a skills folder to get started.</p>
        <Button variant="primary" size="sm" onClick={() => void chooseDirectory()}>Choose folder</Button>
      </div> : <div className={`uam-skills-library flex-1 min-h-0 flex flex-col${embedded ? '' : ' px-5'}`} data-away={childOpen || undefined} aria-hidden={childOpen || undefined} {...(childOpen ? { inert: '' } : {})}>{library}</div>}</Swap>
      {/* Slides out, not unmounted, while a step is open: the library keeps its state and focus can return to its trigger. */}
      {stepPresence.mounted && childSnapshot.current && cloneElement(childSnapshot.current, exitProps)}
      </div>
    </div>
  return embedded ? <div className="h-full min-h-0 min-w-0 flex flex-1 flex-col overflow-hidden">{panel}</div> : <Overlay open zIndex={50}>{panel}</Overlay>
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
      {Array.from(node.folders).sort(([a], [b]) => a.localeCompare(b)).map(([name, folder]) => <Disclosure key={name} defaultOpen className="min-w-0"
        summary={<span className="flex min-w-0 flex-1 items-center gap-1.5"><Folder size={14} aria-hidden style={{ color: 'var(--text-3)' }} /><span className="truncate">{name}</span><span className="uam-skill-folder__count">{folder.entries.length}</span></span>}>
        <div className="ml-3 pl-2" style={{ borderLeft: '1px solid var(--border)' }}>{renderFolder(folder)}</div>
      </Disclosure>)}
      {node.entries.map((entry) => <div key={entry.filePath} className="uam-skill-row flex min-w-0 items-center gap-2" data-selected={entry.filePath === selectedPath}>
        <RowButton type="button" aria-current={entry.filePath === selectedPath ? 'true' : undefined} onClick={() => onSelect(entry.filePath)} className="min-w-0 flex-1 text-left">
          <span className="flex min-w-0 flex-1 items-center gap-1.5"><span className="shrink-0 inline-flex" aria-hidden style={{ color: 'var(--text-3)' }}>{entry.sourceProvider ? <ProviderLogo providerId={normalizeCliProviderIdAlias(entry.sourceProvider)} size={13} /> : <FileText size={13} />}</span><span className="text-sm truncate">{entry.title}</span>{entry.commandName && <code className="uam-skill-row__command">/{entry.commandName}</code>}</span>
          <span className="ml-auto min-w-0 max-w-[45%] truncate text-xs" style={{ color: 'var(--text-3)' }}>{[entry.maker, entry.review || entry.preview].filter(Boolean).join(' · ')}</span>
        </RowButton>
        <IconButton size="sm" className="uam-skill-pin" icon={<Pin size={14} fill={entry.favorite ? 'currentColor' : 'none'} />} active={Boolean(entry.favorite)} label={`${entry.favorite ? 'Unpin' : 'Pin'} ${entry.title}`} onClick={() => onPin(entry)} />
      </div>)}
    </>
  }
  return renderFolder(tree)
}
