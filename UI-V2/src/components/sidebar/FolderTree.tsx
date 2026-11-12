import { OrphanSubagentItem, SubagentSearchMatches, SubagentSessionItem } from './SubagentSessionItem'
import { hasNativeSubagentLineage, subscribeSubagentNavigation, subagentNavigationRevision } from '../../utils/subagentNavigation'
import { useSnoozeClock } from './useSnoozeClock'
import { CustomIcon, useCustomIconPicker } from '../shared/CustomIcon'
import { isCompanionContext } from '../../ipc/cefBridge'
import { reportAppFailure } from '../../utils/appFailure'
import { memo, useCallback, useEffect, useLayoutEffect, useRef, useState, useMemo, useSyncExternalStore } from 'react'
import type { MouseEvent as ReactMouseEvent, PointerEvent as ReactPointerEvent, ReactNode, RefObject } from 'react'
import { automationStartPending, subscribeOutgoingAttempts } from '../../utils/outgoingMessagePresentation'
import { Check, Plus, Folder as FolderIcon, FolderOpen as FolderOpenIcon, FolderSync, X, MoreHorizontal, MessageSquarePlus, Pencil, Trash2, TriangleAlert, ChevronRight, Library, SearchX, RefreshCw, Monitor, Server } from 'lucide-react'
import type { LucideIcon } from 'lucide-react'
import { useAppStore, type AcpAttentionKind } from '../../store/useAppStore'
import type { AppState } from '../../store/storeTypes'
import { useShallow } from 'zustand/react/shallow'
import { shallow } from 'zustand/shallow'
import { SessionItem } from './SessionItem'
import { DeleteConfirmDialog, chatDeleteFailure, countLabel, relatedBranchContents } from './DeleteConfirmDialog'
import './sidebar.css'
import { Button, ConfirmDialog, IconButton, MenuItem, MenuSelect, Notice, ProgressFill, RowButton, Tooltip, ViewportMenu } from '../ui'
import {
  type ChatSearchFilters,
  type ChatSearchFilterContext,
  buildChatSearchIndex,
  buildChatSearchModel,
  tokenizeChatSearchQuery,
} from './chatSearch'
import type { Folder, Session, WorkspaceFolderRecoveryPreview } from '../../types/session'
import { chatGridLeaves, chatPaneColors, readChatGridLayout, setChatInLeaf, subscribeChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'
import { CollectionMenuItems, moveFolderToCollection } from './CollectionMenuItems'
import { RowSubmenu } from './RowSubmenu'
import { applyManualOrder, ChatSortContext, glideAfter, isSorting, MAX_SORTED_CHATS, startSortableDrag, type ChatSortList } from './sortableDrag'
import { ChatSelectionContext, MenuDivider, SingleOnlyMenuItem, type ChatSelection } from './bulkActions'
import { runForEach, type BulkTarget } from '../../utils/runForEach'
import type { ResourceCollection } from '../../types/resourceCollection'
import { canNestCollection, collectionPath, collectionSubtreeIds } from '../../utils/collectionTree'
import { fromViewportMenu } from '../../utils/modalFocus'
import { isAbsoluteRemoteWorkspace, isRemoteDirectoryBrowseAvailable, isSshHost } from '../../utils/remoteWorkspace'
import { RemoteDirectoryBrowser } from './RemoteDirectoryBrowser'
import { Overlay, usePresence } from '../ui/Overlay'
import { SSH_ENABLED } from '../../config/buildFeatures'

interface FolderTreeProps {
  onSessionOpen?: (sessionId: string) => void
  searchQuery: string
  deepSearchSessionIds?: string[]
  filters?: ChatSearchFilters
}

const VISIBLE_SESSION_LIMIT = 5
const EMPTY_SEARCH_INDEX: Record<string, string> = {}
const EMPTY_SELECTION_IDS = new Set<string>()
type FolderDropEdge = 'before' | 'after'
type CompactRuntimeStatus = '' | 'processing' | 'done' | `attention:${AcpAttentionKind}`

// Runtime bindings are immutable store values. Cache their compact sidebar status so
// streaming payload changes only inspect the replaced binding, not every historical chat.
const cliRuntimeStatusCache = new WeakMap<object, CompactRuntimeStatus>()
const acpRuntimeStatusCache = new WeakMap<object, CompactRuntimeStatus>()

function cliRuntimeStatus(binding: ReturnType<typeof useAppStore.getState>['cliBindingBySessionId'][string] | undefined) {
  if (!binding) return ''
  const cached = cliRuntimeStatusCache.get(binding)
  if (cached !== undefined) return cached
  const status: CompactRuntimeStatus = binding.processing || binding.lifecycleState === 'starting' || binding.lifecycleState === 'busy' || binding.lifecycleState === 'shuttingDown'
    ? 'processing'
    : binding.readySinceLastSelect ? 'done' : ''
  cliRuntimeStatusCache.set(binding, status)
  return status
}

function acpRuntimeStatus(binding: ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] | undefined) {
  if (!binding) return ''
  const cached = acpRuntimeStatusCache.get(binding)
  if (cached !== undefined) return cached
  const attentionKind = binding.attentionKind && binding.attentionKind !== 'error' ? binding.attentionKind : null
  const status: CompactRuntimeStatus = attentionKind
    ? `attention:${attentionKind}`
    : binding.processing || (binding.backgroundTaskCount ?? 0) > 0 || binding.lifecycleState === 'starting' || binding.lifecycleState === 'waitingPermission'
      ? 'processing'
      : binding.readySinceLastSelect ? 'done' : ''
  acpRuntimeStatusCache.set(binding, status)
  return status
}

function combinedRuntimeStatus(
  cliBinding: ReturnType<typeof useAppStore.getState>['cliBindingBySessionId'][string] | undefined,
  acpBinding: ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] | undefined,
): CompactRuntimeStatus {
  const acpStatus = acpRuntimeStatus(acpBinding)
  if (acpStatus.startsWith('attention:')) return acpStatus
  const cliStatus = cliRuntimeStatus(cliBinding)
  if (acpStatus === 'processing' || cliStatus === 'processing') return 'processing'
  return acpStatus === 'done' || cliStatus === 'done' ? 'done' : ''
}

function moveId(ids: string[], sourceId: string, targetId: string, edge: FolderDropEdge) {
  const reordered = ids.filter((id) => id !== sourceId)
  const targetIndex = reordered.indexOf(targetId)
  if (targetIndex < 0 || reordered.length === ids.length) return ids
  reordered.splice(targetIndex + (edge === 'after' ? 1 : 0), 0, sourceId)
  return reordered
}

function reorderCollectionFolderReferences(
  collection: ResourceCollection,
  sourceFolderId: string,
  targetFolderId: string,
  edge: FolderDropEdge,
) {
  const folderReferences = collection.references.filter((reference) => reference.type === 'workspace-folder')
  const sourceReference = folderReferences.find((reference) => reference.target === sourceFolderId)
  const targetReference = folderReferences.find((reference) => reference.target === targetFolderId)
  if (!sourceReference || !targetReference) return null

  const reorderedFolderIds = moveId(
    folderReferences.map((reference) => reference.id),
    sourceReference.id,
    targetReference.id,
    edge,
  )
  let folderIndex = 0
  return collection.references.map((reference) =>
    reference.type === 'workspace-folder' ? reorderedFolderIds[folderIndex++] : reference.id
  )
}

function SidebarSectionHeader({ label, listId, expanded, onToggle, trailing, action }: {
  label: string
  listId: string
  expanded: boolean
  onToggle: () => void
  trailing?: ReactNode
  /** Rendered beside the toggle, not inside it, so it can be its own button. */
  action?: ReactNode
}) {
  const toggle = (
    <RowButton
      type="button"
      aria-label={`${expanded ? 'Collapse' : 'Expand'} ${label}`}
      aria-expanded={expanded}
      aria-controls={listId}
      className="mx-1 flex w-[calc(100%-0.5rem)] items-center gap-1.5 rounded-md px-1.5 py-0.5 text-left hover:bg-[var(--sidebar-item-hover)] focus-visible:[outline-offset:var(--focus-inset)]"
      style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer' }}
      onClick={onToggle}
    >
      <ChevronRight
        size={13}
        className="uam-sb-chevron"
        style={{ flexShrink: 0, transform: expanded ? 'rotate(90deg)' : 'rotate(0deg)' }}
        aria-hidden
      />
      <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
        {label}
      </span>
      {trailing}
    </RowButton>
  )
  return action ? <div className="group/section relative flex items-center">{toggle}<div className="absolute right-2 hidden group-hover/section:flex group-focus-within/section:flex">{action}</div></div> : toggle
}

const countAutomationStarts = (state: AppState) => state.sessions.filter((chat) => automationStartPending(chat, state.acpBindingBySessionId[chat.id]?.pendingDirectPrompts)).length

export function FolderTree({ searchQuery, deepSearchSessionIds, filters, onSessionOpen }: FolderTreeProps) {
  const navigationRevision = useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  const companionContext = isCompanionContext()
  const folders = useAppStore(useShallow((s) => s.folders))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
  const resourceCollections = useAppStore(useShallow((s) => s.resourceCollections))
  // Attempts live outside the app store, so their changes need their own re-render; the selector then reads them.
  useSyncExternalStore(subscribeOutgoingAttempts, () => countAutomationStarts(useAppStore.getState()))
  const runtimeStatuses = useAppStore(useShallow((s) => sessions.map((chat) => {
    const status = combinedRuntimeStatus(s.cliBindingBySessionId[chat.id], s.acpBindingBySessionId[chat.id])
    return (!status || status === 'done') && automationStartPending(chat, s.acpBindingBySessionId[chat.id]?.pendingDirectPrompts) ? 'processing' : status
  })))
  const runtimeStatusBySessionId = useMemo(
    () => new Map(sessions.map((session, index) => [session.id, runtimeStatuses[index]])),
    [runtimeStatuses, sessions],
  )
  const snoozeNowMs = useSnoozeClock(sessions)
  const prWatches = useAppStore((s) => s.prWatches)
  const watchingSessionIds = useMemo(() => new Set(prWatches.filter((watch) => watch.watching).map((watch) => watch.chatId)), [prWatches])
  const runtimeBindings = useMemo<ChatSearchFilterContext>(() => {
    const acpBindingBySessionId: NonNullable<ChatSearchFilterContext['acpBindingBySessionId']> = {}
    sessions.forEach((session, index) => {
      const status = runtimeStatuses[index]
      if (status === 'processing') acpBindingBySessionId[session.id] = { processing: true }
      else if (status === 'done') acpBindingBySessionId[session.id] = { readySinceLastSelect: true }
      else if (status) acpBindingBySessionId[session.id] = { attentionKind: status.slice('attention:'.length) as AcpAttentionKind }
    })
    return { acpBindingBySessionId, nowMs: snoozeNowMs, watchingSessionIds }
  }, [runtimeStatuses, sessions, snoozeNowMs, watchingSessionIds])
  const toggleFolder        = useAppStore((s) => s.toggleFolder)
  const addFolder           = useAppStore((s) => s.addFolder)
  const renameFolder        = useAppStore((s) => s.renameFolder)
  const deleteFolders       = useAppStore((s) => s.deleteFolders)
  const deleteFolder        = useAppStore((s) => s.deleteFolder)
  const rescanFolderChats    = useAppStore((s) => s.rescanFolderChats)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const reorderFolders = useAppStore((s) => s.reorderFolders)
  const reorderResourceReferences = useAppStore((s) => s.reorderResourceReferences)
  const createResourceCollection = useAppStore((s) => s.createResourceCollection)
  const deleteSessions = useAppStore((s) => s.deleteSessions)
  const previewUnsortedWorkspaceFolders = useAppStore((s) => s.previewUnsortedWorkspaceFolders)
  const rebuildUnsortedWorkspaceFolders = useAppStore((s) => s.rebuildUnsortedWorkspaceFolders)
  const workspaceFolderRecoveryError = useAppStore((s) => s.workspaceFolderRecoveryError)

  const [addingFolder, setAddingFolder] = useState(false)
  const [actionError, setActionError] = useState('')
  const [addingCollection, setAddingCollection] = useState(false)
  const [newCollectionName, setNewCollectionName] = useState('')
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [newFolderExecutionHostId, setNewFolderExecutionHostId] = useState('local')
  const [remoteBrowserOpen, setRemoteBrowserOpen] = useState(false)
  const newFolderExecutionHost = executionHosts.find((host) => host.id === newFolderExecutionHostId) ?? executionHosts[0]
  const newFolderIsRemote = isSshHost(newFolderExecutionHost)
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')
  const [pendingDeleteFolderId, setPendingDeleteFolderId] = useState<string | null>(null)
  const [deleteFolderError, setDeleteFolderError] = useState('')
  const [deletingFolder, setDeletingFolder] = useState(false)
  const [activeCollapsed, setActiveCollapsed] = useState(false)
  const [snoozedCollapsed, setSnoozedCollapsed] = useState(false)
  const [pinnedCollapsed, setPinnedCollapsed] = useState(false)
  const [allChatsCollapsed, setAllChatsCollapsed] = useState(false)
  const [unsortedCollapsed, setUnsortedCollapsed] = useState(false)
  const [selectedFolderIds, setSelectedFolderIds] = useState<Set<string>>(() => new Set())
  const folderSelectionAnchorRef = useRef<HTMLElement | null>(null)
  const workspaceDeleteInFlightRef = useRef(false)
  const [pendingDeleteFolderIds, setPendingDeleteFolderIds] = useState<string[] | null>(null)
  const [selectedSessionIds, setSelectedSessionIds] = useState<Set<string>>(() => new Set())
  const [selectedCollectionIds, setSelectedCollectionIds] = useState<Set<string>>(() => new Set())
  const collectionSelectionAnchorRef = useRef<HTMLElement | null>(null)
  const [pendingDeleteCollections, setPendingDeleteCollections] = useState<BulkTarget[] | null>(null)
  const selectionAnchorRowRef = useRef<HTMLElement | null>(null)
  const [pendingBulkDeleteIds, setPendingBulkDeleteIds] = useState<string[] | null>(null)
  const [bulkDeleteFailed, setBulkDeleteFailed] = useState(false)
  const [bulkDeleting, setBulkDeleting] = useState(false)
  const bulkDeleteInFlightRef = useRef(false)
  const [unsortedMenuPoint, setUnsortedMenuPoint] = useState<{ x: number; y: number } | null>(null)
  const [recoveryDialogOpen, setRecoveryDialogOpen] = useState(false)
  const [recoveryPreview, setRecoveryPreview] = useState<WorkspaceFolderRecoveryPreview | null>(null)
  const [recoveryLoading, setRecoveryLoading] = useState(false)
  const [recoveryApplying, setRecoveryApplying] = useState(false)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  // Outlines the collection a dragged workspace would join, straight on the DOM so the tree never re-renders mid-drag.
  const showFolderDropTarget = (collectionId: string | null) => {
    for (const element of treeRef.current?.querySelectorAll<HTMLElement>('[data-drop-collection-id]') ?? []) {
      element.classList.toggle('uam-sb-drop-target', element.dataset.dropCollectionId === collectionId)
    }
  }
  const pinnedChatOrder = useAppStore((s) => s.sidebarPinnedChatOrder)
  const activeChatOrder = useAppStore((s) => s.sidebarActiveChatOrder)
  const activeChatOrderSavedAtMs = useAppStore((s) => s.sidebarActiveChatOrderSavedAtMs)
  // Pinned and Active rows reach the latest drag and keyboard handlers through a stable context value.
  const chatSortRef = useRef<{ start: (list: ChatSortList, sessionId: string, event: ReactPointerEvent<HTMLElement>) => void; move: (list: ChatSortList, sessionId: string, direction: -1 | 1) => void } | null>(null)
  const chatSort = useMemo(() => ({
    start: (list: ChatSortList, sessionId: string, event: ReactPointerEvent<HTMLElement>) => chatSortRef.current?.start(list, sessionId, event),
    move: (list: ChatSortList, sessionId: string, direction: -1 | 1) => chatSortRef.current?.move(list, sessionId, direction),
  }), [])
  // Written directly so announcing a drag never re-renders the whole tree.
  const moveStatusRef = useRef<HTMLDivElement>(null)
  const setMoveAnnouncement = (text: string) => { if (moveStatusRef.current) moveStatusRef.current.textContent = text }
  const addControlsRef = useRef<HTMLDivElement>(null)
  const treeRef = useRef<HTMLDivElement>(null)
  const unsortedMenuTriggerRef = useRef<HTMLButtonElement>(null)
  const unsortedMenuRef = useRef<HTMLDivElement>(null)
  const recoveryDialogRef = useRef<HTMLDivElement>(null)
  const recoveryRequestRef = useRef(0)

  useEffect(() => subscribeChatGridLayout(setGridLayout), [])

  useEffect(() => {
    if (!executionHosts.some((host) => host.id === newFolderExecutionHostId)) {
      setNewFolderExecutionHostId(executionHosts[0]?.id ?? 'local')
      setNewFolderDirectory('')
    }
  }, [executionHosts, newFolderExecutionHostId])

  useEffect(() => {
    if (!unsortedMenuPoint) return
    unsortedMenuRef.current?.querySelector<HTMLButtonElement>('button')?.focus()
    const dismiss = (event: MouseEvent) => {
      const target = event.target as Node
      if (!unsortedMenuRef.current?.contains(target) && !unsortedMenuTriggerRef.current?.contains(target)) setUnsortedMenuPoint(null)
    }
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setUnsortedMenuPoint(null)
      unsortedMenuTriggerRef.current?.focus()
    }
    document.addEventListener('mousedown', dismiss)
    document.addEventListener('keydown', closeOnEscape)
    return () => {
      document.removeEventListener('mousedown', dismiss)
      document.removeEventListener('keydown', closeOnEscape)
    }
  }, [unsortedMenuPoint])

  useEffect(() => {
    if (!recoveryDialogOpen) return
    recoveryDialogRef.current?.focus()
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || recoveryApplying) return
      recoveryRequestRef.current++
      setRecoveryDialogOpen(false)
      unsortedMenuTriggerRef.current?.focus()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [recoveryApplying, recoveryDialogOpen])

  useEffect(() => {
    if (addingFolder || addingCollection) {
      addControlsRef.current?.scrollIntoView?.({ block: 'nearest' })
    }
  }, [addingCollection, addingFolder])

  const paneColorsForFolders = (folderIds: Set<string>) => {
    const leaves = chatGridLeaves(gridLayout.root)
    if (leaves.length === 1) return []
    const folderSessionIds = new Set(sessions.filter((session) => session.folderId && folderIds.has(session.folderId)).map((session) => session.id))
    return leaves.flatMap((leaf, index) => folderSessionIds.has(leaf.sessionId) ? [chatPaneColors[index]] : [])
  }
  const paneColorsForFolder = (folderId: string) => paneColorsForFolders(new Set([folderId]))

  const searchTokens = useMemo(
    () => tokenizeChatSearchQuery(searchQuery),
    [searchQuery]
  )
  const searchIndex = useMemo(
    () => searchTokens.length > 0 ? buildChatSearchIndex(sessions) : EMPTY_SEARCH_INDEX,
    [sessions, searchTokens]
  )
  const deepSearchSet = useMemo(
    () => deepSearchSessionIds ? new Set(deepSearchSessionIds) : undefined,
    [deepSearchSessionIds]
  )
  const searchModel = useMemo(
    () => buildChatSearchModel(
      folders,
      sessions,
      searchIndex,
      searchTokens,
      deepSearchSet,
      filters,
      runtimeBindings
    ),
    [folders, sessions, searchIndex, searchTokens, deepSearchSet, filters, runtimeBindings, navigationRevision]
  )
  const sessionsById = useMemo(
    () => new Map(sessions.map((session) => [session.id, session])),
    [sessions]
  )
  const activeStatusCounts = useMemo(() => {
    const counts = { running: 0, attention: 0, done: 0 }
    for (const sessionId of searchModel.activeSessionIds) {
      const status = sessionsById.get(sessionId)?.automationPendingPrompt ? 'attention:automation' : runtimeStatusBySessionId.get(sessionId)
      if (status === 'processing') counts.running += 1
      else if (status?.startsWith('attention:')) counts.attention += 1
      else if (status === 'done') counts.done += 1
    }
    return counts
  }, [runtimeStatusBySessionId, searchModel.activeSessionIds, sessionsById])
  const activeStatusSummary = [
    activeStatusCounts.running ? `${activeStatusCounts.running} running` : '',
    activeStatusCounts.attention ? `${activeStatusCounts.attention} attention` : '',
    activeStatusCounts.done ? `${activeStatusCounts.done} done` : '',
  ].filter(Boolean).join(' · ')
  const snoozedExpanded = searchModel.isSearching || !snoozedCollapsed
  const activeExpanded = searchModel.isSearching || !activeCollapsed
  const pinnedExpanded = searchModel.isSearching || !pinnedCollapsed
  const allChatsExpanded = searchModel.isSearching || !allChatsCollapsed
  // Section rows stay mounted through the exit window, then unmount (collapsed rows are not rendered).
  const activeList = usePresence(activeExpanded)
  const snoozedList = usePresence(snoozedExpanded)
  const pinnedList = usePresence(pinnedExpanded)
  const allChatsList = usePresence(allChatsExpanded)
  const nativeChildIds = useMemo(() => new Set(sessions.filter(hasNativeSubagentLineage).map(session => session.id)), [sessions, navigationRevision])
  const orphanSubagents = useMemo(() => searchModel.orphanSessionIds.flatMap(id => {
    const session = sessionsById.get(id)
    return session ? [session] : []
  }), [searchModel.orphanSessionIds, sessionsById])
  const previousFamiliesRef = useRef(new Map<string, string[]>())
  const familySessionIdsByRootId = useMemo(() => {
    const families = new Map<string, string[]>()
    for (const session of sessions) {
      if (nativeChildIds.has(session.id)) continue
      const rootId = session.branchRootChatId || session.parentChatId || session.id
      const family = families.get(rootId)
      if (family) family.push(session.id)
      else families.set(rootId, [session.id])
    }
    // Metadata updates must not invalidate every memoized chat row.
    for (const [rootId, family] of families) {
      const previous = previousFamiliesRef.current.get(rootId)
      if (previous && shallow(previous, family)) families.set(rootId, previous)
    }
    return families
  }, [sessions, nativeChildIds])
  useLayoutEffect(() => { previousFamiliesRef.current = familySessionIdsByRootId }, [familySessionIdsByRootId])
  // Each selected row stands for its branch family, matching single-row deletion.
  const pendingBulkDeleteFamilyIds = useMemo(() => pendingBulkDeleteIds
    ? [...new Set(pendingBulkDeleteIds.flatMap((id) => familySessionIdsByRootId.get(id) ?? [id]))]
    : null, [pendingBulkDeleteIds, familySessionIdsByRootId])
  const selectedFolderTargets = useMemo<BulkTarget[] | null>(() => selectedFolderIds.size > 1
    ? folders.filter((folder) => selectedFolderIds.has(folder.id)).map((folder) => ({ id: folder.id, label: folder.name }))
    : null, [folders, selectedFolderIds])
  const pendingDeleteFolder = useMemo(
    () => folders.find((folder) => folder.id === pendingDeleteFolderId) ?? null,
    [folders, pendingDeleteFolderId]
  )
  const pendingDeleteChatCount = useMemo(
    () => sessions.filter((session) => session.folderId === pendingDeleteFolderId).length,
    [sessions, pendingDeleteFolderId]
  )
  const pendingDeleteWorkspaceChatCount = useMemo(
    () => sessions.filter((session) => session.folderId && pendingDeleteFolderIds?.includes(session.folderId)).length,
    [sessions, pendingDeleteFolderIds]
  )
  // One confirmation for menu and selection deletes, so a single workspace reads the same either way.
  const workspaceDeleteCount = pendingDeleteFolderIds ? pendingDeleteFolderIds.length : pendingDeleteFolder ? 1 : 0
  const singleWorkspaceDelete = pendingDeleteFolderIds
    ? (pendingDeleteFolderIds.length === 1 ? folders.find((folder) => folder.id === pendingDeleteFolderIds[0]) : undefined)
    : pendingDeleteFolder ?? undefined
  const workspaceDeleteChatCount = pendingDeleteFolderIds ? pendingDeleteWorkspaceChatCount : pendingDeleteChatCount
  const unsortedExpanded = searchModel.isSearching || !unsortedCollapsed

  const visibleSessionRows = useCallback(() => Array.from(
    treeRef.current?.querySelectorAll<HTMLElement>('[data-session-id]') ?? []
  ).filter((row) => !row.closest('[inert]')), [])
  const visibleSessionIds = useCallback(() => visibleSessionRows()
    .map((row) => row.dataset.sessionId ?? '').filter(Boolean), [visibleSessionRows])

  const handleSessionClick = useCallback((sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => {
    setSelectedFolderIds((current) => current.size ? new Set<string>() : current)
    setSelectedCollectionIds((current) => current.size ? new Set<string>() : current)
    collectionSelectionAnchorRef.current = null
    folderSelectionAnchorRef.current = null
    if (isCompanionContext() || !event.shiftKey) {
      selectionAnchorRowRef.current = event.currentTarget
      setSelectedSessionIds((current) => current.size ? new Set<string>() : current)
      return false
    }

    event.preventDefault()
    const rows = visibleSessionRows()
    const anchorIndex = selectionAnchorRowRef.current
      ? rows.indexOf(selectionAnchorRowRef.current)
      : -1
    const targetIndex = rows.indexOf(event.currentTarget)
    if (anchorIndex < 0 || targetIndex < 0) {
      selectionAnchorRowRef.current = event.currentTarget
      setSelectedSessionIds(new Set([sessionId]))
      return true
    }

    const start = Math.min(anchorIndex, targetIndex)
    const end = Math.max(anchorIndex, targetIndex)
    setSelectedSessionIds(new Set(rows.slice(start, end + 1).map((row) => row.dataset.sessionId ?? '').filter(Boolean)))
    return true
  }, [visibleSessionRows])

  const selectedSessionIdsRef = useRef(selectedSessionIds)
  useLayoutEffect(() => { selectedSessionIdsRef.current = selectedSessionIds }, [selectedSessionIds])
  const visibleSelectedSessionIds = useCallback(() => {
    const visible = new Set(visibleSessionIds())
    return [...selectedSessionIdsRef.current].filter((id) => visible.has(id))
  }, [visibleSessionIds])
  const requestBulkDelete = useCallback(() => {
    const ids = visibleSelectedSessionIds()
    if (ids.length > 0) {
      setBulkDeleteFailed(false)
      setPendingBulkDeleteIds(ids)
    }
  }, [visibleSelectedSessionIds])
  const chatSelection = useMemo<ChatSelection>(() => ({ selectedIds: visibleSelectedSessionIds, deleteSelected: requestBulkDelete }), [visibleSelectedSessionIds, requestBulkDelete])

  // Rows in a multi-selection are told so, letting their hover actions name the whole selection.
  const rowSelection = (id: string) => !companionContext && selectedSessionIds.has(id) && (selectedSessionIds.size > 1 ? 'multi' as const : true)

  const clearBulkSelection = useCallback(() => {
    setSelectedSessionIds(new Set())
    selectionAnchorRowRef.current = null
    setPendingBulkDeleteIds(null)
    setBulkDeleteFailed(false)
  }, [])

  const visibleFolderRows = useCallback(() => Array.from(treeRef.current?.querySelectorAll<HTMLElement>('[data-workspace-id]') ?? []).filter((row) => !row.closest('[hidden], [inert], [aria-hidden="true"]')), [])

  const clearWorkspaceSelection = useCallback(() => {
    setSelectedFolderIds(new Set())
    folderSelectionAnchorRef.current = null
    setPendingDeleteFolderIds(null)
    setDeleteFolderError('')
  }, [])

  const handleFolderClick = (folderId: string, event: ReactMouseEvent<HTMLDivElement>) => {
    clearBulkSelection()
    setSelectedCollectionIds((current) => current.size ? new Set<string>() : current)
    collectionSelectionAnchorRef.current = null
    if (isCompanionContext() || !event.shiftKey) {
      folderSelectionAnchorRef.current = event.currentTarget
      setSelectedFolderIds(new Set())
      toggleFolder(folderId)
      return
    }
    event.preventDefault()
    const rows = visibleFolderRows()
    const anchor = folderSelectionAnchorRef.current ? rows.indexOf(folderSelectionAnchorRef.current) : -1
    const target = rows.indexOf(event.currentTarget)
    if (anchor < 0 || target < 0) {
      folderSelectionAnchorRef.current = event.currentTarget
      setSelectedFolderIds(new Set([folderId]))
      return
    }
    setSelectedFolderIds(new Set(rows.slice(Math.min(anchor, target), Math.max(anchor, target) + 1).map((row) => row.dataset.workspaceId!)))
  }

  // Shift-click selects a range of collection headers, mirroring workspaces; a plain click expands or collapses.
  const handleCollectionClick = (collectionId: string, event: ReactMouseEvent<HTMLElement>) => {
    const header = event.currentTarget.closest<HTMLElement>('[data-collection-id]')
    if (isCompanionContext() || !event.shiftKey || !header) {
      collectionSelectionAnchorRef.current = header
      setSelectedCollectionIds((current) => current.size ? new Set<string>() : current)
      void useAppStore.getState().toggleResourceCollection(collectionId)
      return
    }
    event.preventDefault()
    clearBulkSelection()
    clearWorkspaceSelection()
    // Headers inside a collapsed (inert) parent are hidden, so a range never selects them for bulk actions.
    const rows = Array.from(treeRef.current?.querySelectorAll<HTMLElement>('[data-collection-id]') ?? []).filter((row) => !row.closest('[inert]'))
    const anchor = collectionSelectionAnchorRef.current ? rows.indexOf(collectionSelectionAnchorRef.current) : -1
    const target = rows.indexOf(header)
    if (anchor < 0 || target < 0) {
      collectionSelectionAnchorRef.current = header
      setSelectedCollectionIds(new Set([collectionId]))
      return
    }
    setSelectedCollectionIds(new Set(rows.slice(Math.min(anchor, target), Math.max(anchor, target) + 1).map((row) => row.dataset.collectionId!)))
  }
  // Collections are only selectable while their headers are shown, so searching or collapsing All chats clears them.
  const collectionsShown = !searchModel.isSearching && allChatsExpanded
  useEffect(() => {
    setSelectedCollectionIds((current) => {
      const next = collectionsShown ? new Set([...current].filter((id) => resourceCollections.some((collection) => collection.id === id))) : new Set<string>()
      return next.size === current.size ? current : next
    })
  }, [collectionsShown, resourceCollections])
  const selectedCollectionTargets = useMemo<BulkTarget[] | null>(() => selectedCollectionIds.size > 1
    ? resourceCollections.filter((collection) => selectedCollectionIds.has(collection.id)).map((collection) => ({ id: collection.id, label: collection.name }))
    : null, [resourceCollections, selectedCollectionIds])
  useEffect(() => {
    if (selectedCollectionIds.size === 0 || pendingDeleteCollections) return
    const clear = (event: KeyboardEvent) => { if (event.key === 'Escape') setSelectedCollectionIds(new Set()) }
    document.addEventListener('keydown', clear)
    return () => document.removeEventListener('keydown', clear)
  }, [selectedCollectionIds.size, pendingDeleteCollections])
  const requestCollectionDelete = () => {
    const targets = resourceCollections.filter((collection) => selectedCollectionIds.has(collection.id)).map((collection) => ({ id: collection.id, label: collection.name }))
    if (targets.length > 0) setPendingDeleteCollections(targets)
  }

  useEffect(() => {
    if (selectedFolderIds.size === 0) return
    const prune = () => {
      if (deletingFolder) return
      const visible = new Set(visibleFolderRows().map((row) => row.dataset.workspaceId))
      setSelectedFolderIds((selected) => {
        const next = new Set([...selected].filter((id) => visible.has(id)))
        return next.size === selected.size ? selected : next
      })
      setPendingDeleteFolderIds((pending) => {
        if (!pending) return pending
        const next = pending.filter((id) => visible.has(id))
        return next.length === pending.length ? pending : next.length > 0 ? next : null
      })
    }
    prune()
    const observer = new MutationObserver(prune)
    if (treeRef.current) observer.observe(treeRef.current, { subtree: true, childList: true, attributes: true, attributeFilter: ['hidden', 'inert', 'aria-hidden'] })
    return () => observer.disconnect()
  }, [selectedFolderIds.size, folders, searchModel, deletingFolder, visibleFolderRows])

  useEffect(() => {
    if (selectedFolderIds.size === 0 || pendingDeleteFolderIds) return
    const clear = (event: KeyboardEvent) => { if (event.key === 'Escape') clearWorkspaceSelection() }
    document.addEventListener('keydown', clear)
    return () => document.removeEventListener('keydown', clear)
  }, [selectedFolderIds.size, pendingDeleteFolderIds, clearWorkspaceSelection])

  const confirmDeleteWorkspaces = async () => {
    if (!pendingDeleteFolderIds || workspaceDeleteInFlightRef.current) return
    workspaceDeleteInFlightRef.current = true
    setDeletingFolder(true)
    setDeleteFolderError('')
    const failure = pendingDeleteFolderIds.length === 1 ? 'The workspace could not be deleted. Try again.' : 'The workspaces could not be deleted. Try again.'
    try {
      const ids = pendingDeleteFolderIds
      if (await deleteFolders(ids)) clearWorkspaceSelection()
      // Partial deletion: refused workspaces were reported by name and stay selected for another try.
      else if (ids.some((id) => !useAppStore.getState().folders.some((folder) => folder.id === id))) setPendingDeleteFolderIds(null)
      else setDeleteFolderError(useAppStore.getState().workspaceDeletionError || failure)
    } catch (error) {
      setDeleteFolderError(error instanceof Error && error.message.trim() ? error.message : failure)
    } finally {
      workspaceDeleteInFlightRef.current = false
      setDeletingFolder(false)
    }
  }

  const openWorkspaceRecovery = useCallback(() => {
    const requestId = ++recoveryRequestRef.current
    setUnsortedMenuPoint(null)
    setRecoveryDialogOpen(true)
    setRecoveryPreview(null)
    setRecoveryLoading(true)
    void previewUnsortedWorkspaceFolders().then((preview) => {
      if (recoveryRequestRef.current === requestId) setRecoveryPreview(preview)
    }).finally(() => {
      if (recoveryRequestRef.current === requestId) setRecoveryLoading(false)
    })
  }, [previewUnsortedWorkspaceFolders])

  const closeWorkspaceRecovery = useCallback(() => {
    if (recoveryApplying) return
    recoveryRequestRef.current++
    setRecoveryDialogOpen(false)
    unsortedMenuTriggerRef.current?.focus()
  }, [recoveryApplying])

  const applyWorkspaceRecovery = useCallback(() => {
    setRecoveryApplying(true)
    void rebuildUnsortedWorkspaceFolders().then((rebuilt) => {
      if (rebuilt) {
        setRecoveryDialogOpen(false)
        unsortedMenuTriggerRef.current?.focus()
      }
    }).finally(() => setRecoveryApplying(false))
  }, [rebuildUnsortedWorkspaceFolders])

  useEffect(() => {
    if (selectedSessionIds.size === 0 || pendingBulkDeleteIds) return
    const clearOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') clearBulkSelection()
    }
    document.addEventListener('keydown', clearOnEscape)
    return () => document.removeEventListener('keydown', clearOnEscape)
  }, [clearBulkSelection, selectedSessionIds.size, pendingBulkDeleteIds])

  useEffect(() => {
    const root = treeRef.current
    if (!root) return
    const keepVisibleSelection = () => {
      const visible = new Set(visibleSessionIds())
      setSelectedSessionIds((current) => {
        const next = new Set([...current].filter((id) => visible.has(id)))
        return next.size === current.size ? current : next
      })
      if (selectionAnchorRowRef.current && !visibleSessionRows().includes(selectionAnchorRowRef.current)) {
        selectionAnchorRowRef.current = null
      }
    }
    const observer = new MutationObserver(keepVisibleSelection)
    observer.observe(root, { childList: true, subtree: true, attributes: true, attributeFilter: ['inert'] })
    keepVisibleSelection()
    return () => observer.disconnect()
  }, [visibleSessionIds, visibleSessionRows])

  useEffect(() => {
    if (pendingDeleteFolderId !== null && !pendingDeleteFolder) {
      setPendingDeleteFolderId(null)
      setDeleteFolderError('')
      setDeletingFolder(false)
    }
  }, [pendingDeleteFolder, pendingDeleteFolderId])

  const commitAddFolder = () => {
    const name = newFolderName.trim()
    const directory = newFolderDirectory.trim()
    const executionHost = newFolderExecutionHost

    if (!name || !directory || !executionHost) {
      return
    }
    if (isSshHost(executionHost) && !isAbsoluteRemoteWorkspace(executionHost.platform, directory)) {
      setActionError(`Enter an absolute ${executionHost.platform.toLowerCase() === 'windows' ? 'Windows' : 'Linux'} directory.`)
      return
    }

    setActionError('')
    void addFolder(name, null, directory, executionHost.id).then((created) => {
      if (!created) {
        setActionError('The workspace could not be created. Check the directory and try again.')
        return
      }

      setNewFolderName('')
      setNewFolderDirectory('')
      setNewFolderExecutionHostId('local')
      setAddingFolder(false)
    })
  }

  const commitAddCollection = () => {
    const name = newCollectionName.trim()
    if (!name) return
    setActionError('')
    void createResourceCollection(name).then((created) => {
      if (!created) {
        setActionError('The collection could not be created. Try again.')
        return
      }
      setNewCollectionName('')
      setAddingCollection(false)
    })
  }

  const startRenameFolder = (folder: (typeof folders)[number]) => {
    setActionError('')
    setEditingFolderId(folder.id)
    setEditFolderName(folder.name)
    setEditFolderDirectory(folder.directory)
  }

  const commitRenameFolder = async (folderId: string) => {
    const name = editFolderName.trim()
    const directory = editFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    setActionError('')
    if (await renameFolder(folderId, name, directory)) {
      setEditingFolderId(null)
    } else {
      setActionError('The workspace could not be updated. Finish active work and try again.')
    }
  }

  const confirmDeleteFolder = async () => {
    if (!pendingDeleteFolder || deletingFolder) return
    setDeletingFolder(true)
    setDeleteFolderError('')
    try {
      if (await deleteFolder(pendingDeleteFolder.id)) {
        setPendingDeleteFolderId(null)
      } else {
        setDeleteFolderError(useAppStore.getState().workspaceDeletionError || 'The workspace could not be deleted. Try again.')
      }
    } catch (error) {
      setDeleteFolderError(error instanceof Error && error.message.trim() ? error.message : 'The workspace could not be deleted. Try again.')
    } finally {
      setDeletingFolder(false)
    }
  }

  const cancelAddFolder = () => {
    setNewFolderName('')
    setNewFolderDirectory('')
    setNewFolderExecutionHostId('local')
    setAddingFolder(false)
  }

  const chooseNewFolderDirectory = async () => {
    const host = executionHosts.find((candidate) => candidate.id === newFolderExecutionHostId)
    if (isSshHost(host)) {
      setRemoteBrowserOpen(true)
      return
    }
    const selectedPath = await browseFolderDirectory(newFolderDirectory)
    if (selectedPath) {
      setNewFolderDirectory(selectedPath)
      setNewFolderName((name) => name.trim() ? name : selectedPath.split(/[\\/]/).filter(Boolean).pop() ?? '')
    }
  }

  const chooseEditFolderDirectory = async () => {
    const selectedPath = await browseFolderDirectory(editFolderDirectory)
    if (selectedPath) {
      setEditFolderDirectory(selectedPath)
    }
  }

  const folderRowsById = new Map(searchModel.folderRows.map((row) => [row.folder.id, row]))
  const groupedFolderIds = new Set<string>()
  const folderCollectionIds = new Map<string, string>()
  const collectionGroups = resourceCollections.map((collection) => ({
    collection,
    folderRows: collection.references.flatMap((reference) => {
      if (reference.type !== 'workspace-folder' || groupedFolderIds.has(reference.target)) return []
      const row = folderRowsById.get(reference.target)
      if (!row) return []
      groupedFolderIds.add(reference.target)
      folderCollectionIds.set(reference.target, collection.id)
      return [row]
    }),
  }))
  const ungroupedFolderRows = searchModel.folderRows.filter(({ folder }) => !groupedFolderIds.has(folder.id))
  const collectionIds = new Set(resourceCollections.map((collection) => collection.id))
  // Native sanitizes parents; an unknown parent still renders at the top level rather than disappearing.
  const parentOf = (collection: ResourceCollection) => collection.parentId && collectionIds.has(collection.parentId) ? collection.parentId : ''
  const renderCollection = ({ collection, folderRows }: (typeof collectionGroups)[number]): ReactNode => {
    const subtree = new Set(collectionSubtreeIds(resourceCollections, collection.id))
    return (
      <FolderCollection
        key={collection.id}
        collection={collection}
        folderCount={folderRows.length}
        hiddenPaneColors={paneColorsForFolders(new Set(collectionGroups.flatMap((group) => subtree.has(group.collection.id) ? group.folderRows.map(({ folder }) => folder.id) : [])))}
        selected={!companionContext && selectedCollectionIds.has(collection.id)}
        selectionTargets={!companionContext && selectedCollectionIds.has(collection.id) ? selectedCollectionTargets : null}
        onHeaderClick={(event) => handleCollectionClick(collection.id, event)}
        onDeleteSelected={requestCollectionDelete}
      >
        {collectionGroups.filter((group) => parentOf(group.collection) === collection.id).map(renderCollection)}
        {folderRows.map(renderFolderRow)}
      </FolderCollection>
    )
  }

  const commitFolderMove = async (sourceId: string, targetId: string, edge: FolderDropEdge): Promise<boolean> => {
    if (sourceId === targetId) return false
    const sourceCollectionId = folderCollectionIds.get(sourceId)
    const targetCollectionId = folderCollectionIds.get(targetId)
    if (sourceCollectionId && sourceCollectionId === targetCollectionId) {
      const collection = resourceCollections.find((item) => item.id === sourceCollectionId)
      const referenceIds = collection
        ? reorderCollectionFolderReferences(collection, sourceId, targetId, edge)
        : null
      return referenceIds ? reorderResourceReferences(sourceCollectionId, referenceIds) : false
    }
    if (!sourceCollectionId && !targetCollectionId) {
      return reorderFolders(moveId(folders.map((folder) => folder.id), sourceId, targetId, edge))
    }
    return false
  }

  const folderListOf = (folderId: string) => {
    const collectionId = folderCollectionIds.get(folderId)
    return collectionId
      ? collectionGroups.find(({ collection }) => collection.id === collectionId)?.folderRows ?? []
      : ungroupedFolderRows
  }
  const announcePosition = (folderId: string, ids: readonly string[]) => {
    const name = folders.find((folder) => folder.id === folderId)?.name ?? 'Workspace'
    setMoveAnnouncement(`${name} moved to position ${ids.indexOf(folderId) + 1} of ${ids.length}`)
  }

  const moveFolderWithKeyboard = (sourceId: string, direction: -1 | 1) => {
    if (isSorting()) return
    const rows = folderListOf(sourceId)
    const sourceIndex = rows.findIndex(({ folder }) => folder.id === sourceId)
    const target = rows[sourceIndex + direction]
    if (!target) return
    const ids = rows.map(({ folder }) => folder.id)
    ids.splice(sourceIndex + direction, 0, ...ids.splice(sourceIndex, 1))
    const row = () => sortRow(treeRef.current, sourceId)
    const header = () => row()?.querySelector<HTMLElement>('[data-workspace-id]')
    void glideAfter(row(), () => commitFolderMove(sourceId, target.folder.id, direction < 0 ? 'before' : 'after')).then((moved) => {
      if (!moved) return
      announcePosition(sourceId, ids)
      // Reordering moves DOM nodes, which can drop focus; keep it on the moved workspace.
      if (document.activeElement !== header()) header()?.focus()
    })
  }

  const startFolderDrag = (folder: Folder, event: ReactPointerEvent<HTMLElement>) => {
    const ownCollectionId = folderCollectionIds.get(folder.id) ?? ''
    startSortableDrag(event, {
      targetAt: (x, y) => {
        const under = document.elementFromPoint?.(x, y)
        if (!under?.closest('#all-chat-list')) return null
        // The innermost collection under the pointer, or '' outside every collection; the row's own list sorts.
        const collectionId = under.closest<HTMLElement>('[data-drop-collection-id]')?.dataset.dropCollectionId ?? ''
        return collectionId === ownCollectionId ? null : collectionId
      },
      onTargetChange: showFolderDropTarget,
      onStart: () => setMoveAnnouncement(`Moving ${folder.name}. Press Escape to cancel.`),
      onCancel: () => setMoveAnnouncement(`Move of ${folder.name} cancelled`),
      onDrop: async ({ order, target }) => {
        if (target !== null) {
          const moved = await moveFolderToCollection(target || null, folder.id, folder.name)
          const collectionName = resourceCollections.find((collection) => collection.id === target)?.name
          if (moved) setMoveAnnouncement(collectionName ? `${folder.name} moved into ${collectionName}` : `${folder.name} removed from its collection`)
          return moved
        }
        const index = order.indexOf(folder.id)
        const neighbour = order[index - 1] ?? order[index + 1]
        const moved = neighbour ? await commitFolderMove(folder.id, neighbour, index > 0 ? 'after' : 'before') : false
        if (moved) announcePosition(folder.id, order)
        return moved
      },
    })
  }

  // A placed Active chat that left (Done, Clear all, snooze) after the order was saved and is back shows on top again.
  // ponytail: leaving by three days of inactivity sets no timestamp, so such a chat returns to its saved place.
  const returnedToActive = (id: string) => {
    const session = sessionsById.get(id)
    const settledMs = session?.settledAt ? Date.parse(session.settledAt) : NaN
    const wokeMs = session?.snoozedUntilMs ?? 0
    return settledMs > activeChatOrderSavedAtMs || (wokeMs > activeChatOrderSavedAtMs && wokeMs <= snoozeNowMs)
  }
  const chatSortIds = { pinned: applyManualOrder(searchModel.pinnedSessionIds, pinnedChatOrder), active: applyManualOrder(searchModel.activeSessionIds, activeChatOrder, returnedToActive) }
  const chatListElement = (list: ChatSortList) => document.getElementById(list === 'pinned' ? 'pinned-chat-list' : 'active-chat-list')
  const chatName = (sessionId: string) => sessionsById.get(sessionId)?.name || 'Chat'
  const announceChatPosition = (sessionId: string, ids: readonly string[]) =>
    setMoveAnnouncement(`${chatName(sessionId)} moved to position ${ids.indexOf(sessionId) + 1} of ${ids.length}`)
  const showPaneDropTarget = (leafId: string | null) => {
    for (const element of document.querySelectorAll<HTMLElement>('[data-leaf-id]')) element.toggleAttribute('data-pointer-drop', element.dataset.leafId === leafId)
  }
  // Active saves exactly what it shows, so a chat that left drops its old place. A snoozed pin is hidden from
  // Pinned for a while, so it keeps its saved place after the visible ones.
  const saveChatOrder = (list: ChatSortList, ids: string[]) => useAppStore.getState().setSidebarChatOrder(list,
    list === 'pinned' ? [...ids, ...pinnedChatOrder.filter((id) => !ids.includes(id))] : ids)
  chatSortRef.current = {
    start: (list, sessionId, event) => startSortableDrag(event, {
      // Dragging a Pinned or Active chat onto a chat pane still opens it there, as dragging any chat row does.
      targetAt: (x, y) => {
        const leafId = document.elementFromPoint?.(x, y)?.closest<HTMLElement>('[data-leaf-id]')?.dataset.leafId
        return leafId ? `pane:${leafId}` : null
      },
      onTargetChange: (target) => showPaneDropTarget(target?.slice('pane:'.length) ?? null),
      onStart: () => setMoveAnnouncement(`Moving ${chatName(sessionId)}. Press Escape to cancel.`),
      onCancel: () => setMoveAnnouncement(`Move of ${chatName(sessionId)} cancelled`),
      onDrop: async ({ order, target }) => {
        if (target !== null) {
          writeChatGridLayout(setChatInLeaf(readChatGridLayout(), sessionId, target.slice('pane:'.length)))
          useAppStore.getState().setActiveSession(sessionId)
          setMoveAnnouncement(`${chatName(sessionId)} opened in the pane`)
          return true
        }
        const saved = await saveChatOrder(list, order)
        if (saved) announceChatPosition(sessionId, order)
        return saved
      },
    }),
    move: (list, sessionId, direction) => {
      if (isSorting()) return
      const ids = [...chatSortIds[list]]
      const index = ids.indexOf(sessionId)
      if (index < 0) return
      if (!ids[index + direction]) { setMoveAnnouncement(`${chatName(sessionId)} is already ${direction < 0 ? 'first' : 'last'}`); return }
      ids.splice(index + direction, 0, ...ids.splice(index, 1))
      const row = () => sortRow(chatListElement(list), sessionId)
      void glideAfter(row(), () => saveChatOrder(list, ids)).then((moved) => {
        if (!moved) return
        announceChatPosition(sessionId, ids)
        const focusTarget = row()?.querySelector<HTMLElement>('[data-session-id]')
        if (focusTarget && document.activeElement !== focusTarget) focusTarget.focus()
      })
    },
  }
  // Sorting is for the plain lists: search shows a filtered view, and the phone companion only browses.
  const chatSortList = (list: ChatSortList) => searchModel.isSearching || companionContext || chatSortIds[list].length > MAX_SORTED_CHATS ? undefined : list

  const renderActivitySessionItem = (id: string) => {
    const activitySession = sessionsById.get(id)
    const folder = folders.find((candidate) => candidate.id === activitySession?.folderId)
    const hostId = activitySession?.executionHostId?.trim() || folder?.executionHostId || 'local'
    const host = executionHosts.find((candidate) => candidate.id === hostId)
    return (
      <SessionItem
        key={id}
        sessionId={id}
        session={activitySession}
        familySessionIds={familySessionIdsByRootId.get(id)}
        selected={rowSelection(id)}
        activityLayout
        activityFolderName={folder?.name}
        activityFolderIcon={folder?.customIcon}
        activityHostLabel={`Runs on ${host?.label || hostId}`}
        activityHostRemote={SSH_ENABLED && hostId !== 'local'}
        onSessionClick={handleSessionClick} onSessionOpen={onSessionOpen}
        sortList={chatSortList('active')}
      />
    )
  }

  const renderFolderRow = ({ folder, sessionIds, shouldShowSessions }: (typeof searchModel.folderRows)[number]) => {
    const hostId = folder.executionHostId || 'local'
    const machineKind = hostId === 'local' ? 'local' : 'remote'
    const machineLabel = `Runs on ${executionHosts.find((host) => host.id === hostId)?.label || hostId}`
    return <FolderRow
      key={folder.id}
      folder={folder}
      machineKind={machineKind}
      machineLabel={machineLabel}
      sessionIds={sessionIds}
      shouldShowSessions={shouldShowSessions}
      hiddenPaneColors={shouldShowSessions ? [] : paneColorsForFolder(folder.id)}
      isSearching={searchModel.isSearching}
      isEditing={!companionContext && editingFolderId === folder.id}
      editFolderName={editFolderName}
      editFolderDirectory={editFolderDirectory}
      selected={!companionContext && selectedFolderIds.has(folder.id)}
      selectionTargets={!companionContext && selectedFolderIds.has(folder.id) ? selectedFolderTargets : null}
      onToggle={(event) => handleFolderClick(folder.id, event)}
      onStartRename={() => startRenameFolder(folder)}
      onDelete={() => {
        setDeleteFolderError('')
        if (selectedFolderIds.has(folder.id)) {
          const visible = new Set(visibleFolderRows().map((row) => row.dataset.workspaceId))
          const ids = [...selectedFolderIds].filter((id) => visible.has(id))
          if (ids.length > 0) setPendingDeleteFolderIds(ids)
        } else {
          setPendingDeleteFolderId(folder.id)
        }
      }}
      onRescan={() => {
        setActionError('')
        void rescanFolderChats(folder.id).then((rescanned) => {
          if (!rescanned) setActionError(`Could not rescan chats for ${folder.name}. Try again.`)
        })
      }}
      onEditNameChange={setEditFolderName}
      onEditDirectoryChange={setEditFolderDirectory}
      onCommitRename={() => commitRenameFolder(folder.id)}
      onCancelEdit={() => setEditingFolderId(null)}
      onChooseDirectory={() => void chooseEditFolderDirectory()}
      onCreateChat={() => setNewChatModalOpen(true, folder.id)}
      sessionsById={sessionsById}
      familySessionIdsByRootId={familySessionIdsByRootId}
      selectedSessionIds={companionContext ? EMPTY_SELECTION_IDS : selectedSessionIds}
      onSessionClick={handleSessionClick} onSessionOpen={onSessionOpen}
      draggable={!searchModel.isSearching && !isCompanionContext()}
      onDragPointerDown={(event) => startFolderDrag(folder, event)}
      onMove={(direction) => moveFolderWithKeyboard(folder.id, direction)}
    />
  }

  return (
    <SubagentSearchMatches.Provider value={searchModel.matchedSubagentChildIds}>
    <ChatSelectionContext.Provider value={chatSelection}>
    <ChatSortContext.Provider value={chatSort}>
    <div ref={treeRef} className="select-none"
      onContextMenuCapture={(event) => { if (isCompanionContext()) { event.preventDefault(); event.stopPropagation() } }}
      onDragStartCapture={(event) => { if (isCompanionContext()) { event.preventDefault(); event.stopPropagation() } }}
      onKeyDownCapture={(event) => { if (isCompanionContext() && (event.shiftKey || ['F2', 'ContextMenu', 'ArrowUp', 'ArrowDown'].includes(event.key))) event.stopPropagation() }}
    >
      {!companionContext && selectedFolderIds.size > 0 && (
        <div className="mx-1 mb-1 flex items-center gap-2 px-2.5 py-1" style={{ background: 'var(--surface-up)', borderBottom: '1px solid var(--border)' }}>
          <span className="min-w-0 flex-1 text-xs font-medium">{selectedFolderIds.size} {selectedFolderIds.size === 1 ? 'workspace' : 'workspaces'} selected</span>
          <Button size="sm" variant="ghost" disabled={deletingFolder} onClick={clearWorkspaceSelection}>Clear</Button>
          <Button size="sm" variant="danger" disabled={deletingFolder} aria-label={`Delete ${countLabel(selectedFolderIds.size, 'selected workspace')}`} onClick={() => {
            const visible = new Set(visibleFolderRows().map((row) => row.dataset.workspaceId))
            const ids = [...selectedFolderIds].filter((id) => visible.has(id))
            if (ids.length > 0) {
              setDeleteFolderError('')
              setPendingDeleteFolderIds(ids)
            }
          }}>Delete</Button>
        </div>
      )}
      {!companionContext && selectedCollectionIds.size > 0 && (
        <div className="mx-1 mb-1 flex items-center gap-2 px-2.5 py-1" style={{ background: 'var(--surface-up)', borderBottom: '1px solid var(--border)' }}>
          <span className="min-w-0 flex-1 text-xs font-medium">{selectedCollectionIds.size} {selectedCollectionIds.size === 1 ? 'collection' : 'collections'} selected</span>
          <Button size="sm" variant="ghost" onClick={() => setSelectedCollectionIds(new Set())}>Clear</Button>
          <Button size="sm" variant="danger" aria-label={`Delete ${countLabel(selectedCollectionIds.size, 'selected collection')}`} onClick={requestCollectionDelete}>Delete</Button>
        </div>
      )}
      {!companionContext && selectedSessionIds.size > 0 && (
        <div className="mx-1 mb-1 flex items-center gap-2 rounded-md px-2.5 py-1" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
          <span className="min-w-0 flex-1 text-xs font-medium" style={{ color: 'var(--text-2)' }}>{selectedSessionIds.size} {selectedSessionIds.size === 1 ? 'chat' : 'chats'} selected</span>
          <Button size="sm" variant="ghost" onClick={clearBulkSelection}>Clear</Button>
          <Button
            size="sm"
            variant="danger"
            aria-label={`Delete ${countLabel(selectedSessionIds.size, 'selected chat')}`}
            onClick={requestBulkDelete}
          >
            Delete
          </Button>
        </div>
      )}
      {searchModel.activeSessionIds.length > 0 && (
        <div className="mb-0.5" data-testid="active-chats">
          <SidebarSectionHeader
            label="Active chats"
            listId="active-chat-list"
            expanded={activeExpanded}
            onToggle={() => setActiveCollapsed((collapsed) => !collapsed)}
            action={!isCompanionContext() && (
              
              // ui: raw button kept — compact 10px chip styled inline in the section header
              <button
                type="button"
                aria-label="Clear all active chats"
                className="rounded px-1.5 text-[10px] hover:bg-[var(--sidebar-item-hover)]"
                style={{ background: 'var(--surface)', border: 'none', color: 'var(--text-2)', cursor: 'pointer' }}
                onClick={() => {
                  const { setChatSettled } = useAppStore.getState()
                  // Questions and approvals stay: they need an answer, not a dismissal.
                  for (const id of searchModel.activeSessionIds) {
                    if (!runtimeStatusBySessionId.get(id)?.startsWith('attention:')) void setChatSettled(id, true)
                  }
                }}
              >
                Clear all
              </button>
            )}
            trailing={activeStatusSummary && (
              <span aria-label="Active chat status counts" className="ml-auto text-[10px]" title={activeStatusSummary}>
                {activeStatusSummary}
              </span>
            )}
          />
          <div
            id="active-chat-list"
            className="uam-sb-collapse"
            data-state={activeList.closing ? 'closed' : 'open'}
            hidden={!activeList.mounted}
            aria-hidden={!activeExpanded}
            {...(!activeExpanded ? { inert: '' } : {})}
          >
            <div className="uam-sb-collapse__inner">
              {activeList.mounted && chatSortIds.active.map(renderActivitySessionItem)}
            </div>
          </div>
          {!activeExpanded && activeStatusCounts.attention > 0 && (
            <RowButton
              type="button"
              data-testid="active-attention-strip"
              className="uam-sb-attention-strip mx-1 flex w-[calc(100%-0.5rem)] items-center gap-2 rounded-md px-2 py-1 text-left text-[11px]"
                            onClick={() => setActiveCollapsed(false)}
            >
              <TriangleAlert size={12} aria-hidden />
              {activeStatusCounts.attention === 1
                ? '1 chat needs attention'
                : `${activeStatusCounts.attention} chats need attention`}
            </RowButton>
          )}
        </div>
      )}

      {searchModel.snoozedSessionIds.length > 0 && (
        <div className="mb-0.5" data-testid="snoozed-chats">
          <SidebarSectionHeader label="Snoozed" listId="snoozed-chat-list" expanded={snoozedExpanded} onToggle={() => setSnoozedCollapsed((value) => !value)}
            trailing={<span className="ml-auto text-[10px]">{searchModel.snoozedSessionIds.length}</span>} />
          <div id="snoozed-chat-list" className="uam-sb-collapse" data-state={snoozedList.closing ? 'closed' : 'open'} hidden={!snoozedList.mounted} aria-hidden={!snoozedExpanded} {...(!snoozedExpanded ? { inert: '' } : {})}>
            <div className="uam-sb-collapse__inner">
              {snoozedList.mounted && searchModel.snoozedSessionIds.map((id) => (
                <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={rowSelection(id)} onSessionClick={handleSessionClick} onSessionOpen={onSessionOpen} />
              ))}
            </div>
          </div>
        </div>
      )}

      {searchModel.pinnedSessionIds.length > 0 && (
        <div className="mb-0.5" data-testid="pinned-chats">
          <SidebarSectionHeader
            label="Pinned chats"
            listId="pinned-chat-list"
            expanded={pinnedExpanded}
            onToggle={() => setPinnedCollapsed((collapsed) => !collapsed)}
            trailing={<span className="ml-auto text-[10px]">{searchModel.pinnedSessionIds.length}</span>}
          />
          <div
            id="pinned-chat-list"
            className="uam-sb-collapse"
            data-state={pinnedList.closing ? 'closed' : 'open'}
            hidden={!pinnedList.mounted}
            aria-hidden={!pinnedExpanded}
            {...(!pinnedExpanded ? { inert: '' } : {})}
          >
            <div className="uam-sb-collapse__inner">
              {pinnedList.mounted && chatSortIds.pinned.map((id) => (
                <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={rowSelection(id)} onSessionClick={handleSessionClick} onSessionOpen={onSessionOpen} sortList={chatSortList('pinned')} />
              ))}
            </div>
          </div>
        </div>
      )}

      {!searchModel.isSearching && (searchModel.folderRows.length > 0 || resourceCollections.length > 0 || searchModel.unfolderedSessionIds.length > 0 || orphanSubagents.length > 0) && (
        <div data-testid="all-chats">
          <SidebarSectionHeader
            label="All chats"
            listId="all-chat-list"
            expanded={allChatsExpanded}
            onToggle={() => setAllChatsCollapsed((collapsed) => !collapsed)}
          />
        </div>
      )}

      <div
        id="all-chat-list"
        className="uam-sb-collapse"
        data-state={allChatsList.closing ? 'closed' : 'open'}
        hidden={!allChatsList.mounted}
        aria-hidden={!allChatsExpanded}
        {...(!allChatsExpanded ? { inert: '' } : {})}
        onDragOver={(event) => {
          // Dropping a nested collection outside every collection moves it to the top level.
          if (!event.dataTransfer?.types.includes(COLLECTION_DRAG_TYPE) || !resourceCollections.find((entry) => entry.id === draggedCollectionId)?.parentId) return
          event.preventDefault()
        }}
        onDrop={(event) => {
          const id = event.dataTransfer?.getData(COLLECTION_DRAG_TYPE)
          if (!id) return
          event.preventDefault()
          void moveCollection(id, null)
        }}
      >
      <div className="uam-sb-collapse__inner">
      {allChatsList.mounted && <>
      {!searchModel.isSearching && collectionGroups.filter((group) => !parentOf(group.collection)).map(renderCollection)}

      {(searchModel.isSearching ? searchModel.folderRows : ungroupedFolderRows).map(renderFolderRow)}

      {/* Unfoldered sessions */}
      {searchModel.unfolderedSessionIds.length > 0 && (
        <div className="mt-0.5">
          <div
            className="mx-1 flex w-[calc(100%-0.5rem)] items-center rounded-md"
            onContextMenu={(event) => {
              event.preventDefault()
              setUnsortedMenuPoint({ x: event.clientX, y: event.clientY })
            }}
          >
            <RowButton
              type="button"
              aria-label={`${unsortedExpanded ? 'Collapse' : 'Expand'} Unsorted`}
              aria-expanded={unsortedExpanded}
              aria-controls="unsorted-chat-list"
              className="flex min-w-0 flex-1 items-center gap-1.5 rounded-md px-2.5 py-0.5 text-left"
              style={{ background: 'transparent', border: 'none', color: 'var(--text-2)', cursor: 'pointer' }}
              onClick={() => setUnsortedCollapsed((collapsed) => !collapsed)}
            >
              <ChevronRight
                size={14}
                className="transition-transform duration-200 ease-out motion-reduce:transition-none"
                style={{ flexShrink: 0, color: 'var(--text-3)', transform: unsortedExpanded ? 'rotate(90deg)' : 'rotate(0deg)' }}
                aria-hidden
              />
              {unsortedExpanded
                ? <FolderOpenIcon size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />
                : <FolderIcon size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />}
              <span className="min-w-0 flex-1 truncate text-[13px] font-semibold">Unsorted</span>
              <span className="rounded px-1 text-[10px]" style={{ background: 'var(--surface-high)', color: 'var(--text-3)' }}>
                {searchModel.unfolderedSessionIds.length}
              </span>
            </RowButton>
            {!isCompanionContext() && <Tooltip label="Unsorted actions">
              {/* ui: raw button kept — 22px inline trigger; IconButton sizes are 26px and 30px */}<button
                ref={unsortedMenuTriggerRef}
                type="button"
                aria-label="Actions for Unsorted"
                aria-haspopup="menu"
                aria-expanded={unsortedMenuPoint !== null}
                className="mr-1 flex h-[22px] w-[22px] items-center justify-center rounded"
                style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer' }}
                onClick={(event) => {
                  if (unsortedMenuPoint) {
                    setUnsortedMenuPoint(null)
                    return
                  }
                  const rect = event.currentTarget.getBoundingClientRect()
                  setUnsortedMenuPoint({ x: rect.right, y: rect.bottom + 4 })
                }}
              >
                <MoreHorizontal size={14} aria-hidden />
              </button>
            </Tooltip>}
          </div>
          {unsortedMenuPoint && (
            <ViewportMenu
              ref={unsortedMenuRef}
              point={unsortedMenuPoint}
              role="menu"
              aria-label="Unsorted actions"
              align="end"
              className="rounded-md py-1 animate-fade-in"
              style={{ minWidth: 220, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
            >
              
              <MenuItem
                className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
                style={{ color: 'var(--text-2)', border: 'none', fontFamily: 'inherit' }}
                icon={<FolderSync size={14} aria-hidden />}
                label="Rebuild workspace folders…"
                onClick={openWorkspaceRecovery}
              />
            </ViewportMenu>
          )}
          <div
            id="unsorted-chat-list"
            aria-hidden={!unsortedExpanded}
            {...(!unsortedExpanded ? { inert: '' } : {})}
            className={`grid transition-[grid-template-rows,opacity] duration-200 ease-out motion-reduce:transition-none ${unsortedExpanded ? 'grid-rows-[1fr] opacity-100' : 'grid-rows-[0fr] opacity-0'}`}
          >
            <div className="min-h-0 overflow-hidden">
              {searchModel.unfolderedSessionIds.map((id) => (
              <SubagentSessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={rowSelection(id)} onSessionClick={handleSessionClick} onSessionOpen={onSessionOpen} />
              ))}
            </div>
          </div>
        </div>
      )}

      {orphanSubagents.length > 0 && <section className="mt-2 px-2" aria-label="Subagents with unavailable parents">
        <p className="text-xs" style={{ color: 'var(--text-3)' }}>Subagents · Parent unavailable</p>
        {orphanSubagents.map(child => <OrphanSubagentItem key={child.id} child={child} />)}
      </section>}
      </>}
      </div>
      </div>

      {searchModel.isSearching && !searchModel.hasMatches && orphanSubagents.length === 0 && (
        <div className="mx-3 my-6 flex flex-col items-center gap-1 text-center animate-fade-in" style={{ color: 'var(--text-3)' }}>
          <SearchX size={22} strokeWidth={1.5} aria-hidden />
          <div className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>No chats match this search</div>
          <div className="text-[11px]">Try another term or clear the filters.</div>
        </div>
      )}

      <DeleteConfirmDialog
        open={Boolean(pendingBulkDeleteIds)}
        kind="chat"
        count={pendingBulkDeleteIds?.length ?? 0}
        name={pendingBulkDeleteIds?.length === 1 ? sessions.find((session) => session.id === pendingBulkDeleteIds[0])?.name : undefined}
        contents={relatedBranchContents(pendingBulkDeleteIds?.length ?? 0, pendingBulkDeleteFamilyIds?.length ?? 0)}
        returnFocusTo={() => rowFocusTarget('data-session-id', pendingBulkDeleteIds?.[0])}
        error={bulkDeleteFailed ? chatDeleteFailure(pendingBulkDeleteIds?.length ?? 0) : ''}
        busy={bulkDeleting}
        onCancel={() => { if (!bulkDeleteInFlightRef.current) setPendingBulkDeleteIds(null) }}
        onConfirm={() => {
          if (!pendingBulkDeleteFamilyIds || bulkDeleteInFlightRef.current) return
          const ids = pendingBulkDeleteFamilyIds
          bulkDeleteInFlightRef.current = true
          setBulkDeleting(true)
          setBulkDeleteFailed(false)
          void (async () => {
            try {
              if (await deleteSessions(ids)) clearBulkSelection()
              // Partial deletion: refused chats were reported by name and stay selected for another try.
              else if (ids.some((id) => !useAppStore.getState().sessions.some((session) => session.id === id))) setPendingBulkDeleteIds(null)
              else setBulkDeleteFailed(true)
            } catch (cause) {
              setBulkDeleteFailed(true)
              reportAppFailure('Could not confirm chat deletion.', cause instanceof Error ? cause.message : 'Check the chat state before trying again.')
            } finally {
              bulkDeleteInFlightRef.current = false
              setBulkDeleting(false)
            }
          })()
        }}
      />

      {pendingDeleteCollections?.length === 1 && resourceCollections.filter((collection) => collection.id === pendingDeleteCollections[0].id).map((collection) => (
        <CollectionDeleteDialog key={collection.id} open collection={collection} onClose={() => setPendingDeleteCollections(null)} />
      ))}
      <DeleteConfirmDialog
        open={(pendingDeleteCollections?.length ?? 0) > 1}
        kind="collection"
        count={pendingDeleteCollections?.length ?? 0}
        onCancel={() => setPendingDeleteCollections(null)}
        onConfirm={() => {
          const targets = pendingDeleteCollections ?? []
          setPendingDeleteCollections(null)
          void runForEach(targets, async ({ id }) => await useAppStore.getState().deleteResourceCollection(id) ? null : 'The collection could not be deleted.', 'Could not delete collections')
            .then((deleted) => setSelectedCollectionIds((current) => new Set([...current].filter((id) => !deleted.includes(id)))))
        }}
      >
        <p className="mt-2">What is inside a nested one moves to its parent; a top-level one's child collections become top level, its workspaces become ungrouped and its other links are removed. Workspaces remain, but these collections cannot be restored.</p>
      </DeleteConfirmDialog>

      {recoveryDialogOpen && (
        <WorkspaceFolderRecoveryModal
          dialogRef={recoveryDialogRef}
          preview={recoveryPreview}
          loading={recoveryLoading}
          applying={recoveryApplying}
          error={workspaceFolderRecoveryError}
          onCancel={closeWorkspaceRecovery}
          onApply={applyWorkspaceRecovery}
          onDeleteUnavailable={(ids) => {
            recoveryRequestRef.current++
            setRecoveryDialogOpen(false)
            setBulkDeleteFailed(false)
            setPendingBulkDeleteIds(ids)
          }}
        />
      )}

      {/* Add folder */}
      <div ref={addControlsRef} className="mt-1 px-2.5" style={isCompanionContext() ? { display: 'none' } : undefined}>
        {actionError && (
          <Notice tone="error" title="Workspace action failed" dismissLabel="Dismiss folder action error" onDismiss={() => setActionError('')}>
            {actionError}
          </Notice>
        )}
        {addingFolder ? (
          <div className="uam-inline-form uam-reveal" role="group" aria-label="New workspace">
            <div className="uam-inline-form__title">New workspace</div>
            {executionHosts.length > 1 && (
              <div>
                <label className="uam-inline-form__label">Runs on</label>
                <MenuSelect
                  label="Workspace computer"
                  value={newFolderExecutionHostId}
                  options={executionHosts.map((host) => ({
                    value: host.id,
                    label: host.label,
                    description: host.transport === 'local' ? 'This computer' : `${host.sshAlias} · ${host.runnerStatus}`,
                  }))}
                  onChange={(hostId) => {
                    setNewFolderExecutionHostId(hostId)
                    setNewFolderDirectory('')
                    setActionError('')
                  }}
                />
              </div>
            )}
            <div>
              <label className="uam-inline-form__label" htmlFor="uam-new-workspace-directory">Folder</label>
              <div className="flex items-center gap-1.5">
                <input
                  id="uam-new-workspace-directory"
                  autoFocus
                  value={newFolderDirectory}
                  onChange={(e) => setNewFolderDirectory(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') commitAddFolder()
                    if (e.key === 'Escape') cancelAddFolder()
                  }}
                  placeholder={newFolderIsRemote ? 'Absolute directory on selected computer' : 'Workspace directory'}
                  className="uam-field min-w-0 flex-1"
                />
                <Button
                  variant="secondary"
                  size="sm"
                  disabled={newFolderIsRemote && (!newFolderExecutionHost || !isRemoteDirectoryBrowseAvailable(newFolderExecutionHost))}
                  onClick={() => { void chooseNewFolderDirectory() }}
                >
                  Browse
                </Button>
              </div>
            </div>
            <div>
              <label className="uam-inline-form__label" htmlFor="uam-new-workspace-name">Name</label>
              <input
                id="uam-new-workspace-name"
                value={newFolderName}
                onChange={(e) => setNewFolderName(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') commitAddFolder()
                  if (e.key === 'Escape') cancelAddFolder()
                }}
                placeholder="Folder name"
                className="uam-field w-full"
              />
            </div>
            {newFolderIsRemote && (
              <p className="text-[11px]" style={{ color: 'var(--text-3)' }}>
                The path is interpreted only by {newFolderExecutionHost?.label}. Browse is read-only and requires a ready helper.
              </p>
            )}
            <div className="uam-inline-form__actions">
              <span className="uam-inline-form__hint">↵ create · esc cancel</span>
              <IconButton
                icon={<X size={14} />}
                label="Cancel new workspace"
                variant="danger"
                size="sm"
                style={{ color: 'var(--error)' }}
                onClick={cancelAddFolder}
              />
              <IconButton
                icon={<Check size={14} />}
                label="Create workspace"
                size="sm"
                style={{ background: 'var(--accent)', borderColor: 'var(--accent)', color: 'var(--on-accent)' }}
                disabled={!newFolderName.trim() || !newFolderDirectory.trim()}
                onClick={commitAddFolder}
              />
            </div>
          </div>
        ) : addingCollection ? (
          <div className="uam-inline-form uam-reveal" role="group" aria-label="New collection">
            <div className="uam-inline-form__title">New collection</div>
            <input
              autoFocus
              aria-label="Collection name"
              value={newCollectionName}
              onChange={(event) => setNewCollectionName(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === 'Enter') commitAddCollection()
                if (event.key === 'Escape') { setNewCollectionName(''); setAddingCollection(false) }
              }}
              placeholder="Collection name"
              className="uam-field w-full"
            />
            <div className="uam-inline-form__actions">
              <Button variant="ghost" size="sm" aria-label="Cancel new collection" onClick={() => { setNewCollectionName(''); setAddingCollection(false) }}>Cancel</Button>
              <Button variant="primary" size="sm" aria-label="Create collection" disabled={!newCollectionName.trim()} onClick={commitAddCollection}>Create</Button>
            </div>
          </div>
        ) : (
          <div className="flex justify-center gap-1">
            <Button asLink leadingIcon={<Plus size={14} aria-hidden />} onClick={() => { setActionError(''); setAddingFolder(true) }}>New workspace</Button>
            <Button asLink leadingIcon={<Plus size={14} aria-hidden />} onClick={() => { setActionError(''); setAddingCollection(true) }}>New collection</Button>
          </div>
        )}
      </div>

      {remoteBrowserOpen && isSshHost(newFolderExecutionHost) && (
        <RemoteDirectoryBrowser
          host={newFolderExecutionHost}
          initialPath={newFolderDirectory}
          onCancel={() => setRemoteBrowserOpen(false)}
          onSelect={(directory) => {
            setNewFolderDirectory(directory)
            setRemoteBrowserOpen(false)
          }}
        />
      )}

      <DeleteConfirmDialog
        open={!companionContext && workspaceDeleteCount > 0}
        kind="workspace"
        count={workspaceDeleteCount}
        name={singleWorkspaceDelete?.name}
        title={singleWorkspaceDelete && workspaceDeleteChatCount > 0 ? 'Delete workspace and chats?' : undefined}
        contents={workspaceDeleteChatCount > 0 ? countLabel(workspaceDeleteChatCount, 'chat') : undefined}
        busy={deletingFolder}
        error={deleteFolderError || undefined}
        returnFocusTo={() => rowFocusTarget('data-workspace-id', pendingDeleteFolderIds?.[0] ?? pendingDeleteFolderId)}
        onCancel={() => {
          if (deletingFolder) return
          setPendingDeleteFolderIds(null)
          setPendingDeleteFolderId(null)
          setDeleteFolderError('')
        }}
        onConfirm={() => { void (pendingDeleteFolderIds ? confirmDeleteWorkspaces() : confirmDeleteFolder()) }}
      >
        <p className="mt-2">{singleWorkspaceDelete ? 'The folder on disk is kept.' : 'Workspace directories stay on disk.'}</p>
        {singleWorkspaceDelete?.directory && <p className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>{singleWorkspaceDelete.directory}</p>}
        {workspaceDeleteChatCount > 0 && <p className="mt-2 text-xs" style={{ color: 'var(--error)' }}>Deleted chats cannot be restored from this app.</p>}
        {deletingFolder && <DeletionProgressBar label={workspaceDeleteCount === 1 ? 'Deleting workspace' : 'Deleting workspaces'} className="mt-2" />}
      </DeleteConfirmDialog>
      <div ref={moveStatusRef} className="sr-only" role="status" aria-live="polite" data-testid="sidebar-move-status" />
    </div>
    </ChatSortContext.Provider>
    </ChatSelectionContext.Provider>
    </SubagentSearchMatches.Provider>
  )
}

/** The sortable row with `id` inside `scope`; chat ids can appear in both Pinned and Active, so callers scope the lookup. */
const sortRow = (scope: ParentNode | null | undefined, id: string) =>
  Array.from(scope?.querySelectorAll<HTMLElement>('[data-sort-id]') ?? []).find((row) => row.dataset.sortId === id)

const COLLECTION_DRAG_TYPE = 'text/x-uam-collection-id'
// dataTransfer values are unreadable during dragover, so valid drop targets read the dragged id from here.
let draggedCollectionId: string | null = null

/** A reference with no home of its own once its collection goes, unlike a workspace or chat, which stays in the sidebar. */
const hasOtherLinks = (collection: ResourceCollection) => collection.references.some((reference) => reference.type !== 'workspace-folder' && reference.type !== 'chat')

/** Move contents up is a real choice only when it keeps something Delete all would not. */
function canMoveContentsUp(collections: readonly ResourceCollection[], collection: ResourceCollection) {
  return collections.some((entry) => entry.parentId === collection.id) || (collections.some((entry) => entry.id === collection.parentId) && collection.references.length > 0)
}

function collectionDeleteMessage(collections: readonly ResourceCollection[], collection: ResourceCollection): string[] {
  const subtree = collectionSubtreeIds(collections, collection.id)
  const inside = subtree.length - 1
  const parent = collections.find((entry) => entry.id === collection.parentId)
  const ownLinks = hasOtherLinks(collection)
  const direct = collections.filter((entry) => entry.parentId === collection.id).length
  const anyLinks = collections.some((entry) => subtree.includes(entry.id) && hasOtherLinks(entry))
  return [
    `The collection “${collection.name}” will be permanently deleted. This cannot be undone.${inside > 0 ? ` It has ${inside} ${inside === 1 ? 'collection' : 'collections'} inside${inside > direct ? ', counting nested ones' : ''}.` : ''}`,
    parent
      ? `Move contents up moves everything inside to “${parent.name}”.`
      : `Move contents up moves its child collections to the top level and leaves its workspaces ungrouped${ownLinks ? '; its other links are removed' : ''}.`,
    `Delete all ${inside > 0 ? 'permanently deletes the collections inside too, and its workspaces and theirs become ungrouped' : 'leaves its workspaces ungrouped'}${anyLinks ? '; other links inside are removed' : ''}. Workspaces and chats remain.`,
  ]
}

/** One collection's delete prompt, from its menu or as a one-item selection. */
function CollectionDeleteDialog({ collection, open, onClose }: { collection: ResourceCollection; open: boolean; onClose: () => void }) {
  const collections = useAppStore((state) => state.resourceCollections)
  const remove = useAppStore((state) => state.deleteResourceCollection)
  const offerMoveUp = canMoveContentsUp(collections, collection)
  const removeCollection = (deleteContents: boolean) => {
    const parent = collections.find((entry) => entry.id === collection.parentId)
    // Only moving references up can hit the parent's limit.
    const mayBeFull = !deleteContents && parent && collection.references.length > 0
    onClose()
    void remove(collection.id, deleteContents).then((removed) => {
      if (!removed) reportAppFailure(`Could not delete "${collection.name}".`, mayBeFull ? `A collection holds at most 500 items, so “${parent.name}” may be full. Refresh and try again.` : 'Could not save the change. Refresh and try again.')
    })
  }
  if (!offerMoveUp) {
    return (
      <DeleteConfirmDialog open={open} kind="collection" count={1} name={collection.name} onCancel={onClose} onConfirm={() => removeCollection(false)}>
        <p className="mt-2">Workspaces remain, but this collection cannot be restored.{hasOtherLinks(collection) ? ' Its other links are removed.' : ''}</p>
      </DeleteConfirmDialog>
    )
  }
  // ConfirmDialog directly: the shared delete confirmation has no middle action or custom confirm label.
  return (
    <ConfirmDialog
      open={open}
      title="Delete collection?"
      label={`Delete collection ${collection.name}`}
      confirmLabel="Delete all"
      secondaryAction={{ label: 'Move contents up', onClick: () => removeCollection(false) }}
      onCancel={onClose}
      onConfirm={() => removeCollection(true)}
    >
      {collectionDeleteMessage(collections, collection).map((line, index) => <p key={index} className={index > 0 ? 'mt-2' : undefined}>{line}</p>)}
    </ConfirmDialog>
  )
}

async function moveCollection(collectionId: string, parentId: string | null) {
  if (await useAppStore.getState().moveResourceCollection(collectionId, parentId)) return true
  reportAppFailure('Could not move the collection.', 'Collections can nest at most 5 levels deep and never inside themselves. Refresh and try again.')
  return false
}

function FolderCollection({ collection, folderCount, hiddenPaneColors, selected, selectionTargets, onHeaderClick, onDeleteSelected, children }: {
  collection: ResourceCollection; folderCount: number; hiddenPaneColors: string[]; selected: boolean
  /** Selected collections the menu acts on when this header is part of a multi-selection. */
  selectionTargets: BulkTarget[] | null
  onHeaderClick: (event: ReactMouseEvent<HTMLElement>) => void; onDeleteSelected: () => void
  children: ReactNode
}) {
  const iconPicker = useCustomIconPicker()
  const toggle = useAppStore((state) => state.toggleResourceCollection)
  const rename = useAppStore((state) => state.renameResourceCollection)
  const collections = useAppStore((state) => state.resourceCollections)
  const childCount = collections.filter((entry) => entry.parentId === collection.id).length
  const moveTargets = collections.filter((entry) => entry.id !== collection.parentId && canNestCollection(collections, collection.id, entry.id))
  const canNestInside = canNestCollection(collections, null, collection.id)
  const [addingChild, setAddingChild] = useState(false)
  const [childName, setChildName] = useState('')
  const [editing, setEditing] = useState(false)
  const [name, setName] = useState(collection.name)
  const [menuOpen, setMenuOpen] = useState(false)
  const [menuPoint, setMenuPoint] = useState<{ x: number; y: number } | null>(null)
  const [dropTarget, setDropTarget] = useState(false)
  const [confirmingDelete, setConfirmingDelete] = useState(false)
  const activeSessionId = useAppStore((state) => state.activeSessionId)
  const menuTriggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)

  useEffect(() => { setMenuOpen(false) }, [activeSessionId])

  useEffect(() => {
    if (!menuOpen) return
    menuRef.current?.querySelector<HTMLButtonElement>('button:not([aria-disabled="true"])')?.focus()
    const onDown = (event: MouseEvent) => {
      // Submenus are portaled outside menuRef; pressing one must not unmount it before its click lands.
      if (fromViewportMenu(event)) return
      const target = event.target as Node
      if (!menuRef.current?.contains(target) && !menuTriggerRef.current?.contains(target)) { setMenuOpen(false); setMenuPoint(null) }
    }
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setMenuOpen(false)
      setMenuPoint(null)
      menuTriggerRef.current?.focus()
    }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [menuOpen])

  const commitName = () => {
    const trimmed = name.trim()
    if (trimmed && trimmed !== collection.name) void rename(collection.id, trimmed)
    else setName(collection.name)
    setEditing(false)
  }
  const cancelChild = () => { setChildName(''); setAddingChild(false) }
  const commitChild = () => {
    const trimmed = childName.trim()
    if (!trimmed) return
    void useAppStore.getState().createResourceCollection(trimmed, collection.id).then((created) => {
      if (created) cancelChild()
      else reportAppFailure('Could not create the collection.', 'Collections can nest at most 5 levels deep. Try again.')
    })
  }
  const startChild = () => {
    setMenuOpen(false)
    if (collection.collapsed) void toggle(collection.id)
    setAddingChild(true)
  }
  const isCollectionDrag = (event: { dataTransfer: DataTransfer }) => event.dataTransfer.types.includes(COLLECTION_DRAG_TYPE)

  return (
    <div
      data-testid={`folder-collection-${collection.id}`}
      // Workspace rows drag by pointer (sortableDrag) and find their target collection through this attribute.
      data-drop-collection-id={collection.id}
      // The lifted workspace covers the header, so a workspace drop outlines the whole collection (uam-sb-drop-target).
      className="mb-0 rounded-md"
      onDragOver={(event) => {
        if (!isCollectionDrag(event)) return
        // Only the innermost collection is a target; invalid nesting targets do not accept the drop.
        event.stopPropagation()
        if (!draggedCollectionId || !canNestCollection(collections, draggedCollectionId, collection.id)) return
        event.preventDefault()
        setDropTarget(true)
      }}
      onDragLeave={() => setDropTarget(false)}
      onDrop={(event) => {
        const nestedId = event.dataTransfer.getData(COLLECTION_DRAG_TYPE)
        if (!nestedId) return
        event.preventDefault()
        event.stopPropagation()
        setDropTarget(false)
        if (canNestCollection(collections, nestedId, collection.id)) void moveCollection(nestedId, collection.id)
      }}
    >
      <Tooltip label={isCompanionContext() ? undefined : 'Shift-click to select collections'}>
      <div
        data-testid={`collection-header-${collection.id}`}
        data-collection-id={collection.id}
        draggable={!isCompanionContext() && !editing}
        onDragStart={(event) => {
          event.stopPropagation()
          draggedCollectionId = collection.id
          event.dataTransfer.setData(COLLECTION_DRAG_TYPE, collection.id)
          event.dataTransfer.effectAllowed = 'move'
        }}
        onDragEnd={() => { draggedCollectionId = null }}
        className={`group relative mx-1 flex items-center gap-1.5 rounded-md px-2.5 py-0.5 transition-colors duration-150${selected && !dropTarget ? ' uam-selected-item' : ''}`}
        // Selected tint and line come from the shared .uam-selected-item; inline paint only for drop targets and rest.
        style={{ color: selected ? 'var(--text)' : 'var(--text-2)', background: dropTarget ? 'var(--sidebar-item-hover)' : selected ? undefined : 'transparent', border: '1px solid transparent', outline: dropTarget ? '1px solid var(--accent)' : undefined }}
        onContextMenu={(event) => {
          event.preventDefault()
          event.stopPropagation()
          setMenuPoint({ x: event.clientX, y: event.clientY })
          setMenuOpen(true)
        }}
      >
        <Tooltip label={collection.collapsed ? `Expand ${collection.name}` : `Collapse ${collection.name}`}>
          {/* ui: raw button kept — unsized inline chevron that must keep its layout; IconButton imposes 26px */}<button
            type="button"
            aria-label={collection.collapsed ? `Expand ${collection.name}` : `Collapse ${collection.name}`}
            aria-expanded={!collection.collapsed}
            aria-controls={`collection-children-${collection.id}`}
            onClick={() => { void toggle(collection.id) }}
            style={{ display: 'flex', background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer', padding: 0 }}
          >
            <ChevronRight size={14} className="transition-transform duration-200 ease-out motion-reduce:transition-none" style={{ transform: collection.collapsed ? 'rotate(0deg)' : 'rotate(90deg)' }} />
          </button>
        </Tooltip>
        {selected ? <Check size={14} style={{ color: 'var(--accent)', flexShrink: 0 }} aria-hidden /> : <CustomIcon icon={collection.customIcon} fallback={<PaneColorIcon Icon={Library} colors={collection.collapsed ? hiddenPaneColors : []} testId={`collection-icon-${collection.id}`} />} />}
        {editing ? (
          <input
            autoFocus
            aria-label={`Rename ${collection.name}`}
            value={name}
            onChange={(event) => setName(event.target.value)}
            onBlur={commitName}
            onKeyDown={(event) => {
              if (event.key === 'Enter') commitName()
              if (event.key === 'Escape') { setName(collection.name); setEditing(false) }
            }}
            className="min-w-0 flex-1 bg-transparent text-xs outline-none"
            style={{ color: 'var(--text)', borderBottom: '1px solid var(--accent)' }}
          />
        ) : (
          <Tooltip label={collection.name}>
            <RowButton type="button" aria-expanded={!collection.collapsed} aria-controls={`collection-children-${collection.id}`} aria-label={`${collection.name}${selected ? ', selected for bulk actions' : ''}`} className="min-w-0 flex-1 truncate text-left text-[13px] font-semibold" onClick={onHeaderClick} style={{ background: 'transparent', border: 'none', color: 'inherit', cursor: 'pointer' }}>
              {collection.name}
            </RowButton>
          </Tooltip>
        )}
        <span data-testid={`collection-count-${collection.id}`} className="text-[10px]" style={{ color: 'var(--text-3)' }}>{folderCount + childCount}</span>
        {!isCompanionContext() && <Tooltip label="Collection actions">
          {/* ui: raw button kept — unsized inline trigger that must keep its layout; IconButton imposes 26px */}<button ref={menuTriggerRef} type="button" aria-label={`Actions for ${collection.name}`} aria-haspopup="menu" aria-expanded={menuOpen} onClick={() => { setMenuPoint(null); setMenuOpen((open) => !open) }} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer', padding: 0 }}>
            <MoreHorizontal size={14} />
          </button>
        </Tooltip>}
      </div>
      </Tooltip>
      {menuOpen && (
        <ViewportMenu ref={menuRef} {...(menuPoint ? { point: menuPoint } : { anchorRef: menuTriggerRef, align: 'end' as const })} role="menu" aria-label={`${collection.name} actions`} onRequestClose={() => { setMenuOpen(false); setMenuPoint(null) }} onClick={(e) => e.stopPropagation()} className="fixed z-50 rounded-md py-1 animate-fade-in" style={{ minWidth: 168, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}>
          {selectionTargets ? <>
            <SingleOnlyMenuItem noun="collection" icon={<Pencil size={14} aria-hidden />} label="Rename" />
            <FolderMenuItem icon={<Library size={14} aria-hidden />} label={`Change icon for ${selectionTargets.length} collections`} onClick={() => { setMenuOpen(false); iconPicker.open('collection', selectionTargets, `${selectionTargets.length} collections`) }} />
            <SingleOnlyMenuItem noun="collection" icon={<Plus size={14} aria-hidden />} label="New collection inside" />
            <SingleOnlyMenuItem noun="collection" icon={<Library size={14} aria-hidden />} label="Move to collection" />
            <SingleOnlyMenuItem noun="collection" icon={<Library size={14} aria-hidden />} label="Move to top level" />
            <MenuDivider />
            <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label={`Delete ${selectionTargets.length} collections…`} danger onClick={() => { setMenuOpen(false); onDeleteSelected() }} />
          </> : <>
          <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label="Rename" onClick={() => { setMenuOpen(false); setEditing(true) }} />
          <FolderMenuItem icon={<Library size={14} aria-hidden />} label="Change icon" onClick={() => { setMenuOpen(false); iconPicker.open('collection', collection.id, collection.name) }} />
          {canNestInside && <FolderMenuItem icon={<Plus size={14} aria-hidden />} label="New collection inside" onClick={startChild} />}
          {moveTargets.length > 0 && (
            <RowSubmenu label="Move to collection" ariaLabel={`Move ${collection.name} to collection`} icon={<Library size={14} aria-hidden />}>
              {moveTargets.map((target) => (
                <MenuItem key={target.id} className="uam-sb-menu-item" icon={<Library size={13} aria-hidden />} label={collectionPath(collections, target.id)} onSelect={() => { setMenuOpen(false); void moveCollection(collection.id, target.id) }} />
              ))}
            </RowSubmenu>
          )}
          {collection.parentId && <FolderMenuItem icon={<Library size={14} aria-hidden />} label="Move to top level" onClick={() => { setMenuOpen(false); void moveCollection(collection.id, null) }} />}
          <MenuDivider />
          <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label="Delete" danger onClick={() => { setMenuOpen(false); setConfirmingDelete(true) }} />
          </>}
        </ViewportMenu>
      )}
      <div
        aria-hidden={collection.collapsed}
        {...(collection.collapsed ? { inert: '' } : {})}
        className={`grid transition-[grid-template-rows,opacity] duration-200 ease-out motion-reduce:transition-none ${collection.collapsed ? 'grid-rows-[0fr] opacity-0' : 'grid-rows-[1fr] opacity-100'}`}
      >
        <div className="min-h-0 overflow-hidden">
          <div
            id={`collection-children-${collection.id}`}
            data-testid={`collection-children-${collection.id}`}
            role="group"
            aria-label={`${collection.name} contents`}
            className="ml-3 mr-1 mt-0 py-0 pl-1"
            style={{
              background: 'transparent',
              boxShadow: 'none',
            }}
          >
            {addingChild && (
              <div className="uam-inline-form uam-reveal" role="group" aria-label={`New collection inside ${collection.name}`}>
                <div className="uam-inline-form__title">New collection</div>
                <input
                  autoFocus
                  aria-label="Collection name"
                  value={childName}
                  onChange={(event) => setChildName(event.target.value)}
                  onKeyDown={(event) => {
                    if (event.key === 'Enter') commitChild()
                    if (event.key === 'Escape') cancelChild()
                  }}
                  placeholder="Collection name"
                  className="uam-field w-full"
                />
                <div className="uam-inline-form__actions">
                  <Button variant="ghost" size="sm" aria-label="Cancel new collection" onClick={cancelChild}>Cancel</Button>
                  <Button variant="primary" size="sm" aria-label="Create collection" disabled={!childName.trim()} onClick={commitChild}>Create</Button>
                </div>
              </div>
            )}
            {children}
            {collection.references.filter((reference) => reference.type === 'desktop-app').map((reference) => <div key={reference.id} className="flex items-center gap-2 px-3 py-1 text-xs" onContextMenu={(event) => { event.preventDefault(); iconPicker.open('desktop-app', reference.id, reference.label) }}><CustomIcon icon={reference.customIcon} fallback={<Monitor size={14} aria-hidden />} /><span className="min-w-0 flex-1 truncate" title={reference.target}>{reference.label}</span><Tooltip label={`Change icon for ${reference.label}`}>{/* ui: raw button kept — nested inside its own Tooltip; inline icon chip */}<button type="button" aria-label={`Change icon for ${reference.label}`} className="uam-icon-button" onClick={() => iconPicker.open('desktop-app', reference.id, reference.label)}><Pencil size={12} /></button></Tooltip></div>)}
            {folderCount === 0 && childCount === 0 && <div className="px-3 py-2 text-xs" style={{ color: 'var(--text-3)' }}>Drag a workspace here</div>}
          </div>
        </div>
      </div>
      {iconPicker.dialog}
      <CollectionDeleteDialog collection={collection} open={confirmingDelete} onClose={() => setConfirmingDelete(false)} />
    </div>
  )
}

function PaneColorIcon({ Icon, colors, testId }: { Icon: LucideIcon; colors: string[]; testId: string }) {
  const paneNumbers = colors.flatMap((color) => {
    const index = chatPaneColors.findIndex((paneColor) => paneColor === color)
    return index < 0 ? [] : [index + 1]
  })
  const paneLabel = paneNumbers.length === 1
    ? `Shown in pane ${paneNumbers[0]}`
    : `Shown in panes ${paneNumbers.slice(0, -1).join(', ')} and ${paneNumbers[paneNumbers.length - 1]}`
  return (
    <span
      {...(colors.length > 0 ? { role: 'img', 'aria-label': paneLabel } : {})}
      className="inline-flex shrink-0 items-center gap-1"
    >
      <Icon
        data-testid={testId}
        size={14}
        style={{ flexShrink: 0, color: 'var(--text-3)', filter: 'none', opacity: 0.85 }}
        aria-hidden
      />
      {colors.map((color, index) => (
        <span
          key={`${color}-${index}`}
          data-testid={`${testId}-marker`}
          className="h-1.5 w-1.5 rounded-full"
          style={{ background: color }}
          aria-hidden
        />
      ))}
    </span>
  )
}

/** The sidebar row a delete confirmation returns focus to when its opening menu item has gone. */
function rowFocusTarget(attribute: 'data-session-id' | 'data-workspace-id', id: string | null | undefined): HTMLElement | null {
  if (!id) return null
  return Array.from(document.querySelectorAll<HTMLElement>(`[${attribute}]`)).find((row) => row.getAttribute(attribute) === id) ?? null
}

// Determinate only when the backend reports per-item progress; otherwise the indeterminate sweep (no fake percentage).
function DeletionProgressBar({ label, className }: { label: string; className?: string }) {
  const progress = useAppStore((s) => s.deletionProgress)
  const row = `flex h-6 min-w-0 items-center justify-between gap-2 rounded-md px-2 text-xs${className ? ` ${className}` : ''}`
  if (!progress || progress.total <= 0) {
    return <ProgressFill label={label} indeterminate className={row} />
  }
  return <ProgressFill label={label} value={progress.completed} max={progress.total} className={row}>
    <span className="shrink-0 tabular-nums">{progress.completed} of {progress.total} removed</span>
    {progress.currentLabel && <span className="truncate" style={{ color: 'var(--text-3)' }}>{progress.currentLabel}</span>}
  </ProgressFill>
}

interface WorkspaceFolderRecoveryModalProps {
  dialogRef: RefObject<HTMLDivElement>
  preview: WorkspaceFolderRecoveryPreview | null
  loading: boolean
  applying: boolean
  error: string
  onCancel: () => void
  onApply: () => void
  onDeleteUnavailable: (ids: string[]) => void
}

function WorkspaceFolderRecoveryModal({
  dialogRef,
  preview,
  loading,
  applying,
  error,
  onCancel,
  onApply,
  onDeleteUnavailable,
}: WorkspaceFolderRecoveryModalProps) {
	const executionHosts = useAppStore((state) => state.executionHosts)
  const readyChatCount = preview?.groups.reduce((count, group) => count + group.chatIds.length, 0) ?? 0
  const newFolderCount = preview?.groups.filter((group) => !group.existingFolderId).length ?? 0
  const unavailableChats = [...(preview?.missing ?? []), ...(preview?.unavailable ?? [])]
  const unavailableIds = Array.from(new Set(unavailableChats.map((chat) => chat.id)))
  const attentionSections = preview ? [
    { title: 'Location not found', chats: preview.missing },
    { title: 'Location unavailable', chats: preview.unavailable },
    { title: 'No saved location', chats: preview.noLocation },
  ].filter((section) => section.chats.length > 0) : []
  const hasResults = (preview?.groups.length ?? 0) > 0 || attentionSections.length > 0

  return (
    <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!applying) onCancel() }}>
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label="Rebuild workspace folders"
        tabIndex={-1}
        className="flex max-h-[calc(100vh-2rem)] w-full max-w-2xl flex-col overflow-hidden rounded-xl"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
      >
        <div className="flex items-start justify-between gap-4 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div>
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Rebuild workspace folders</div>
            <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>Review what will change before organising Unsorted chats.</div>
          </div>
          <IconButton icon={<X size={16} />} label="Close workspace recovery" disabled={applying} onClick={onCancel} />
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-5">
          {loading && (
            <div role="status" className="flex items-center gap-2 rounded-md p-3 text-sm" style={{ color: 'var(--text-2)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
              <FolderSync size={16} aria-hidden />
              Checking saved workspace locations…
            </div>
          )}
          {error && <div role="alert" className="rounded-md p-3 text-sm" style={{ color: 'var(--error)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>{error}</div>}
          {!loading && preview && (
            <div className="grid gap-4">
              {preview.groups.length > 0 && (
                <section aria-label="Ready workspace folders">
                  <div className="mb-2 flex items-center gap-2">
                    <Check size={15} style={{ color: 'var(--success)' }} aria-hidden />
                    <h3 className="text-xs font-semibold" style={{ color: 'var(--text)' }}>Ready · {readyChatCount} chat{readyChatCount === 1 ? '' : 's'}</h3>
                  </div>
                  <div className="grid gap-2">
					{preview.groups.map((group) => (
					  <div key={`${group.executionHostId || 'local'}\n${group.directory}`} className="rounded-md px-3 py-2" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
                        <div className="flex items-center justify-between gap-3 text-xs">
                          <strong className="truncate" style={{ color: 'var(--text)' }}>{group.title}</strong>
                          <span className="shrink-0" style={{ color: 'var(--text-3)' }}>{group.existingFolderId ? 'Use existing' : 'Create'} · {group.chatIds.length}</span>
                        </div>
						<div className="mt-1 truncate text-[11px]" title={group.directory} style={{ color: 'var(--text-3)' }}>
						  {group.directory} · {executionHosts.find((host) => host.id === (group.executionHostId || 'local'))?.label || group.executionHostId || 'This computer'}
						</div>
                      </div>
                    ))}
                  </div>
                </section>
              )}

              {attentionSections.length > 0 && (
                <section aria-label="Chats needing attention">
                  <div className="mb-2 flex items-center gap-2">
                    <TriangleAlert size={15} style={{ color: 'var(--warning)' }} aria-hidden />
                    <h3 className="text-xs font-semibold" style={{ color: 'var(--text)' }}>Needs attention</h3>
                  </div>
                  <div className="grid gap-2">
                    {attentionSections.map((section) => (
                      <div key={section.title} className="rounded-md px-3 py-2" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
                        <div className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>{section.title} · {section.chats.length}</div>
                        {section.chats.map((chat) => (
                          <div key={chat.id} className="mt-1 min-w-0 text-[11px]" style={{ color: 'var(--text-3)' }}>
                            <span style={{ color: 'var(--text-2)' }}>{chat.title}</span>
                            {chat.directory && <span className="block truncate" title={chat.directory}>{chat.directory}</span>}
                            <span className="block">{chat.reason}</span>
                          </div>
                        ))}
                      </div>
                    ))}
                  </div>
                  <p className="mt-2 text-xs" style={{ color: 'var(--text-3)' }}>These chats stay in Unsorted unless you explicitly delete the unavailable ones.</p>
                </section>
              )}

              {!hasResults && <div className="rounded-md p-4 text-center text-sm" style={{ color: 'var(--text-3)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>No Unsorted chats need workspace recovery.</div>}
            </div>
          )}
        </div>

        <div className="flex flex-wrap items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
          {unavailableIds.length > 0 && (
            <Button variant="danger" size="sm" disabled={applying} onClick={() => onDeleteUnavailable(unavailableIds)}>
              Delete {unavailableIds.length} unavailable…
            </Button>
          )}
          <Button variant="ghost" size="sm" disabled={applying} onClick={onCancel}>Keep unchanged</Button>
          <Button variant="primary" size="sm" loading={applying} disabled={loading || readyChatCount === 0} onClick={onApply}>
            {newFolderCount > 0
              ? `Create ${newFolderCount} folder${newFolderCount === 1 ? '' : 's'} and organise ${readyChatCount} chat${readyChatCount === 1 ? '' : 's'}`
              : `Organise ${readyChatCount} chat${readyChatCount === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>
    </Overlay>
  )
}

// ---------------------------------------------------------------------------
// FolderRow - memoized so it only re-renders when its own folder/session IDs change
// ---------------------------------------------------------------------------

function FolderMenuItem({ icon, label, onClick, danger }: { icon: ReactNode; label: string; onClick: () => void; danger?: boolean }) {
  return (
    <MenuItem
      icon={icon}
      label={label}
      danger={danger}
      className={`uam-sb-menu-item${danger ? ' uam-sb-menu-item--danger' : ''}`}
      onSelect={onClick}
    />
  )
}

interface FolderRowProps {
  folder: Folder
  machineKind: 'local' | 'remote'
  machineLabel: string
  sessionIds: string[]
  shouldShowSessions: boolean
  hiddenPaneColors: string[]
  isSearching: boolean
  isEditing: boolean
  editFolderName: string
  editFolderDirectory: string
  selected: boolean
  /** Selected workspaces the menu acts on when this row is part of a multi-selection. */
  selectionTargets: BulkTarget[] | null
  onToggle: (event: ReactMouseEvent<HTMLDivElement>) => void
  onStartRename: () => void
  onDelete: () => void
  onRescan: () => void
  onEditNameChange: (v: string) => void
  onEditDirectoryChange: (v: string) => void
  onCommitRename: () => void
  onCancelEdit: () => void
  onChooseDirectory: () => void
  onCreateChat: () => void
  sessionsById: Map<string, Session>
  familySessionIdsByRootId: Map<string, string[]>
  selectedSessionIds: Set<string>
  onSessionOpen?: (sessionId: string) => void
  onSessionClick: (sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => boolean
  draggable: boolean
  onDragPointerDown: (event: ReactPointerEvent<HTMLElement>) => void
  onMove: (direction: -1 | 1) => void
}

const FolderRow = memo(function FolderRow({
  folder,
  machineKind,
  machineLabel,
  sessionIds,
  shouldShowSessions,
  hiddenPaneColors,
  isSearching,
  isEditing,
  editFolderName,
  editFolderDirectory,
  selected,
  selectionTargets,
  onToggle,
  onStartRename,
  onDelete,
  onRescan,
  onEditNameChange,
  onEditDirectoryChange,
  onCommitRename,
  onCancelEdit,
  onChooseDirectory,
  onCreateChat,
  sessionsById,
  familySessionIdsByRootId,
  selectedSessionIds,
  onSessionClick,
  onSessionOpen,
  draggable,
  onDragPointerDown,
  onMove,
}: FolderRowProps) {
  const iconPicker = useCustomIconPicker()
  const host = useAppStore((state) => state.executionHosts.find((candidate) => candidate.id === (folder.executionHostId || 'local')))
  const [showAllSessions, setShowAllSessions] = useState(false)
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const menuTriggerRef = useRef<HTMLButtonElement>(null)
  const shouldLimitSessions = !isCompanionContext() && !isSearching && sessionIds.length > VISIBLE_SESSION_LIMIT
  const visibleSessionIds = shouldLimitSessions && !showAllSessions
    ? sessionIds.slice(0, VISIBLE_SESSION_LIMIT)
    : sessionIds
  const hiddenSessionCount = sessionIds.length - visibleSessionIds.length

  useEffect(() => {
    if (sessionIds.length <= VISIBLE_SESSION_LIMIT && showAllSessions) {
      setShowAllSessions(false)
    }
  }, [sessionIds.length, showAllSessions])

  useEffect(() => {
    if (!menuPos) return
    const onDown = (e: MouseEvent) => {
      const target = e.target as Node
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (menuRef.current && !menuRef.current.contains(target)) setMenuPos(null)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setMenuPos(null) }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [menuPos])

  return (
    <div
      className="mb-0"
      data-testid={`folder-row-${folder.id}`}
      data-sort-id={folder.id}
    >
      {/* Folder header; its actions button is a sibling, not nested inside the role=button header. */}
      <div
        className="relative group"
        onContextMenu={(event) => {
          if (isCompanionContext()) { event.preventDefault(); event.stopPropagation(); return }
          event.preventDefault()
          event.stopPropagation()
          setMenuPos({ x: event.clientX, y: event.clientY })
        }}
      >
      <Tooltip label={isCompanionContext() ? undefined : 'Shift-click to select workspaces'}>
      <div
        data-testid={`folder-header-${folder.id}`}
        data-workspace-id={folder.id}
        role="button"
        aria-label={`${folder.name}${selected ? ", selected for bulk actions" : ""}`}
        tabIndex={0}
        aria-expanded={shouldShowSessions}
        aria-controls={`folder-sessions-${folder.id}`}
        aria-keyshortcuts={draggable ? "ArrowUp ArrowDown Enter Space Shift+Enter Shift+Space" : "Enter Space Shift+Enter Shift+Space"}
        className={`relative flex items-center gap-1.5 px-2.5 py-0.5 cursor-pointer rounded-md mx-1 focus-visible:[outline-offset:var(--focus-inset)]${selected ? ' uam-selected-item' : ''}`}
        style={{
          // Selected tint and line come from the shared .uam-selected-item.
          background: selected ? undefined : 'transparent',
          color: selected ? 'var(--text)' : 'var(--text-2)',
        }}
        onClick={onToggle}
        onPointerDown={draggable && !isEditing ? onDragPointerDown : undefined}
        onKeyDown={(event) => {
          if (event.currentTarget !== event.target) return
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault()
            event.currentTarget.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: event.shiftKey }))
            return
          }
          if (!draggable || (event.key !== 'ArrowUp' && event.key !== 'ArrowDown')) return
          event.preventDefault()
          onMove(event.key === 'ArrowUp' ? -1 : 1)
        }}
      >
        {selected ? <Check size={14} style={{ color: 'var(--accent)', flexShrink: 0 }} aria-hidden /> : <CustomIcon icon={folder.customIcon} secondary={host?.customIcon} fallback={shouldShowSessions ? (
          <FolderOpenIcon data-testid={`folder-icon-${folder.id}`} size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />
        ) : (
          <PaneColorIcon Icon={FolderIcon} colors={hiddenPaneColors} testId={`folder-icon-${folder.id}`} />
        )} />}
        <span className="font-semibold truncate flex-1" style={{ fontSize: 13 }}>
          {folder.name}
        </span>
        <Tooltip label={machineLabel}>
          <span data-testid={`workspace-machine-${folder.id}`} role="img" aria-label={machineLabel} className="inline-flex shrink-0 items-center justify-center" style={{ width: 16, height: 16, color: 'var(--text-3)', fontSize: 13 }}>
            {machineKind === 'remote'
              ? <Server size={13} aria-hidden />
              : <Monitor size={13} aria-hidden />}
          </span>
        </Tooltip>
        {folder.missing && (
          <span
            role="img"
            aria-label={`Workspace folder missing: ${folder.directory}`}
            title={`Workspace folder missing: ${folder.directory}`}
            data-testid={`folder-missing-${folder.id}`}
            style={{ color: 'var(--warning)', flexShrink: 0 }}
          >
            <TriangleAlert size={14} aria-hidden />
          </span>
        )}
        {/* Count */}
        <span
          className="text-xs flex-shrink-0 rounded px-1 group-hover:opacity-0 group-has-[:focus-visible]:opacity-0 transition-opacity duration-100"
          style={{ fontSize: 10, background: 'var(--surface-high)', color: 'var(--text-3)' }}
        >
          {sessionIds.length}
        </span>
      </div>
      </Tooltip>
        {/* Overflow menu trigger: revealed on hover and whenever keyboard focus is in the row. */}
        <div
          className={`absolute inset-y-0 right-3 flex items-center transition-opacity duration-100 ${menuPos ? 'opacity-100' : 'opacity-0 group-hover:opacity-100 group-has-[:focus-visible]:opacity-100'}`}
        >
          {!isCompanionContext() && <Tooltip label="Folder actions" side="top">
            {/* ui: raw button kept — inline 22px trigger; IconButton sizes are 26px and 30px */}<button
              ref={menuTriggerRef}
              type="button"
              aria-label="Folder actions"
              aria-haspopup="menu"
              aria-expanded={Boolean(menuPos)}
              className="flex items-center justify-center rounded focus-visible:[outline-offset:var(--focus-inset)]"
              style={{ width: 22, height: 22, background: 'var(--surface-up)', color: 'var(--text-3)', border: '1px solid var(--border)', cursor: 'pointer' }}
              onClick={(e) => {
                e.stopPropagation()
                if (menuPos) { setMenuPos(null); return }
                const r = e.currentTarget.getBoundingClientRect()
                setMenuPos({ x: r.right - 168, y: r.bottom + 4 })
              }}
            >
              <MoreHorizontal size={14} aria-hidden />
            </button>
          </Tooltip>}
        </div>
      </div>

      {iconPicker.dialog}
      {menuPos && (
        <ViewportMenu
          ref={menuRef}
          anchorRef={menuTriggerRef}
          point={menuPos}
          role="menu"
          aria-label={`${folder.name} actions`}
          onRequestClose={() => setMenuPos(null)}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            minWidth: 168,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
          onClick={(e) => e.stopPropagation()}
        >
          {selectionTargets ? <>
            <SingleOnlyMenuItem noun="workspace" icon={<MessageSquarePlus size={14} aria-hidden />} label="New chat" />
            <CollectionMenuItems target={folder.id} label={folder.name} targets={selectionTargets} onAdded={() => setMenuPos(null)} />
            <SingleOnlyMenuItem noun="workspace" icon={<RefreshCw size={14} aria-hidden />} label="Review chats" />
            <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label={`Change icon for ${selectionTargets.length} workspaces`} onClick={() => { setMenuPos(null); iconPicker.open('workspace', selectionTargets, `${selectionTargets.length} workspaces`) }} />
            <SingleOnlyMenuItem noun="workspace" icon={<Monitor size={14} aria-hidden />} label="Change host icon" />
            <SingleOnlyMenuItem noun="workspace" icon={<Pencil size={14} aria-hidden />} label="Rename folder" />
            <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label={`Delete ${selectionTargets.length} workspaces…`} danger onClick={() => { setMenuPos(null); onDelete() }} />
          </> : <>
          <FolderMenuItem icon={<MessageSquarePlus size={14} aria-hidden />} label="New chat" onClick={() => { setMenuPos(null); onCreateChat() }} />
          <CollectionMenuItems target={folder.id} label={folder.name} onAdded={() => setMenuPos(null)} />
          {!isCompanionContext() && <FolderMenuItem icon={<RefreshCw size={14} aria-hidden />} label="Review chats" onClick={() => { setMenuPos(null); onRescan() }} />}
          <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label="Change workspace icon" onClick={() => { setMenuPos(null); iconPicker.open('workspace', folder.id, folder.name) }} />
          <FolderMenuItem icon={<Monitor size={14} aria-hidden />} label="Change host icon" onClick={() => { setMenuPos(null); iconPicker.open('host', host?.id || 'local', host?.label || 'This computer') }} />
          {!isCompanionContext() && <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label="Rename folder" onClick={() => { setMenuPos(null); onStartRename() }} />}
          {!isCompanionContext() && <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label={selected ? 'Delete selected workspace' : 'Delete folder'} danger onClick={() => { setMenuPos(null); onDelete() }} />}
          </>}
        </ViewportMenu>
      )}

      {isEditing && (
        <div
          className="mx-3 mb-2 rounded-md p-2 space-y-2"
          style={{
            background: 'var(--surface-up)',
            border: '1px solid var(--border)',
          }}
        >
          <label className="grid gap-1 text-[11px] font-medium" style={{ color: 'var(--text-2)' }}>
            Runs on
            <input
              aria-label="Workspace computer"
              value={machineLabel.replace(/^Runs on /, '')}
              readOnly
              className="w-full rounded px-2 py-1 text-xs outline-none"
              style={{ background: 'var(--surface)', border: '1px solid var(--border)', color: 'var(--text)', fontFamily: 'inherit' }}
            />
          </label>
          <label className="grid gap-1 text-[11px] font-medium" style={{ color: 'var(--text-2)' }}>
            Name
            <input
              autoFocus
              aria-label="Workspace name"
              value={editFolderName}
              onChange={(e) => onEditNameChange(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') onCommitRename()
                if (e.key === 'Escape') onCancelEdit()
              }}
              placeholder="Folder name"
              className="w-full rounded px-2 py-1 text-xs outline-none"
              style={{ background: 'var(--surface)', border: '1px solid var(--border)', color: 'var(--text)', fontFamily: 'inherit' }}
            />
          </label>
          <label className="grid gap-1 text-[11px] font-medium" style={{ color: 'var(--text-2)' }}>
            Directory
          <div className="flex items-center gap-2">
            <input
              aria-label="Workspace directory"
              value={editFolderDirectory}
              onChange={(e) => onEditDirectoryChange(e.target.value)}
			  readOnly={machineKind === 'remote'}
              onKeyDown={(e) => {
                if (e.key === 'Enter') onCommitRename()
                if (e.key === 'Escape') onCancelEdit()
              }}
              placeholder="Workspace directory"
              className="w-full flex-1 rounded px-2 py-1 text-xs outline-none"
              style={{
                background: 'var(--surface)',
                border: '1px solid var(--border)',
                color: 'var(--text)',
                fontFamily: 'inherit',
              }}
            />
			{machineKind === 'local' && <Button variant="secondary" size="sm" onClick={onChooseDirectory}>
			  Browse
			</Button>}
          </div>
          </label>
          <div className="flex items-center justify-end gap-2">
            <Button
              variant="ghost"
              size="sm"
              onClick={onCancelEdit}
            >
              Cancel
            </Button>
            <Button
              variant="primary"
              size="sm"
              disabled={!editFolderName.trim() || !editFolderDirectory.trim()}
              onClick={onCommitRename}
            >
              Save
            </Button>
          </div>
        </div>
      )}

      {/* Sessions */}
      <div
        id={`folder-sessions-${folder.id}`}
        data-testid={`folder-sessions-${folder.id}`}
        aria-hidden={!shouldShowSessions}
        {...(!shouldShowSessions ? { inert: '' } : {})}
      >
        <div className="min-h-0 overflow-hidden">
          {shouldShowSessions && (
            <div className="uam-folder-expand-glide pl-3.5">
              {sessionIds.length === 0 ? (
                <div className="px-4 py-0.5 text-xs" style={{ color: 'var(--text-3)', opacity: 0.5, fontSize: 11 }}>
                  Empty
                </div>
              ) : (
                visibleSessionIds.map((id) => (
                  <SubagentSessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={selectedSessionIds.has(id) && (selectedSessionIds.size > 1 ? 'multi' : true)} onSessionClick={onSessionClick} onSessionOpen={onSessionOpen} />
                ))
              )}
              {shouldLimitSessions && (
                // ui: raw button kept — inline text link with its own text-xs size and JS hover colour; Button asLink is a 26px pill
                <button
                  type="button"
                  onClick={() => setShowAllSessions((value) => !value)}
                  className="mx-3 mt-0.5 inline-flex items-center px-2 py-0.5 text-xs transition-colors duration-100"
                  style={{
                    background: 'transparent',
                    color: 'var(--text-3)',
                    border: 'none',
                    cursor: 'pointer',
                    fontFamily: 'inherit',
                  }}
                  onMouseEnter={(e) => {
                    e.currentTarget.style.color = 'var(--text-2)'
                  }}
                  onMouseLeave={(e) => {
                    e.currentTarget.style.color = 'var(--text-3)'
                  }}
                >
                  {showAllSessions ? 'Show less' : `Show ${hiddenSessionCount} more`}
                </button>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  )
})
