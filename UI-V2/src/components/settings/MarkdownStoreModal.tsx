import { cloneElement, Fragment, useEffect, useMemo, useRef, useState, type ReactElement, type ReactNode } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Button, Disclosure, IconButton, MenuSelect, Notice, Overlay, ProgressFill, Reveal, RowButton, SegmentedControl, Switch, Tooltip, usePresence, Swap } from '../ui'
import type { MenuSelectOption } from '../ui'
import { MarkdownContent } from '../markdown/Markdown'
import { ProviderLogo } from '../shared/ProviderLogo'
import { SelectionGrid } from '../shared/SelectionGrid'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { SaveButton, SettingRow, SettingsLoading } from './SettingsPrimitives'
import { normalizeCliProviderIdAlias, providerMetadataForId, SUPPORTED_TEXT_WORKER_PROVIDER_IDS } from '../../utils/providerMetadata'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate } from '../../types/markdownStore'
import { ArrowLeft, ChevronRight, Download, ExternalLink, FileInput, FileText, Folder, FolderInput, FolderOpen, Paperclip, Pencil, Pin, Plus, RefreshCw, Search, X } from 'lucide-react'
import './stores.css'
import './provider-settings.css'

/** The detail header already shows the title; drop a leading Markdown heading that repeats it. */
function withoutRepeatedTitle(body: string, title: string) {
  const match = /^\s*#{1,2}\s+(.+?)\s*(?:\n|$)/.exec(body)
  return match && match[1].trim().toLowerCase() === title.trim().toLowerCase() ? body.slice(match[0].length) : body
}

const EMPTY_DRAFT: MarkdownStoreDraft = { title: '', maker: '', review: '', body: '', group: '' }
const PROVIDER_OPTIONS = SUPPORTED_TEXT_WORKER_PROVIDER_IDS.map((id) => ({
  id, label: providerMetadataForId(id).name, icon: <ProviderLogo providerId={id} size={24} />,
}))
const FOLDER_ACTIONS: MenuSelectOption[] = [{ value: 'choose', label: 'Choose folder…' }, { value: 'enter', label: 'Enter path…' }]

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
  // openPath is the skill shown in place of the list; folderKey filters the list ('' = all skills).
  const [openPath, setOpenPath] = useState('')
  const [folderKey, setFolderKey] = useState('')
  const [collapsed, setCollapsed] = useState<Set<string>>(() => new Set())
  const [pathOpen, setPathOpen] = useState(false)
  const [pathDraft, setPathDraft] = useState('')
  const searchRef = useRef<HTMLInputElement | null>(null)
  const backRef = useRef<HTMLButtonElement | null>(null)
  const previousOpenRef = useRef('')
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
  const filtered = useMemo(() => {
    const query = search.trim().toLowerCase()
    return entries.filter((entry) => {
      if (filter === 'favorites' && !entry.favorite) return false
      if (filter.startsWith('source:') && entry.sourceProvider !== filter.slice(7)) return false
      return !query || [entry.title, entry.maker, entry.review, entry.preview, entry.sourceProvider ?? '', entry.group ?? '', entry.filePath]
        .some((value) => value.toLowerCase().includes(query))
    })
  }, [entries, filter, search])
  const folderTree = useMemo(() => buildFolderTree(entries), [entries])
  const listed = filtered.filter((entry) => folderContains(folderKey, entry))
  const opened = openPath ? entries.find((entry) => entry.filePath === openPath) ?? null : null
  const filterOptions: MenuSelectOption[] = [
    { value: 'all', label: 'All entries' },
    { value: 'favorites', label: 'Pinned' },
    ...sourceProviders.map((provider) => ({ value: `source:${provider}`, label: `Source: ${provider}` })),
  ]
  // Opening a skill moves focus to Back; going back returns focus to the search field.
  useEffect(() => {
    if (openPath && !previousOpenRef.current) backRef.current?.focus()
    else if (!openPath && previousOpenRef.current) searchRef.current?.focus()
    previousOpenRef.current = openPath
  }, [openPath])

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

  /** The typed path stays in its field until the store accepts it. */
  async function usePath() {
    const next = pathDraft.trim()
    const ok = await runAction(async () => await setDirectory(next) && await refresh(), 'Could not set the Skills folder.')
    if (ok) setPathOpen(false)
  }

  function changeFolder(action: string) {
    if (action === 'enter') {
      setPathDraft(markdownStoreDirectory)
      setPathOpen(true)
    } else void chooseDirectory()
  }

  function toggleFolder(key: string) {
    setCollapsed((current) => {
      const next = new Set(current)
      if (next.has(key)) next.delete(key)
      else next.add(key)
      return next
    })
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

  const childTitle = editing ? (editing === 'new' ? 'Create skill' : 'Edit skill') : imports.length ? 'Import skills' : addView === 'provider' ? 'Import from provider' : 'New skill'
  const stepKey = editing ? 'editor' : imports.length ? 'import' : addView ?? ''
  // Back steps out one level; from the first add step it closes the add flow.
  const stepBack = editing ? requestEditorClose : imports.length ? closeImportReview : addView === 'provider' ? () => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setAddView('choose') } : closeAdd
  // The add, import and editor steps render in the library pane, beside the folder nav; they never stack a second dialog.
  const childPanel = <div ref={childDialogRef} role="group" aria-label={childTitle} tabIndex={-1} className="uam-skills-step flex min-h-0 flex-1 flex-col outline-none">
    <Swap swapKey={stepKey} className="flex min-h-0 flex-1 flex-col"><div className="flex min-h-0 flex-1 flex-col">
      <div className="uam-store-toolbar">
        <IconButton size="sm" icon={<ArrowLeft size={15} aria-hidden />} label="Back" disabled={submitting || importing} onClick={stepBack} />
        <div className="uam-store-truncate min-w-0 flex-1 text-sm font-semibold" style={{ color: 'var(--text)' }}>{childTitle}</div>
      </div>
      <div data-step-scroll className="flex min-h-0 flex-1 flex-col gap-3 overflow-y-auto px-4 py-4">{editing ? <>
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
      {idleActionsPresence.mounted && <div className="flex items-center justify-end gap-2 uam-presence" data-state={idleActionsPresence.closing ? 'closed' : 'open'}><Button variant="secondary" size="sm" disabled={submitting} onClick={requestEditorClose}>Cancel</Button><SaveButton saving={submitting} pendingLabel="Saving..." disabled={submitting || !draftDirty || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>Save</SaveButton></div>}
    </> : imports.length ? <>
      <div data-import-candidates className="grid min-h-0 gap-2">{imports.map((candidate) => <Reveal key={candidate.id} open><div title={candidate.sourcePath} className="flex flex-wrap items-start gap-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
        <div className="flex min-w-0 flex-1 flex-col gap-1">
          <Switch label={candidate.title || candidate.sourcePath.split(/[\\/]/).pop() || candidate.sourcePath} disabled={!candidate.supported || importing} checked={candidate.supported && selectedImports.has(candidate.id)} onChange={(event) => setSelectedImports((current) => { const next = new Set(current); if (event.target.checked) next.add(candidate.id); else next.delete(candidate.id); return next })} />
          {candidate.validationError && <span className="flex items-center gap-1" style={{ color: 'var(--error)' }}><X size={14} aria-hidden className="shrink-0" />{candidate.validationError}</span>}
        </div>
        {candidate.collisionPath && <div className="w-40 shrink-0"><MenuSelect label="If a skill with this name exists" disabled={importing} value={conflicts[candidate.id] ?? 'skip'} onChange={(value) => setConflicts({ ...conflicts, [candidate.id]: value as MarkdownStoreConflictAction })} options={[{ value: 'skip', label: 'Skip' }, { value: 'replace', label: 'Replace' }, { value: 'separate', label: 'Keep both' }]} /></div>}
      </div></Reveal>)}</div>
      <div className="shrink-0 flex items-center justify-end gap-2"><Button variant="secondary" size="sm" disabled={importing} onClick={closeImportReview}>Cancel</Button><Button variant="primary" size="sm" disabled={importing || !imports.some((candidate) => candidate.supported && selectedImports.has(candidate.id))} onClick={() => void runImport()}>{importing ? 'Importing...' : 'Import selected'}</Button></div>
    </> : addView === 'choose' ? <div aria-label="Add skill choices" className="grid">
        <ProgressFill label="Reading skills" indeterminate active={previewing} className="grid">
        {[
          { label: 'Create', hint: 'Write a new skill in the app', icon: <Plus size={14} />, action: beginNew },
          { label: 'Import from provider', hint: 'Copy skills from a CLI provider', icon: <Download size={14} />, action: () => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setAddView('provider') } },
          { label: 'Import file', hint: 'Add one skill file from disk', icon: <FileInput size={14} />, action: () => void chooseImport('file') },
          { label: 'Import folder', hint: 'Add every skill in a folder', icon: <FolderInput size={14} />, action: () => void chooseImport('folder') },
        ].map((choice) => <RowButton key={choice.label} className="uam-skill-choice" aria-label={choice.label} disabled={previewing} onClick={choice.action}>
          <span className="uam-skill-choice__icon" aria-hidden>{choice.icon}</span>
          <span className="uam-skill-choice__label">{choice.label}</span>
          <span className="uam-skill-choice__hint">{choice.hint}</span>
          <ChevronRight size={14} aria-hidden />
        </RowButton>)}
        </ProgressFill>
      </div> : <>
        <SelectionGrid label="Provider" options={PROVIDER_OPTIONS} value={importProvider} onChange={(id) => { ++importPreviewRequestRef.current; setPreviewing(false); resetError(); setImportProvider(id) }} />
        <ProgressFill label="Reading skills" indeterminate active={previewing} className="flex items-center justify-end gap-2"><Button variant="primary" disabled={!importProvider || previewing} onClick={() => void loadImports({ includeProviders: true }, importProvider)}>Review imports</Button></ProgressFill>
      </>}</div></div></Swap>
  </div>

  const listView = <>
    <div className="uam-store-toolbar">
      <label className="uam-field uam-store-field flex min-w-0 flex-1 items-center gap-2 px-2.5">
        <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
        <input ref={searchRef} type="search" aria-label="Search skills" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search skills" className="min-w-0 flex-1 text-sm" style={{ border: 0, background: 'transparent', color: 'var(--text)', padding: 0, outline: 'none', boxShadow: 'none' }} />
      </label>
      <div className="shrink-0" style={{ width: 'max-content' }}>
        <MenuSelect label="Filter skills" value={filter} onChange={setFilter} options={filterOptions} />
      </div>
      <IconButton icon={<RefreshCw size={15} />} label="Refresh skills" disabled={loading || refreshing} onClick={async () => { setRefreshing(true); await runAction(refresh, 'Could not refresh Skills.'); setRefreshing(false) }} />
      <Button ref={addTriggerRef} variant="primary" size="sm" leadingIcon={<Plus size={14} aria-hidden />} disabled={!markdownStoreDirectory} onClick={openAddChoices}>New skill</Button>
    </div>
    <Swap swapKey={loading ? 'loading' : listed.length === 0 ? 'empty' : 'list'} className="flex min-h-0 flex-1 flex-col">{loading ? <SettingsLoading label="Loading skills" rows={5} /> : listed.length === 0 ? (search.trim() || filter !== 'all' || folderKey
      ? <StoreEmpty>No skills match this view.</StoreEmpty>
      : visibleError ? <div className="flex-1" />
      : <StoreEmpty action={<Button size="sm" leadingIcon={<Plus size={14} aria-hidden />} disabled={!markdownStoreDirectory} onClick={openAddChoices}>New skill</Button>}>No skills yet.</StoreEmpty>)
      : <SkillRows entries={listed} onOpen={setOpenPath} onPin={(entry) => void runAction(() => setFavorite(entry, !entry.favorite), 'Could not update the pinned skill.')} />}</Swap>
  </>

  const detailView = (entry: MarkdownStoreEntry) => <section aria-label="Skill preview" className="uam-store-detail flex min-h-0 flex-1 flex-col">
    <div className="uam-store-toolbar">
      <IconButton ref={backRef} size="sm" icon={<ArrowLeft size={15} aria-hidden />} label="Back to skills" onClick={() => setOpenPath('')} />
      <span className="min-w-0 flex-1" />
      <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${entry.title} in app`} onClick={() => beginEdit(entry)} />
      <IconButton size="sm" icon={<Paperclip size={14} />} label="Attach to message" disabled={!activeSessionId} onClick={() => attach(entry)} />
      <IconButton size="sm" icon={<ExternalLink size={14} />} label="Open in external editor" onClick={() => void runAction(() => editEntry(entry), 'Could not open the external editor.')} />
      <IconButton size="sm" icon={<FolderOpen size={14} />} label="Reveal file" onClick={() => void runAction(() => revealEntry(entry), 'Could not reveal the skill file.')} />
    </div>
    <div className="min-h-0 flex-1 overflow-y-auto px-5 py-4" style={{ overflowWrap: 'anywhere' }}>
      <h3 className="uam-store-detail__title">{entry.title}</h3>
      <div className="uam-store-detail__chips">
        {entry.sourceProvider && <span className="uam-chip uam-chip--sm"><ProviderLogo providerId={normalizeCliProviderIdAlias(entry.sourceProvider)} size={13} />{providerMetadataForId(normalizeCliProviderIdAlias(entry.sourceProvider)).name}</span>}
        {folderSegments(entry.group).length > 0 && <span className="uam-chip uam-chip--sm"><Folder size={13} aria-hidden />{folderSegments(entry.group).join(' / ')}</span>}
        {entry.commandName && <span className="uam-chip uam-chip--sm uam-store-command">/{entry.commandName}</span>}
      </div>
      <div className="uam-store-markdown"><MarkdownContent content={withoutRepeatedTitle(entry.body ?? entry.preview, entry.title)} /></div>
      <Disclosure className="uam-store-details mt-4" summary="Details">
        <dl>
          {entry.dateUpdated && <><dt>Updated</dt><dd>{formatDate(entry.dateUpdated)}</dd></>}
          <dt>File</dt><dd className="uam-store-path" title={entry.filePath}>{shortPath(entry.filePath, 2)}</dd>
        </dl>
      </Disclosure>
    </div>
  </section>

  if (childOpen) childSnapshot.current = childPanel
  const exitProps: Record<string, string | true | undefined> = stepPresence.closing ? { 'data-closing': 'true', inert: '', 'aria-hidden': 'true' } : {}

  // The folder nav stays visible beside every step; only the pane's content changes.
  const library = <div ref={libraryRef} className="flex min-h-0 flex-1">
    <nav aria-label="Skill folders" className="uam-store-nav flex shrink-0 flex-col" {...(childOpen ? { inert: '' } : {})}>
      <RowButton className="uam-store-nav__item" selected={folderKey === ''} onClick={() => setFolderKey('')}>
        <span className="uam-store-truncate flex-1">All skills</span>
        <span className="uam-store-nav__count">{entries.length}</span>
      </RowButton>
      <FolderTree nodes={folderTree} depth={0} selectedKey={folderKey} collapsed={collapsed} onSelect={setFolderKey} onToggle={toggleFolder} />
    </nav>
    <div className="uam-store-pane relative flex min-w-0 flex-1 flex-col overflow-hidden">
      {/* The list or detail slides out left while a step is open. */}
      <div className="uam-skills-library flex min-h-0 flex-1 flex-col" data-away={childOpen || undefined} aria-hidden={childOpen || undefined} {...(childOpen ? { inert: '' } : {})}>
        <Swap swapKey={opened ? 'detail' : 'list'} className="flex min-h-0 flex-1 flex-col">{opened ? detailView(opened) : listView}</Swap>
      </div>
      {/* Slides out, not unmounted, while a step is open: the library keeps its state and focus can return to its trigger. */}
      {stepPresence.mounted && childSnapshot.current && cloneElement(childSnapshot.current, exitProps)}
    </div>
  </div>

  // Overlay has no attribute passthrough, so the dialog role stays on the panel.
  const panel = <div role={embedded ? 'region' : 'dialog'} aria-modal={embedded ? undefined : true} aria-label="Skills" tabIndex={-1} className={embedded ? 'h-full min-h-0 w-full flex flex-1 flex-col overflow-hidden' : 'uam-skills-dialog w-full max-w-6xl h-[min(780px,90vh)] flex flex-col overflow-hidden rounded-xl'} style={{ background: 'var(--surface)', color: 'var(--text)' }}>
      {!embedded && <div className="shrink-0 flex items-center justify-between px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}><h2 className="text-base font-semibold">Skills</h2><IconButton icon={<X size={16} />} label="Close skills" onClick={close} /></div>}
      <div className={`flex min-h-0 flex-1 flex-col${embedded ? '' : ' p-5'}`}>
        <div className="uam-set-card uam-set-card--flush uam-store-card flex min-h-0 flex-1 flex-col">
          {/* The flush card has no side padding, so the row carries its own. */}
          <div className="px-4">
          <SettingRow
            icon={<FolderOpen size={15} aria-hidden />}
            title="Skills folder"
            description={<Tooltip label={markdownStoreDirectory || undefined}><span className="uam-store-path">{markdownStoreDirectory ? shortPath(markdownStoreDirectory, 2) : 'Not set'}</span></Tooltip>}
            control={<div style={{ width: 'max-content' }}><MenuSelect label="Change skills folder" value="" placeholder="Change…" options={FOLDER_ACTIONS} onChange={changeFolder} /></div>}
          >
            <Reveal open={pathOpen}>
              <div className="flex items-center gap-2 pb-3">
                <input aria-label="Skills directory" value={pathDraft} onChange={(event) => setPathDraft(event.target.value)} placeholder="Skills directory" className="uam-field min-w-0 flex-1" />
                <Button variant="ghost" size="sm" onClick={() => setPathOpen(false)}>Cancel</Button>
                <Button size="sm" disabled={!pathDraft.trim() || pathDraft.trim() === markdownStoreDirectory} onClick={() => void usePath()}>Use path</Button>
              </div>
            </Reveal>
          </SettingRow>
          </div>
          {/* Stage: the library and the step share one area, so the step slides over the library. */}
          <div className="flex min-h-0 flex-1 flex-col" style={{ borderTop: '1px solid var(--border)' }}>
            <Swap swapKey={markdownStoreDirectory ? 'library' : 'empty'} className="flex min-h-0 flex-1 flex-col">{!markdownStoreDirectory
              ? <StoreEmpty action={<Button variant="primary" size="sm" onClick={() => void chooseDirectory()}>Choose folder</Button>}>Choose a skills folder to get started.</StoreEmpty>
              : library}</Swap>
          </div>
        </div>
      </div>
    </div>
  return embedded ? <div className="h-full min-h-0 min-w-0 flex flex-1 flex-col overflow-hidden">{panel}</div> : <Overlay open zIndex={50}>{panel}</Overlay>
}

interface FolderNode { key: string; name: string; count: number; children: FolderNode[] }

/** Slash-separated groups are folders. Counts include subfolders; pinning still uses the persisted favorite flag. */
function folderSegments(group: string | undefined): string[] {
  return (group ?? '').split('/').map((part) => part.trim()).filter(Boolean)
}

function folderContains(key: string, entry: MarkdownStoreEntry): boolean {
  if (!key) return true
  const path = folderSegments(entry.group).join('/')
  return path === key || path.startsWith(`${key}/`)
}

function buildFolderTree(entries: MarkdownStoreEntry[]): FolderNode[] {
  const roots: FolderNode[] = []
  const nodes = new Map<string, FolderNode>()
  for (const entry of entries) {
    const segments = folderSegments(entry.group)
    segments.forEach((name, depth) => {
      const key = segments.slice(0, depth + 1).join('/')
      let node = nodes.get(key)
      if (!node) {
        node = { key, name, count: 0, children: [] }
        nodes.set(key, node)
        if (depth === 0) roots.push(node)
        else nodes.get(segments.slice(0, depth).join('/'))!.children.push(node)
      }
      node.count++
    })
  }
  const sort = (list: FolderNode[]) => {
    list.sort((a, b) => a.name.localeCompare(b.name))
    list.forEach((node) => sort(node.children))
  }
  sort(roots)
  return roots
}

/** Last segments of a path, so a long location stays on one line; the full path stays in a tooltip or title. */
function shortPath(path: string, segments: number): string {
  const parts = path.split(/[\\/]/).filter(Boolean)
  return parts.length > segments ? `…/${parts.slice(-segments).join('/')}` : path
}

function formatDate(value: string): string {
  const date = new Date(value)
  return Number.isNaN(date.getTime()) ? value : date.toLocaleDateString()
}

function StoreEmpty({ children, action }: { children: ReactNode; action?: ReactNode }) {
  return <div className="flex flex-1 flex-col items-center justify-center gap-3 px-4 text-center">
    <p className="text-sm" style={{ color: 'var(--text-3)' }}>{children}</p>
    {action}
  </div>
}

function FolderTree({ nodes, depth, selectedKey, collapsed, onSelect, onToggle }: { nodes: FolderNode[]; depth: number; selectedKey: string; collapsed: Set<string>; onSelect: (key: string) => void; onToggle: (key: string) => void }) {
  return <>
    {nodes.map((node) => {
      const open = !collapsed.has(node.key)
      return <Fragment key={node.key}>
        <div className="uam-store-nav__row" style={{ paddingLeft: depth * 14 }}>
          {node.children.length
            ? <IconButton size="sm" icon={<ChevronRight size={13} aria-hidden className={open ? 'uam-store-chevron is-open' : 'uam-store-chevron'} />} label={`${open ? 'Collapse' : 'Expand'} ${node.name}`} aria-expanded={open} onClick={() => onToggle(node.key)} />
            : <span className="uam-store-nav__spacer" aria-hidden />}
          <Tooltip label={node.key.split('/').join(' / ')}>
            <RowButton className="uam-store-nav__item" selected={selectedKey === node.key} onClick={() => onSelect(node.key)}>
              <Folder size={14} aria-hidden className="shrink-0" />
              <span className="uam-store-truncate flex-1">{node.name}</span>
              <span className="uam-store-nav__count">{node.count}</span>
            </RowButton>
          </Tooltip>
        </div>
        {open && <FolderTree nodes={node.children} depth={depth + 1} selectedKey={selectedKey} collapsed={collapsed} onSelect={onSelect} onToggle={onToggle} />}
      </Fragment>
    })}
  </>
}

/** One line per skill: title, command, muted summary, then a fixed pin column. The source shows in the detail, not on every row. */
function SkillRows({ entries, onOpen, onPin }: { entries: MarkdownStoreEntry[]; onOpen: (path: string) => void; onPin: (entry: MarkdownStoreEntry) => void }) {
  return <div aria-label="Skill list" className="flex min-h-0 flex-1 flex-col overflow-y-auto px-2 py-1">
    {entries.map((entry) => <div key={entry.filePath} className="uam-store-row">
      <RowButton className="uam-store-row__open" onClick={() => onOpen(entry.filePath)}>
        <FileText size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)' }} />
        <span className="uam-store-truncate text-sm" style={{ color: 'var(--text)', fontWeight: 'var(--fw-medium)', maxWidth: '45%' }}>{entry.title}</span>
        {entry.commandName && <span className="uam-store-command shrink-0">/{entry.commandName}</span>}
        <span className="uam-store-truncate min-w-0 flex-1 text-xs" style={{ color: 'var(--text-3)' }}>{entry.review || entry.preview}</span>
      </RowButton>
      <div className="uam-store-row__pin">
        <IconButton size="sm" className="uam-skill-pin" icon={<Pin size={14} fill={entry.favorite ? 'currentColor' : 'none'} />} active={Boolean(entry.favorite)} label={`${entry.favorite ? 'Unpin' : 'Pin'} ${entry.title}`} onClick={() => onPin(entry)} />
      </div>
    </div>)}
  </div>
}
