import { forwardRef, useCallback, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { SaveButton, SettingsField, SettingsLoading } from './SettingsPrimitives'
import './stores.css'
import './settings-shell.css'
import { ArrowLeft, ExternalLink, FolderOpen, Library, Plus, RefreshCw, Search, Trash2, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, EXIT_MS, IconButton, MenuSelect, Notice, Overlay, RowButton, motionAllowed, scaledMs, usePresence, useOverlayState } from '../ui'
import type { Folder, ExecutionHost } from '../../types/session'
import type { MemoryEntry, MemoryEntryDraft, MemoryScope } from '../../types/memory'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'

const MEMORY_CATEGORIES = [
  'Failures/AI_Failures',
  'Failures/User_Failures',
  'Lessons/AI_Lessons',
  'Lessons/User_Lessons',
] as const

const MEMORY_CONFIDENCE = ['high', 'medium', 'low'] as const
const confidenceLabel = (value: string) => value.charAt(0).toUpperCase() + value.slice(1)

const memoryFolderLabel = (folder: Folder, hosts: ExecutionHost[]) => (folder.executionHostId || 'local') === 'local' ? folder.name : `${folder.name} · ${hosts.find((host) => host.id === folder.executionHostId)?.label || folder.executionHostId}`
/** "3 hours ago" / "12 Sept" — raw ISO stamps are hard to scan. */
function formatObserved(value: string): string {
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  const minutes = Math.round((Date.now() - date.getTime()) / 60000)
  if (minutes < 1) return 'just now'
  if (minutes < 60) return `${minutes} min ago`
  if (minutes < 60 * 24) return `${Math.round(minutes / 60)} h ago`
  return date.toLocaleDateString([], { day: 'numeric', month: 'short', year: date.getFullYear() === new Date().getFullYear() ? undefined : 'numeric' })
}

const memoryCategoryLabel = (category: string) => ({
  'Failures/AI_Failures': 'AI failures',
  'Failures/User_Failures': 'Your failures',
  'Lessons/AI_Lessons': 'AI lessons',
  'Lessons/User_Lessons': 'Your lessons',
}[category] ?? category.replace(/_/g, ' ').replace('/', ' · '))

const EMPTY_DRAFT: MemoryEntryDraft = {
  category: MEMORY_CATEGORIES[2],
  title: '',
  memory: '',
  evidence: '',
  confidence: 'medium',
  sourceChatId: '',
}

interface MemoryCategoryGroup {
  category: string
  entries: MemoryEntry[]
}

interface MemoryLocationGroup {
  key: string
  label: string
  rootPath: string
  count: number
  sortIndex: number
  categories: MemoryCategoryGroup[]
  entries: MemoryEntry[]
}

interface MenuOption {
  value: string
  label: string
}

function InlineMenu({
  label,
  value,
  options,
  onChange,
}: {
  label: string
  value: string
  options: MenuOption[]
  onChange: (value: string) => void
}) {
  return (
    <SettingsField label={label}>
      <MenuSelect label={label} value={value} options={options} onChange={onChange} />
    </SettingsField>
  )
}

function memoryLocationKey(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'global'
  }

  if (entry.scopeType === 'folder' && entry.folderId) {
    return `folder:${entry.folderId}`
  }

  if (scope.scopeType === 'folder' && scope.folderId) {
    return `folder:${scope.folderId}`
  }

  const rootPath = (entry.rootPath || scope.rootPath || '').trim()
  return rootPath ? `root:${rootPath}` : `scope:${scope.scopeType}:${entry.scope || 'local'}`
}

function memoryLocationLabel(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'Global memory'
  }

  const scopeLabel = (entry.scopeLabel || '').trim()
  if (scopeLabel) {
    return scopeLabel
  }

  if (scope.scopeType === 'folder') {
    return scope.label
  }

  return entry.scope === 'global' ? 'Global memory' : 'Project memory'
}

function memoryLocationRootPath(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.rootPath) {
    return entry.rootPath
  }

  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'Global memory root'
  }

  return scope.rootPath
}

function buildMemoryLocationGroups(
  entries: MemoryEntry[],
  scope: MemoryScope | null,
  folders: Folder[],
  hosts: ExecutionHost[],
): MemoryLocationGroup[] {
  if (!scope) {
    return []
  }

  const projectFolders = folders.filter((folder) => folder.directory.trim().length > 0)
  const folderOrder = new Map(projectFolders.map((folder, index) => [folder.id, index]))
  const groups = new Map<string, {
    label: string
    rootPath: string
    sortIndex: number
    entries: MemoryEntry[]
  }>()

  if (scope.scopeType === 'all' || scope.scopeType === 'global') {
    groups.set('global', { label: 'Global memory', rootPath: scope.scopeType === 'global' ? scope.rootPath : 'Global memory root', sortIndex: -1, entries: [] })
  }
  if (scope.scopeType === 'all') {
    projectFolders.forEach((folder, index) => groups.set(`folder:${folder.id}`, {
      label: memoryFolderLabel(folder, hosts),
      rootPath: `${folder.directory.replace(/[\\/]+$/, '')}/.UAM`,
      sortIndex: index,
      entries: [],
    }))
  }

  for (const entry of entries) {
    const key = memoryLocationKey(entry, scope)
    const folderIndex = key.startsWith('folder:')
      ? folderOrder.get(key.replace(/^folder:/, ''))
      : undefined
    const sortIndex = key === 'global'
      ? -1
      : folderIndex !== undefined
        ? folderIndex
        : 10000
    const existing = groups.get(key)
    if (existing) {
      existing.entries.push(entry)
      continue
    }

    groups.set(key, {
      label: memoryLocationLabel(entry, scope),
      rootPath: memoryLocationRootPath(entry, scope),
      sortIndex,
      entries: [entry],
    })
  }

  return Array.from(groups.entries())
    .map(([key, group]) => ({
      key,
      label: group.label,
      rootPath: group.rootPath,
      count: group.entries.length,
      sortIndex: group.sortIndex,
      entries: group.entries,
      categories: MEMORY_CATEGORIES
        .map((category) => ({
          category,
          entries: group.entries.filter((entry) => entry.category === category),
        }))
        .filter((categoryGroup) => categoryGroup.entries.length > 0),
    }))
    .sort((left, right) => {
      if (left.sortIndex !== right.sortIndex) {
        return left.sortIndex - right.sortIndex
      }
      return left.label.localeCompare(right.label)
    })
}

export interface MemoryLibraryHandle { requestLeave(next: () => void): void }

// Location rows: colour follows the active state here instead of inline styles.
const LOCATION_ROW_CLASS = 'relative z-[1] flex w-full min-w-0 items-center gap-2 rounded-md px-2 py-1.5 text-left text-xs text-[var(--text-2)] transition-colors duration-[var(--dur-fast)] data-[active=true]:text-[var(--text)]'

export const MemoryLibraryModal = forwardRef<MemoryLibraryHandle, { embedded?: boolean }>(function MemoryLibraryModal({ embedded = false }, ref) {
  const liveScope = useAppStore((s) => s.memoryLibraryScope)
  const liveEntries = useAppStore(useShallow((s) => s.memoryLibraryEntries))
  const overlayState = useOverlayState()
  // Keep the last content on screen while the dialog animates closed.
  const lastLibrary = useRef({ scope: liveScope, entries: liveEntries })
  if (liveScope) lastLibrary.current = { scope: liveScope, entries: liveEntries }
  const closing = !liveScope && overlayState === 'closed'
  const memoryLibraryScope = closing ? lastLibrary.current.scope : liveScope
  const memoryLibraryEntries = closing ? lastLibrary.current.entries : liveEntries
  const memoryLibraryLoading = useAppStore((s) => s.memoryLibraryLoading)
  const memoryLibraryError = useAppStore((s) => s.memoryLibraryError)
  const closeMemoryLibrary = useAppStore((s) => s.closeMemoryLibrary)
  const refreshMemoryLibrary = useAppStore((s) => s.refreshMemoryLibrary)
  const createMemoryEntry = useAppStore((s) => s.createMemoryEntry)
  const deleteMemoryEntry = useAppStore((s) => s.deleteMemoryEntry)
  const deleteMemoryEntries = useAppStore((s) => s.deleteMemoryEntries)
  const openMemoryRoot = useAppStore((s) => s.openMemoryRoot)
  const revealMemoryEntry = useAppStore((s) => s.revealMemoryEntry)
  const folders = useAppStore(useShallow((s) => s.folders))
  const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
  const resourceCollections = useAppStore(useShallow((s) => s.resourceCollections))
  // Strings (not tuples) so useShallow can tell when nothing changed.
  const chatTitles = useAppStore(useShallow((s) => s.sessions.map((session) => `${session.id}\u0000${session.name}`)))
  const chatTitleById = useMemo(() => new Map(chatTitles.map((entry) => {
    const split = entry.indexOf('\u0000')
    return [entry.slice(0, split), entry.slice(split + 1)] as const
  })), [chatTitles])
  const [searchQuery, setSearchQuery] = useState('')
  const [isAdding, setIsAdding] = useState(false)
  const [pendingExit, setPendingExit] = useState<(() => void) | null>(null)
  const exitPrompt = usePresence(Boolean(pendingExit))
  const formPresence = usePresence(!pendingExit)
  const toolbarPresence = usePresence(!isAdding)
  const lastPendingExit = useRef<(() => void) | null>(null)
  if (pendingExit) lastPendingExit.current = pendingExit
  const libraryRef = useRef<HTMLDivElement>(null)
  const openAllMemoryLibrary = useAppStore(s => s.openAllMemoryLibrary)
  useEffect(() => {
    if (!embedded) return
    void openAllMemoryLibrary()
    return () => closeMemoryLibrary()
  }, [embedded, openAllMemoryLibrary, closeMemoryLibrary])
  const [draft, setDraft] = useState<MemoryEntryDraft>(EMPTY_DRAFT)
  const [submitting, setSubmitting] = useState(false)
  const submittingRef = useRef(false)
  const [pendingDeleteEntryId, setPendingDeleteEntryId] = useState<string | null>(null)
  // Rows collapsing out before the store removes them.
  const [removingEntryIds, setRemovingEntryIds] = useState<Set<string>>(() => new Set())
  const [pendingMassDeleteEntryIds, setPendingMassDeleteEntryIds] = useState<string[] | null>(null)
  const [selectedLocationKey, setSelectedLocationKey] = useState('')
  const [viewingEntryId, setViewingEntryId] = useState<string | null>(null)
  // Changing location always returns the pane to the list.
  const chooseLocation = (key: string) => { setSelectedLocationKey(key); setViewingEntryId(null) }
  const draftDirty = JSON.stringify(draft) !== JSON.stringify(EMPTY_DRAFT)
  const requestLeave = useCallback((next: () => void) => {
    if (submittingRef.current) return
    if (isAdding && draftDirty) setPendingExit(() => next)
    else next()
  }, [isAdding, draftDirty])
  useImperativeHandle(ref, () => ({requestLeave}), [requestLeave])
  useEffect(() => {
    if (!isAdding || !draftDirty) return
    const guard = (event: BeforeUnloadEvent) => {event.preventDefault(); event.returnValue = ''}
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [isAdding, draftDirty])
  const requestClose = useCallback(() => {
    if (submittingRef.current) return
    if (pendingDeleteEntryId) {
      setPendingDeleteEntryId(null)
      return
    }
    if (pendingMassDeleteEntryIds) {
      setPendingMassDeleteEntryIds(null)
      return
    }
    if (pendingExit) { setPendingExit(null); return }
    if (isAdding) { requestLeave(() => setIsAdding(false)); return }
    if (!embedded) closeMemoryLibrary()
  }, [closeMemoryLibrary, embedded, isAdding, pendingDeleteEntryId, pendingMassDeleteEntryIds, pendingExit, requestLeave])

  useEffect(() => {
    if (!memoryLibraryScope) {
      return
    }

    const handler = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && !event.defaultPrevented) {
        const dialogs = document.querySelectorAll<HTMLElement>('[aria-modal="true"]')
        const top = dialogs.item(dialogs.length - 1)
        if (top && !libraryRef.current?.contains(top)) return
        if (embedded && !isAdding && !pendingExit && !pendingDeleteEntryId && !pendingMassDeleteEntryIds) return
        event.preventDefault()
        requestClose()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [memoryLibraryScope, requestClose, embedded, isAdding, pendingExit, pendingDeleteEntryId, pendingMassDeleteEntryIds])

  const filteredEntries = useMemo(() => {
    const query = searchQuery.trim().toLowerCase()
    if (!query) {
      return memoryLibraryEntries
    }

    return memoryLibraryEntries.filter((entry) =>
      [
        entry.title,
        entry.category,
        entry.preview,
        entry.sourceChatId,
        entry.scopeLabel ?? '',
        entry.rootPath ?? '',
      ].some((value) => value.toLowerCase().includes(query))
    )
  }, [memoryLibraryEntries, searchQuery])

  const groupedEntries = useMemo(
    () => buildMemoryLocationGroups(filteredEntries, memoryLibraryScope, folders, executionHosts),
    [filteredEntries, folders, memoryLibraryScope, executionHosts],
  )

  // Same glide as the settings sidebar: one indicator measured against the active location.
  // ponytail: re-measures every commit (bails out when unchanged); add deps if it ever shows in a profile.
  const locationNavRef = useRef<HTMLElement>(null)
  const [locationIndicator, setLocationIndicator] = useState<{ top: number; height: number } | null>(null)
  const deleteConfirmRef = useRef<HTMLDivElement>(null)
  const deleteCancelRef = useRef<HTMLButtonElement>(null)
  // The inline confirm renders at the top of a scrolled pane; bring it into view and focus Cancel.
  useEffect(() => {
    if (!pendingDeleteEntryId) return
    deleteConfirmRef.current?.scrollIntoView?.({ block: 'nearest' })
    deleteCancelRef.current?.focus()
  }, [pendingDeleteEntryId])
  useLayoutEffect(() => {
    const nav = locationNavRef.current
    if (!nav) return
    const measure = () => {
      const active = nav.querySelector<HTMLElement>('[data-active="true"]')
      const next = active ? { top: active.offsetTop, height: active.offsetHeight } : null
      setLocationIndicator(previous => previous?.top === next?.top && previous?.height === next?.height ? previous : next)
    }
    measure()
    if (typeof ResizeObserver === 'undefined') return
    const observer = new ResizeObserver(measure)
    observer.observe(nav)
    return () => observer.disconnect()
  })
  if (!memoryLibraryScope) {
    if (!embedded) return null
    return memoryLibraryError
      ? <div role="alert" className="uam-memory-state">
          <p className="flex items-center gap-1.5 text-sm font-medium" style={{ color: 'var(--error)' }}><X size={14} aria-hidden />The memory library did not open.</p>
          <p className="text-xs" style={{ color: 'var(--text-3)' }}>{memoryLibraryError}</p>
          <div><Button size="sm" onClick={() => void openAllMemoryLibrary()}>Try again</Button></div>
        </div>
      : <SettingsLoading label="Loading memory library" rows={3} />
  }

  const isAllMemory = memoryLibraryScope.scopeType === 'all'
  const memoryTitle = memoryLibraryScope.scopeType === 'global'
    ? 'Global memory'
    : isAllMemory
      ? 'All memory'
      : `${memoryLibraryScope.label} memory`
  const targetValue = draft.targetScopeType === 'folder' && draft.targetFolderId
    ? `folder:${draft.targetFolderId}`
    : 'global'
  const targetOptions = [
    { value: 'global', label: 'Global memory' },
    ...folders
	  .filter((folder) => folder.directory.trim().length > 0)
      .map((folder) => ({ value: `folder:${folder.id}`, label: memoryFolderLabel(folder, executionHosts) })),
  ]
  const categoryOptions = MEMORY_CATEGORIES.map((category) => ({ value: category, label: memoryCategoryLabel(category) }))
  const confidenceOptions = MEMORY_CONFIDENCE.map((confidence) => ({ value: confidence, label: confidenceLabel(confidence) }))

  // Rows slide out before the delete runs; a failed delete puts them back.
  const removeWithExit = (ids: string[], remove: () => Promise<boolean>): Promise<boolean> => {
    const run = async () => {
      const ok = await remove()
      setRemovingEntryIds((current) => { const next = new Set(current); ids.forEach((id) => next.delete(id)); return next })
      return ok
    }
    if (!motionAllowed()) return run()
    setRemovingEntryIds((current) => new Set([...current, ...ids]))
    return new Promise((resolve) => window.setTimeout(() => resolve(run()), scaledMs(EXIT_MS)))
  }

  const submitDraft = async () => {
    if (submittingRef.current || !draft.title.trim() || !draft.memory.trim()) {
      return false
    }

    submittingRef.current = true
    setSubmitting(true)
    const saveDraft: MemoryEntryDraft = {
      ...draft,
      title: draft.title.trim(),
      memory: draft.memory.trim(),
      evidence: draft.evidence.trim(),
      sourceChatId: draft.sourceChatId.trim(),
    }
    if (isAllMemory && !saveDraft.targetScopeType) {
      saveDraft.targetScopeType = 'global'
      saveDraft.targetFolderId = ''
    }
    let ok = false
    let failure = ''
    try {
      ok = await createMemoryEntry(saveDraft)
    } catch (error) {
      failure = error instanceof Error ? error.message : ''
    } finally {
      submittingRef.current = false
      setSubmitting(false)
    }

    if (!ok) {
      // A store error is already shown in the Notice above; report only failures the store did not hold.
      if (!useAppStore.getState().memoryLibraryError) reportAppFailure('Could not save memory.', failure || 'Try again.')
      return false
    }

    reportAppNotice(`Saved “${saveDraft.title}”.`, 'success')
    setDraft(EMPTY_DRAFT)
    setIsAdding(false)
    return true
  }

  const pendingDelete = memoryLibraryEntries.find((entry) => entry.id === pendingDeleteEntryId) ?? null
  const pendingMassDeleteCount = pendingMassDeleteEntryIds?.length ?? 0
  const hasSearchQuery = searchQuery.trim().length > 0
  const massDeleteLabel = hasSearchQuery ? 'Delete matches' : 'Delete all'
  const requestedLocation = groupedEntries.find((location) => location.key === selectedLocationKey) ?? groupedEntries[0]
  const selectedLocation = hasSearchQuery && requestedLocation?.count === 0
    ? groupedEntries.find((location) => location.count > 0) ?? requestedLocation
    : requestedLocation
  const visibleEntryIds = selectedLocation?.entries.map((entry) => entry.id) ?? []
  const viewingEntry = viewingEntryId ? memoryLibraryEntries.find((entry) => entry.id === viewingEntryId) ?? null : null
  const groupedFolderIds = new Set(resourceCollections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === 'workspace-folder')
    .map((reference) => reference.target)))

  const panel = (
      <div
        ref={libraryRef}
        role={embedded ? "region" : "dialog"}
        aria-modal={embedded ? undefined : true}
        aria-label={memoryTitle}
        tabIndex={-1}
        className={embedded ? "flex flex-col min-h-0 h-full" : "flex w-full max-w-6xl flex-col overflow-hidden rounded-xl border border-[var(--border-bright)] bg-[var(--surface)] shadow-[var(--elev-3)] h-[min(780px,calc(100vh-2rem))]"}
        style={embedded ? { minHeight: 0 } : undefined}
      >
        {!embedded && <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <div>
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
              {memoryTitle}
            </div>
            <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
              {memoryLibraryScope.rootPath}
            </div>
          </div>
          <IconButton
            icon={<X size={16} />}
            label="Close memory library"
            onClick={requestClose}
          />
        </div>}

        <div className="grid grid-cols-[208px_minmax(0,1fr)] flex-1 min-h-0">
          <aside
            className="p-3 space-y-4 overflow-y-auto overflow-x-hidden min-w-0"
            style={{
              background: 'color-mix(in srgb, var(--surface-up) 72%, var(--surface))',
              borderRight: '1px solid var(--border)',
            }}
          >
            {groupedEntries.length > 0 && (
              <nav ref={locationNavRef} aria-label="Memory locations" className="uam-settings-nav grid gap-1 min-w-0">
                {locationIndicator && <span aria-hidden className="uam-settings-nav__indicator" style={{ transform: `translateY(${locationIndicator.top}px)`, height: locationIndicator.height }} />}
                <div className="uam-set-section__title px-2" style={{ color: 'var(--text-3)' }}>Locations</div>
                {groupedEntries.filter((location) => location.key === 'global').map((location) => {
                  const active = location.key === selectedLocation?.key
                  return (
                    <RowButton
                      key={location.key}
                      type="button"
                      data-active={active}
                      aria-current={active ? 'page' : undefined}
                      onClick={() => chooseLocation(location.key)}
                      title={location.rootPath}
                      className={LOCATION_ROW_CLASS}
                    >
                      <Library size={14} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} />
                      <span className="min-w-0 flex-1 truncate text-xs font-medium">{location.label}</span>
                      <span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span>
                    </RowButton>
                  )
                })}
                {isAllMemory && resourceCollections.map((collection) => {
                  const locations = collection.references.flatMap((reference) => {
                    if (reference.type !== 'workspace-folder') return []
                    const location = groupedEntries.find((group) => group.key === `folder:${reference.target}`)
                    return location ? [location] : []
                  })
                  if (locations.length === 0) return null
                  return (
                    <div key={collection.id} className="mt-1">
                      <div className="flex items-center gap-1.5 px-2 py-1 text-xs font-semibold" style={{ color: 'var(--text-2)' }}>
                        <Library size={13} aria-hidden />
                        <span className="truncate">{collection.name}</span>
                      </div>
                      <div className="ml-4 grid gap-0.5 border-l pl-1" style={{ borderColor: 'var(--border)' }}>
                        {locations.map((location) => {
                          const active = location.key === selectedLocation?.key
                          return <RowButton key={location.key} type="button" aria-current={active ? 'page' : undefined} onClick={() => chooseLocation(location.key)} title={location.rootPath} className={LOCATION_ROW_CLASS} data-active={active}><FolderOpen size={13} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} /><span className="min-w-0 flex-1 truncate text-xs">{location.label}</span><span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span></RowButton>
                        })}
                      </div>
                    </div>
                  )
                })}
                {isAllMemory && groupedEntries.some((location) => location.key.startsWith('folder:') && !groupedFolderIds.has(location.key.slice(7))) && (
                  <div className="mt-1">
                    <div className="uam-set-section__title uam-store-truncate px-2 py-1" title="Unassigned workspaces" style={{ color: 'var(--text-3)' }}>Unassigned workspaces</div>
                    {groupedEntries.filter((location) => location.key.startsWith('folder:') && !groupedFolderIds.has(location.key.slice(7))).map((location) => {
                      const active = location.key === selectedLocation?.key
                      return <RowButton key={location.key} type="button" aria-current={active ? 'page' : undefined} onClick={() => chooseLocation(location.key)} title={location.rootPath} className={LOCATION_ROW_CLASS} data-active={active}><FolderOpen size={13} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} /><span className="min-w-0 flex-1 truncate text-xs">{location.label}</span><span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span></RowButton>
                    })}
                  </div>
                )}
              </nav>
            )}

          </aside>

          <div className="p-5 md:p-6 overflow-y-auto min-h-0">
            {/* Delete confirmations are steps inside this dialog, like the unsaved-memory prompt; they never stack a second dialog. */}
            {pendingDelete && (
              <div ref={deleteConfirmRef} role="group" aria-label="Delete memory entry" className="uam-reveal mb-4">
                <Notice tone="warning" dismissible={false} dismissLabel="Dismiss" title="Delete memory entry?" actions={<>
                  <Button ref={deleteCancelRef} onClick={() => setPendingDeleteEntryId(null)}>Cancel</Button>
                  <Button variant="danger" onClick={() => { const entry = pendingDelete; setPendingDeleteEntryId(null); void removeWithExit([entry.id], () => deleteMemoryEntry(entry.id)).then((ok) => { if (ok) reportAppNotice(`Deleted “${entry.title}”.`, 'success') }) }}>Delete memory</Button>
                </>}>
                  This permanently deletes "{pendingDelete.title}" and its file. This cannot be undone.
                </Notice>
              </div>
            )}
            {pendingMassDeleteEntryIds && (
              <div role="group" aria-label={hasSearchQuery ? 'Delete matching memories' : 'Delete all memories'} className="uam-reveal mb-4">
                <Notice tone="warning" dismissible={false} dismissLabel="Dismiss" title={hasSearchQuery ? 'Delete matching memories?' : 'Delete all memories?'} actions={<>
                  <Button autoFocus onClick={() => setPendingMassDeleteEntryIds(null)}>Cancel</Button>
                  <Button variant="danger" onClick={() => { const count = pendingMassDeleteCount; void deleteMemoryEntries(pendingMassDeleteEntryIds).then((ok) => { if (ok) reportAppNotice(`Deleted ${count} ${count === 1 ? 'memory' : 'memories'}.`, 'success') }); setPendingMassDeleteEntryIds(null) }}>Delete {pendingMassDeleteCount} {pendingMassDeleteCount === 1 ? 'memory' : 'memories'}</Button>
                </>}>
                  This permanently deletes {pendingMassDeleteCount} memory {pendingMassDeleteCount === 1 ? 'entry' : 'entries'}. This cannot be undone.
                </Notice>
              </div>
            )}
            {toolbarPresence.mounted && <div data-state={toolbarPresence.closing ? 'closed' : 'open'} className="uam-memory-toolbar flex items-center gap-2 mb-4">
              <label className="uam-search-field uam-store-search flex min-w-0 flex-1 items-center gap-2 rounded-md px-3 bg-[var(--surface-up)] border border-[var(--border)]">
              <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
              <input
                aria-label="Search memory library"
                value={searchQuery}
                onChange={(event) => {
                  const value = event.currentTarget.value
                  setSearchQuery(value)
                }}
                placeholder="Search memory"
                className="w-full bg-transparent py-2 text-sm text-[var(--text)] outline-none"
              />
              </label>
              {!isAllMemory && <IconButton icon={<FolderOpen size={15}/>} label="Open memory root" onClick={() => void openMemoryRoot()}/>}
              <IconButton icon={<RefreshCw size={15}/>} label="Refresh memory library" disabled={memoryLibraryLoading} onClick={() => void refreshMemoryLibrary()}/>
              <IconButton icon={<Plus size={15}/>} label="Add memory" onClick={() => setIsAdding(true)}/>
              <IconButton
                variant="danger"
                icon={<Trash2 size={15} />}
                label={massDeleteLabel}
                disabled={memoryLibraryLoading || visibleEntryIds.length === 0}
                onClick={() => setPendingMassDeleteEntryIds(visibleEntryIds)}
              />
            </div>}

            {memoryLibraryError && (
              <Notice tone="error" dismissLabel="Dismiss memory error" onDismiss={() => useAppStore.setState({ memoryLibraryError: '' })}
                actions={<Button size="sm" variant="ghost" disabled={memoryLibraryLoading} onClick={() => void refreshMemoryLibrary()}>Try again</Button>}>
                {memoryLibraryError}
              </Notice>
            )}

            {isAdding ? (
              <div role="group" aria-label="New memory" className="space-y-3 uam-reveal">
              <div className="flex justify-end"><IconButton icon={<X size={16}/>} label="Close add memory" disabled={submitting} onClick={requestClose}/></div>
              {/* The unsaved-memory prompt is a step in this pane, not a second dialog. The form stays mounted under it. */}
              {exitPrompt.mounted && (
                <div role="group" aria-label="Unsaved memory" data-state={exitPrompt.closing ? 'closed' : 'open'} className="uam-memory-exit space-y-4">
                  <p className="text-sm font-semibold">Save this memory before leaving?</p>
                  <div className="flex justify-end gap-2"><Button autoFocus disabled={submitting} onClick={() => setPendingExit(null)}>Go back</Button><Button variant="danger" disabled={submitting} onClick={() => {const next=lastPendingExit.current; setPendingExit(null); setDraft(EMPTY_DRAFT); setIsAdding(false); next?.()}}>Discard</Button><Button variant="primary" disabled={submitting || !draft.title.trim() || !draft.memory.trim()} onClick={async () => { if(await submitDraft()) {const next = lastPendingExit.current; setPendingExit(null); next?.()} }}>Save and leave</Button></div>
                </div>
              )}
              <div hidden={!formPresence.mounted} data-state={formPresence.closing ? 'closed' : 'open'} className="uam-memory-form space-y-3">


                    {isAllMemory && (
                      <InlineMenu
                        label="Save to"
                        value={targetValue}
                        options={targetOptions}
                        onChange={(value) => {
                          if (value === 'global') {
                            setDraft((current) => ({ ...current, targetScopeType: 'global', targetFolderId: '' }))
                            return
                          }
                          const folderId = value.replace(/^folder:/, '')
                          setDraft((current) => ({ ...current, targetScopeType: 'folder', targetFolderId: folderId }))
                        }}
                      />
                    )}

                    <InlineMenu
                      label="Category"
                      value={draft.category}
                      options={categoryOptions}
                      onChange={(value) => {
                        setDraft((current) => ({ ...current, category: value }))
                      }}
                    />

                    <SettingsField label="Title" htmlFor="memory-draft-title">
                      <input
                        id="memory-draft-title"
                        autoFocus
                        aria-label="Title"
                        value={draft.title}
                        onChange={(event) => {
                          const value = event.currentTarget.value
                          setDraft((current) => ({ ...current, title: value }))
                        }}
                        className="uam-field"
                      />
                    </SettingsField>

                    <SettingsField label="Memory" htmlFor="memory-draft-memory">
                      <textarea
                        id="memory-draft-memory"
                        value={draft.memory}
                        onChange={(event) => {
                          const value = event.currentTarget.value
                          setDraft((current) => ({ ...current, memory: value }))
                        }}
                        rows={5}
                        className="uam-field uam-field--area"
                      />
                    </SettingsField>

                    <SettingsField label="Evidence" htmlFor="memory-draft-evidence">
                      <textarea
                        id="memory-draft-evidence"
                        value={draft.evidence}
                        onChange={(event) => {
                          const value = event.currentTarget.value
                          setDraft((current) => ({ ...current, evidence: value }))
                        }}
                        rows={3}
                        className="uam-field uam-field--area"
                      />
                    </SettingsField>

                    <div className="grid gap-2">
                      <InlineMenu
                        label="Confidence"
                        value={draft.confidence}
                        options={confidenceOptions}
                        onChange={(value) => {
                          setDraft((current) => ({ ...current, confidence: value }))
                        }}
                      />

                      <SettingsField label="Source chat" htmlFor="memory-draft-source-chat">
                        <input
                          id="memory-draft-source-chat"
                          value={draft.sourceChatId}
                          onChange={(event) => {
                            const value = event.currentTarget.value
                            setDraft((current) => ({ ...current, sourceChatId: value }))
                          }}
                          className="uam-field"
                        />
                      </SettingsField>
                    </div>

                    <SaveButton
                      saving={submitting}
                      pendingLabel="Saving..."
                      block
                      disabled={submitting || !draft.title.trim() || !draft.memory.trim()}
                      onClick={() => void submitDraft()}
                    >
                      Save memory
                    </SaveButton>
              </div>
              </div>
            ) : viewingEntry ? (
              <div key={viewingEntry.id} className="space-y-4 uam-reveal">
                <div className="flex items-center gap-2">
                  <Button size="sm" variant="ghost" onClick={() => setViewingEntryId(null)}><ArrowLeft size={14} aria-hidden />Back</Button>
                  <div className="flex-1" />
                  <IconButton size="sm" icon={<ExternalLink size={14} />} label={`Reveal ${viewingEntry.title} file`} onClick={() => void revealMemoryEntry(viewingEntry.id)} />
                  <IconButton size="sm" icon={<Trash2 size={14} />} label={`Delete ${viewingEntry.title}`} variant="danger" onClick={() => setPendingDeleteEntryId(viewingEntry.id)} />
                </div>
                <div className="text-base font-semibold" style={{ color: 'var(--text)' }}>{viewingEntry.title}</div>
                <dl className="grid grid-cols-[max-content_minmax(0,1fr)] gap-x-4 gap-y-1 text-xs" style={{ color: 'var(--text-3)' }}>
                  <dt>Category</dt><dd style={{ color: 'var(--text-2)' }}>{memoryCategoryLabel(viewingEntry.category)}</dd>
                  <dt>Confidence</dt><dd style={{ color: 'var(--text-2)' }}>{confidenceLabel(viewingEntry.confidence)}</dd>
                  <dt>Occurrences</dt><dd style={{ color: 'var(--text-2)' }}>{viewingEntry.occurrenceCount}</dd>
                  <dt>Scope</dt><dd style={{ color: 'var(--text-2)' }}>{memoryLocationLabel(viewingEntry, memoryLibraryScope)}</dd>
                  {viewingEntry.sourceChatId && <><dt>Source chat</dt><dd style={{ color: 'var(--text-2)' }}>{chatTitleById.get(viewingEntry.sourceChatId) ?? viewingEntry.sourceChatId}</dd></>}
                  {viewingEntry.lastObserved && <><dt>Last seen</dt><dd style={{ color: 'var(--text-2)' }} title={viewingEntry.lastObserved}>{formatObserved(viewingEntry.lastObserved)}</dd></>}
                  <dt>File</dt><dd className="uam-store-path" style={{ color: 'var(--text-2)' }}>{viewingEntry.filePath}</dd>
                </dl>
                <p className="uam-store-markdown whitespace-pre-wrap">{viewingEntry.preview || 'No preview available.'}</p>
              </div>
            ) : memoryLibraryLoading ? (
              <SettingsLoading label="Loading memory library" rows={4} />
            ) : !selectedLocation || selectedLocation.entries.length === 0 ? (
              memoryLibraryError ? null : <div className="grid justify-items-start gap-3">
                <p className="text-sm" style={{ color: 'var(--text-3)' }}>{hasSearchQuery ? 'No memory matches this search.' : 'No memory saved here yet.'}</p>
                {!hasSearchQuery && <Button variant="primary" size="sm" onClick={() => setIsAdding(true)}>Add memory</Button>}
              </div>
            ) : selectedLocation ? (
              <div key={selectedLocation.key} className="space-y-4 uam-reveal">
                <div className="min-w-0">
                  <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>{selectedLocation.label}</div>
                  <div className="mt-0.5 text-xs truncate" style={{ color: 'var(--text-3)' }}>{selectedLocation.rootPath}</div>
                </div>
                {selectedLocation.categories.map(({ category, entries }) => (
                          <section key={`${selectedLocation.key}:${category}`}>
                            <div className="flex items-center justify-between gap-2 mb-2">
                              <div className="uam-set-section__title" style={{ color: 'var(--text-3)' }}>
                                {memoryCategoryLabel(category)}
                              </div>
                              <div className="text-xs tabular-nums rounded-full px-2 py-0.5" style={{ background: 'var(--surface-up)', color: 'var(--text-3)' }}>
                                {entries.length}
                              </div>
                            </div>
                            <div className="grid gap-2">
                              {entries.map((entry) => (
                                <div key={entry.id} className="uam-memory-row" data-state={removingEntryIds.has(entry.id) ? 'closed' : 'open'}>
                                  <RowButton className="uam-memory-row__open" onClick={() => setViewingEntryId(entry.id)}>
                                    <span className="uam-memory-row__title">{entry.title}</span>
                                    <span className="uam-memory-row__summary">{entry.preview || 'No preview available.'}</span>
                                  </RowButton>
                                  <span className="uam-memory-row__meta">
                                    <span>{memoryLocationLabel(entry, memoryLibraryScope)}</span>
                                    {entry.lastObserved && <span title={entry.lastObserved}>{formatObserved(entry.lastObserved)}</span>}
                                  </span>
                                  <div className="uam-memory-row__actions flex items-center gap-1">
                                    <IconButton size="sm" icon={<ExternalLink size={14} />} label={`Reveal ${entry.title} file`} onClick={() => void revealMemoryEntry(entry.id)} />
                                    <IconButton size="sm" icon={<Trash2 size={14} />} label={`Delete ${entry.title}`} variant="danger" onClick={() => setPendingDeleteEntryId(entry.id)} />
                                  </div>
                                </div>
                              ))}
                            </div>
                          </section>
                        ))}
              </div>
            ) : null}
          </div>
        </div>
      </div>
  )
  // Embedded in Settings it sits in the page; standalone it is an Overlay dialog.
  return embedded
    ? <div className="h-full min-h-0">{panel}</div>
    : <Overlay open zIndex={50} onBackdropMouseDown={requestClose}>{panel}</Overlay>
})
