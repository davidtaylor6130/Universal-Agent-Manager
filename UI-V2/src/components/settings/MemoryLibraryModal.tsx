import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ChevronDown, ExternalLink, FolderOpen, Library, Plus, RefreshCw, Search, SearchX, Trash2, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, IconButton, MenuSelect } from '../ui'
import type { Folder } from '../../types/session'
import type { MemoryEntry, MemoryEntryDraft, MemoryScope } from '../../types/memory'

const MEMORY_CATEGORIES = [
  'Failures/AI_Failures',
  'Failures/User_Failures',
  'Lessons/AI_Lessons',
  'Lessons/User_Lessons',
] as const

const MEMORY_CONFIDENCE = ['high', 'medium', 'low'] as const

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
    <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
      <span>{label}</span>
      <MenuSelect label={label} value={value} options={options} onChange={onChange} />
    </div>
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
): MemoryLocationGroup[] {
  if (!scope) {
    return []
  }

  const folderOrder = new Map(folders.map((folder, index) => [folder.id, index]))
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
    folders.forEach((folder, index) => groups.set(`folder:${folder.id}`, {
      label: folder.name,
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

export function MemoryLibraryModal() {
  const memoryLibraryScope = useAppStore((s) => s.memoryLibraryScope)
  const memoryLibraryEntries = useAppStore(useShallow((s) => s.memoryLibraryEntries))
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
  const resourceCollections = useAppStore(useShallow((s) => s.resourceCollections))
  const [searchQuery, setSearchQuery] = useState('')
  const [isAdding, setIsAdding] = useState(false)
  const [draft, setDraft] = useState<MemoryEntryDraft>(EMPTY_DRAFT)
  const [submitting, setSubmitting] = useState(false)
  const submittingRef = useRef(false)
  const [pendingDeleteEntryId, setPendingDeleteEntryId] = useState<string | null>(null)
  const [pendingMassDeleteEntryIds, setPendingMassDeleteEntryIds] = useState<string[] | null>(null)
  const [selectedLocationKey, setSelectedLocationKey] = useState('')
  const draftDirty = Object.entries(draft).some(([key, value]) => key !== 'category' && key !== 'confidence' && Boolean(value))
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
    if (isAdding && draftDirty) {
      setIsAdding(false)
      return
    }
    closeMemoryLibrary()
  }, [closeMemoryLibrary, draftDirty, isAdding, pendingDeleteEntryId, pendingMassDeleteEntryIds])

  useEffect(() => {
    if (!memoryLibraryScope) {
      return
    }

    const handler = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        requestClose()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [memoryLibraryScope, requestClose])

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
    () => buildMemoryLocationGroups(filteredEntries, memoryLibraryScope, folders),
    [filteredEntries, folders, memoryLibraryScope],
  )

  if (!memoryLibraryScope) {
    return null
  }

  const isAllMemory = memoryLibraryScope.scopeType === 'all'
  const memoryTitle = memoryLibraryScope.scopeType === 'global'
    ? 'Global Memory'
    : isAllMemory
      ? 'All Memory'
      : `${memoryLibraryScope.label} Memory`
  const targetValue = draft.targetScopeType === 'folder' && draft.targetFolderId
    ? `folder:${draft.targetFolderId}`
    : 'global'
  const targetOptions = [
    { value: 'global', label: 'Global memory' },
    ...folders
      .filter((folder) => folder.directory.trim().length > 0)
      .map((folder) => ({ value: `folder:${folder.id}`, label: folder.name })),
  ]
  const categoryOptions = MEMORY_CATEGORIES.map((category) => ({ value: category, label: memoryCategoryLabel(category) }))
  const confidenceOptions = MEMORY_CONFIDENCE.map((confidence) => ({ value: confidence, label: confidence }))

  const submitDraft = async () => {
    if (submittingRef.current || !draft.title.trim() || !draft.memory.trim()) {
      return
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
    try {
      ok = await createMemoryEntry(saveDraft)
    } finally {
      submittingRef.current = false
      setSubmitting(false)
    }

    if (!ok) {
      return
    }

    setDraft(EMPTY_DRAFT)
    setIsAdding(false)
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
  const groupedFolderIds = new Set(resourceCollections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === 'workspace-folder')
    .map((reference) => reference.target)))

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(event) => {
        if (event.target === event.currentTarget) requestClose()
      }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label={memoryTitle}
        tabIndex={-1}
        className="rounded-2xl shadow-2xl w-full max-w-6xl mx-4 animate-slide-in overflow-hidden flex flex-col"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', height: 'min(780px, calc(100vh - 2rem))' }}
      >
        <div
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
        </div>

        <div className="grid lg:grid-cols-[320px_minmax(0,1fr)] min-h-[620px] flex-1 min-h-0">
          <aside
            className="p-5 space-y-4 overflow-y-auto"
            style={{
              background: 'color-mix(in srgb, var(--surface-up) 72%, var(--surface))',
              borderRight: '1px solid var(--border)',
            }}
          >
            <div className="flex items-center gap-1">
              {!isAllMemory && (
                <IconButton icon={<FolderOpen size={15} />} label="Open memory root" onClick={() => void openMemoryRoot()} />
              )}
              <IconButton icon={<RefreshCw size={15} className={memoryLibraryLoading ? 'animate-spin' : undefined} />} label="Refresh memory library" disabled={memoryLibraryLoading} onClick={() => void refreshMemoryLibrary()} />
              <IconButton icon={isAdding ? <X size={15} /> : <Plus size={15} />} label={isAdding ? 'Close add form' : 'Add memory'} variant={isAdding ? 'ghost' : 'solid'} active={isAdding} onClick={() => setIsAdding((value) => !value)} />
            </div>

            {groupedEntries.length > 0 && (
              <nav aria-label="Memory locations" className="grid gap-1">
                <div className="px-2 text-xs font-semibold uppercase tracking-[0.16em]" style={{ color: 'var(--text-3)' }}>Locations</div>
                {groupedEntries.filter((location) => location.key === 'global').map((location) => {
                  const active = location.key === selectedLocation?.key
                  return (
                    <button
                      key={location.key}
                      type="button"
                      aria-current={active ? 'page' : undefined}
                      onClick={() => setSelectedLocationKey(location.key)}
                      className="flex items-center gap-2 rounded-md px-2 py-2 text-left transition-colors duration-150"
                      style={{ background: active ? 'var(--accent-dim)' : 'transparent', color: active ? 'var(--text)' : 'var(--text-2)', border: 0 }}
                    >
                      <Library size={14} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} />
                      <span className="min-w-0 flex-1">
                        <span className="block truncate text-xs font-medium">{location.label}</span>
                        <span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{location.rootPath}</span>
                      </span>
                      <span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span>
                    </button>
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
                        <ChevronDown size={13} aria-hidden className="transition-transform duration-150" />
                        <Library size={13} aria-hidden />
                        <span className="truncate">{collection.name}</span>
                      </div>
                      <div className="ml-4 grid gap-0.5 border-l pl-1" style={{ borderColor: 'var(--border)' }}>
                        {locations.map((location) => {
                          const active = location.key === selectedLocation?.key
                          return <button key={location.key} type="button" aria-current={active ? 'page' : undefined} onClick={() => setSelectedLocationKey(location.key)} className="flex items-center gap-2 rounded-md px-2 py-1.5 text-left transition-colors duration-150" style={{ background: active ? 'var(--accent-dim)' : 'transparent', color: active ? 'var(--text)' : 'var(--text-2)', border: 0 }}><FolderOpen size={13} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} /><span className="min-w-0 flex-1 truncate text-xs">{location.label}</span><span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span></button>
                        })}
                      </div>
                    </div>
                  )
                })}
                {isAllMemory && groupedEntries.some((location) => location.key.startsWith('folder:') && !groupedFolderIds.has(location.key.slice(7))) && (
                  <div className="mt-1">
                    <div className="px-2 py-1 text-xs font-semibold uppercase tracking-[0.12em]" style={{ color: 'var(--text-3)' }}>Unassigned workspaces</div>
                    {groupedEntries.filter((location) => location.key.startsWith('folder:') && !groupedFolderIds.has(location.key.slice(7))).map((location) => {
                      const active = location.key === selectedLocation?.key
                      return <button key={location.key} type="button" aria-current={active ? 'page' : undefined} onClick={() => setSelectedLocationKey(location.key)} className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left transition-colors duration-150" style={{ background: active ? 'var(--accent-dim)' : 'transparent', color: active ? 'var(--text)' : 'var(--text-2)', border: 0 }}><FolderOpen size={13} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} /><span className="min-w-0 flex-1 truncate text-xs">{location.label}</span><span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{location.count}</span></button>
                    })}
                  </div>
                )}
              </nav>
            )}

            {isAdding && (
              <div
                className="rounded-xl p-3 space-y-3"
                style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}
              >
                <div className="text-xs font-semibold" style={{ color: 'var(--text)' }}>
                  New memory
                </div>

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

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Title
                  <input
                    value={draft.title}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, title: value }))
                    }}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                  />
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Memory
                  <textarea
                    value={draft.memory}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, memory: value }))
                    }}
                    rows={5}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px', resize: 'vertical' }}
                  />
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Evidence
                  <textarea
                    value={draft.evidence}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, evidence: value }))
                    }}
                    rows={3}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px', resize: 'vertical' }}
                  />
                </label>

                <div className="grid gap-2">
                  <InlineMenu
                    label="Confidence"
                    value={draft.confidence}
                    options={confidenceOptions}
                    onChange={(value) => {
                      setDraft((current) => ({ ...current, confidence: value }))
                    }}
                  />

                  <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                    Source chat
                    <input
                      value={draft.sourceChatId}
                      onChange={(event) => {
                        const value = event.currentTarget.value
                        setDraft((current) => ({ ...current, sourceChatId: value }))
                      }}
                      style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                    />
                  </label>
                </div>

                <Button
                  variant="primary"
                  block
                  size="sm"
                  disabled={submitting || !draft.title.trim() || !draft.memory.trim()}
                  onClick={() => void submitDraft()}
                >
                  {submitting ? 'Saving...' : 'Save memory'}
                </Button>
              </div>
            )}
          </aside>

          <div className="p-5 md:p-6 overflow-y-auto min-h-0">
            <div className="flex items-center gap-2 mb-4">
              <label className="flex min-w-0 flex-1 items-center gap-2 rounded-md px-3" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
              <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
              <input
                aria-label="Search memory library"
                value={searchQuery}
                onChange={(event) => {
                  const value = event.currentTarget.value
                  setSearchQuery(value)
                }}
                placeholder="Search memory titles, categories, previews, or source chats"
                className="w-full py-2 text-sm outline-none"
                style={{ background: 'transparent', color: 'var(--text)', border: 0 }}
              />
              </label>
              <IconButton
                variant="danger"
                icon={<Trash2 size={15} />}
                label={massDeleteLabel}
                disabled={memoryLibraryLoading || visibleEntryIds.length === 0}
                onClick={() => setPendingMassDeleteEntryIds(visibleEntryIds)}
              />
            </div>

            {memoryLibraryError && (
              <div className="mb-4 rounded-md px-3 py-2 text-xs" style={{ background: 'color-mix(in srgb, var(--red) 14%, transparent)', color: 'var(--red)', border: '1px solid color-mix(in srgb, var(--red) 35%, var(--border))' }}>
                {memoryLibraryError}
              </div>
            )}

            {memoryLibraryLoading ? (
              <div className="text-sm" style={{ color: 'var(--text-3)' }}>
                Loading memory library...
              </div>
            ) : !selectedLocation || selectedLocation.entries.length === 0 ? (
              <div
                className="rounded-xl p-8 text-center flex flex-col items-center gap-2 animate-fade-in"
                style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text-3)' }}
              >
                {hasSearchQuery
                  ? <SearchX size={24} strokeWidth={1.5} aria-hidden />
                  : <FolderOpen size={24} strokeWidth={1.5} aria-hidden />}
                <div className="text-sm font-medium" style={{ color: 'var(--text-2)' }}>
                  {hasSearchQuery ? 'No memory matches this search' : 'No memory saved here yet'}
                </div>
                <div className="text-xs">
                  {hasSearchQuery ? 'Try another term or clear the search.' : 'Add a memory to make it available to future chats.'}
                </div>
              </div>
            ) : selectedLocation ? (
              <div key={selectedLocation.key} className="space-y-4 animate-fade-in">
                <div className="min-w-0">
                  <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>{selectedLocation.label}</div>
                  <div className="mt-0.5 text-xs truncate" style={{ color: 'var(--text-3)' }}>{selectedLocation.rootPath}</div>
                </div>
                {selectedLocation.categories.map(({ category, entries }) => (
                          <section key={`${selectedLocation.key}:${category}`}>
                            <div className="flex items-center justify-between gap-2 mb-2">
                              <div className="text-xs font-semibold uppercase tracking-[0.16em]" style={{ color: 'var(--text-3)' }}>
                                {memoryCategoryLabel(category)}
                              </div>
                              <div className="text-xs rounded px-1.5 py-0.5" style={{ background: 'var(--surface-up)', color: 'var(--text-3)', border: '1px solid var(--border)' }}>
                                {entries.length}
                              </div>
                            </div>
                            <div className="space-y-3">
                              {entries.map((entry) => (
                                <article
                                  key={entry.id}
                                  className="p-3 transition-colors duration-150 hover:bg-[var(--surface-up)]"
                                  style={{ borderBottom: '1px solid var(--border)' }}
                                >
                                  <div className="flex items-start justify-between gap-4">
                                    <div className="min-w-0">
                                      <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
                                        {entry.title}
                                      </div>
                                      <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
                                        {entry.scope} • {entry.confidence} confidence • {entry.occurrenceCount} occurrence{entry.occurrenceCount === 1 ? '' : 's'}
                                      </div>
                                    </div>
                                    <div className="flex items-center gap-2">
                                      <IconButton icon={<ExternalLink size={14} />} label={`Reveal ${entry.title} file`} onClick={() => void revealMemoryEntry(entry.id)} />
                                      <IconButton icon={<Trash2 size={14} />} label={`Delete ${entry.title}`} variant="danger" onClick={() => setPendingDeleteEntryId(entry.id)} />
                                    </div>
                                  </div>

                                  <div className="grid md:grid-cols-2 gap-2 mt-3 text-xs" style={{ color: 'var(--text-3)' }}>
                                    <div>Source chat: {entry.sourceChatId || '—'}</div>
                                    <div>Last observed: {entry.lastObserved || '—'}</div>
                                  </div>

                                  <div className="mt-3 text-sm leading-6" style={{ color: 'var(--text-2)' }}>
                                    {entry.preview || 'No preview available.'}
                                  </div>
                                </article>
                              ))}
                            </div>
                          </section>
                        ))}
              </div>
            ) : null}
          </div>
        </div>
      </div>

      {pendingDelete && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center"
          style={{ background: 'rgba(0,0,0,0.25)' }}
          onClick={(event) => {
            if (event.target === event.currentTarget) setPendingDeleteEntryId(null)
          }}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-label="Delete memory entry"
            tabIndex={-1}
            className="rounded-xl shadow-2xl w-full max-w-md mx-4"
            style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)' }}
          >
            <div className="px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                Delete memory entry?
              </div>
            </div>
            <div className="p-5 space-y-4">
              <div className="text-sm" style={{ color: 'var(--text)' }}>
                This permanently removes "{pendingDelete.title}" and deletes its backing markdown file. This cannot be undone or restored.
              </div>
            </div>
            <div className="flex items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button
                variant="secondary"
                size="sm"
                onClick={() => setPendingDeleteEntryId(null)}
              >
                Cancel
              </Button>
              <Button
                variant="danger"
                size="sm"
                onClick={() => {
                  void deleteMemoryEntry(pendingDelete.id)
                  setPendingDeleteEntryId(null)
                }}
              >
                Delete memory
              </Button>
            </div>
          </div>
        </div>
      )}

      {pendingMassDeleteEntryIds && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center"
          style={{ background: 'rgba(0,0,0,0.25)' }}
          onClick={(event) => {
            if (event.target === event.currentTarget) setPendingMassDeleteEntryIds(null)
          }}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-label={hasSearchQuery ? 'Delete matching memories' : 'Delete all memories'}
            tabIndex={-1}
            className="rounded-xl shadow-2xl w-full max-w-md mx-4"
            style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)' }}
          >
            <div className="px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                {hasSearchQuery ? 'Delete matching memories?' : 'Delete all memories?'}
              </div>
            </div>
            <div className="p-5 space-y-4">
              <div className="text-sm" style={{ color: 'var(--text)' }}>
                This permanently deletes {pendingMassDeleteCount} {hasSearchQuery ? 'matching' : 'listed'} memory {pendingMassDeleteCount === 1 ? 'entry' : 'entries'} and removes the backing markdown {pendingMassDeleteCount === 1 ? 'file' : 'files'}. This cannot be undone or restored.
              </div>
            </div>
            <div className="flex items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button
                variant="secondary"
                size="sm"
                onClick={() => setPendingMassDeleteEntryIds(null)}
              >
                Cancel
              </Button>
              <Button
                variant="danger"
                size="sm"
                onClick={() => {
                  void deleteMemoryEntries(pendingMassDeleteEntryIds)
                  setPendingMassDeleteEntryIds(null)
                }}
              >
                Delete {pendingMassDeleteCount} {pendingMassDeleteCount === 1 ? 'memory' : 'memories'}
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
