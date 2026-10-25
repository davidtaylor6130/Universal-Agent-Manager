import { returnToSubagentParent, subagentRouteFor, subscribeSubagentNavigation, subagentNavigationRevision, hasNativeSubagentLineage, subagentParentFor } from '../../utils/subagentNavigation'
import { SnoozeMenu } from './SnoozeMenu'
import { RowSubmenu } from './RowSubmenu'
import { isCompanionContext, isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { useState, useRef, useEffect, useLayoutEffect, useSyncExternalStore, memo, useContext } from 'react'
import type { MouseEvent as ReactMouseEvent, RefObject } from 'react'
import {
  Pin, MoreHorizontal, Pencil, Trash2, HelpCircle, ClipboardList, Brain,
  ShieldCheck, SquareChevronRight, FileText, TriangleAlert, CircleAlert, Check,
  Folder as FolderIcon, GitBranch, Monitor, Server, CircleDashed, CircleCheck,
  MessageCircleQuestion, ShieldQuestion, Copy, RefreshCw, MessageSquarePlus, AlarmClockOff, Clock, SmilePlus, Bot,
} from 'lucide-react'
import { useAppStore, type AcpAttentionKind } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { shallow } from 'zustand/shallow'
import type { Session } from '../../types/session'
import { ConfirmDialog, IconButton, Tooltip, ViewportMenu } from '../ui'
import { ProviderLogo } from '../shared/ProviderLogo'
import {
  chatGridLeaves,
  chatPaneColors,
  activateExistingChatPane,
  readChatGridLayout,
  subscribeChatGridLayout,
} from '../../utils/chatGridStorage'
import { providerShortName } from '../../utils/providerMetadata'
import { preferredFamilyChat } from '../../utils/branchPreferenceStorage'
import { CollectionMenuItems } from './CollectionMenuItems'
import { ChatSelectionContext, CopyDetailItems, MenuDivider, SingleOnlyMenuItem, runForEach, type BulkTarget, type CopyDetail } from './bulkActions'
import { displayedChatStatus, type DisplayedChatStatus } from './chatSearch'
import { CustomIcon, useCustomIconPicker } from '../shared/CustomIcon'
import { resolveCustomIcon } from '../../types/customIcon'
import { reportAppFailure } from '../../utils/appFailure'
import type { CustomIcon as CustomIconValue } from '../../types/customIcon'
import { SSH_ENABLED } from '../../config/buildFeatures'

export function formatWorkingDuration(elapsedMs: number): string {
  const seconds = Math.max(0, Math.floor(elapsedMs / 1000))
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  if (minutes < 60) return `${minutes}m`
  return `${Math.floor(minutes / 60)}h ${minutes % 60}m`
}

function WorkingDuration({ startedAtMs }: { startedAtMs: number }) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1000)
    return () => window.clearInterval(timer)
  }, [])
  return <span className="tabular-nums">{formatWorkingDuration(now - startedAtMs)}</span>
}

const branchCache = new Map<string, string>()

/** Cache branches by workspace scope; discard replies from a prior scope or refresh. */
function useActivityBranch(session: Session | null | undefined, row: RefObject<HTMLDivElement>): string {
  const sessionId = session?.id
  const isolatedBranch = session?.workspaceBranchName
  const importedReadOnly = session?.importedReadOnly === true
  const scope = sessionId ? JSON.stringify([sessionId, session?.executionHostId ?? 'local', session?.workspaceDirectory ?? '', session?.workspaceWorktreeDirectory ?? '', session?.folderId ?? '']) : ''
  const refresh = (session?.interactionAt ?? session?.updatedAt)?.getTime() ?? 0
  const [branch, setBranch] = useState(() => ({ scope, name: branchCache.get(scope) || '' }))
  const [visible, setVisible] = useState(() => typeof IntersectionObserver === 'undefined')
  useEffect(() => {
    if (!sessionId || isolatedBranch || importedReadOnly || isCompanionContext() || typeof IntersectionObserver === 'undefined' || !row.current) return
    // Offscreen activity cards must not fill the serial VCS queue at startup.
    const observer = new IntersectionObserver((entries) => {
      setVisible(entries.some((entry) => entry.isIntersecting))
    }, { rootMargin: '100px' })
    observer.observe(row.current)
    return () => observer.disconnect()
  }, [sessionId, isolatedBranch, importedReadOnly, row])
  useEffect(() => {
    if (!visible || !sessionId || isolatedBranch || importedReadOnly || isCompanionContext()) return
    let cancelled = false
    void useAppStore.getState().getVcsCommitStatus(sessionId, 'git', { includeLineStats: false, contextOnly: true, requestId: `sidebar-branch:${scope}:${refresh}` }).then((status) => {
      if (cancelled) return
      const next = status?.available && status.activeVcsType !== 'svn' ? status.branchOrRevision || '' : ''
      branchCache.set(scope, next)
      setBranch({ scope, name: next })
    })
    return () => { cancelled = true }
  }, [scope, refresh, sessionId, isolatedBranch, importedReadOnly, visible])
  return importedReadOnly ? '' : isolatedBranch || (branch.scope === scope ? branch.name : '')
}

type ActivityStatus = { label: string; color: string; Icon: typeof CircleCheck }

/** T3-style at-a-glance label for a card that is not running. */
export function activityStatusLabel(status: DisplayedChatStatus): ActivityStatus | null {
  if (status?.type === 'attention') {
    if (status.kind === 'error') return { label: 'Failed', color: 'var(--error)', Icon: CircleAlert }
    if (status.kind === 'permission' || status.kind === 'command' || status.kind === 'file') return { label: 'Approval', color: 'var(--warning)', Icon: ShieldQuestion }
    return { label: 'Input', color: 'var(--accent)', Icon: MessageCircleQuestion }
  }
  if (status?.type === 'done') return { label: 'Done', color: 'var(--success)', Icon: CircleCheck }
  return null
}

export function formatActivityAge(date: Date | null | undefined, nowMs = Date.now()): string {
  if (!date || Number.isNaN(date.getTime())) return ''
  const minutes = Math.max(0, Math.floor((nowMs - date.getTime()) / 60000))
  if (minutes < 1) return 'now'
  if (minutes < 60) return `${minutes}m`
  const hours = Math.floor(minutes / 60)
  if (hours < 24) return `${hours}h`
  return `${Math.floor(hours / 24)}d`
}

function formatSidebarTime(date: Date | null): string {
  if (!date || Number.isNaN(date.getTime())) {
    return ''
  }

  const now = new Date()
  const isSameDay = date.toDateString() === now.toDateString()
  if (isSameDay) {
    return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  }

  const yesterday = new Date(now)
  yesterday.setDate(now.getDate() - 1)
  if (date.toDateString() === yesterday.toDateString()) {
    return 'Yesterday'
  }

  return date.toLocaleDateString([], {
    month: 'short',
    day: 'numeric',
  })
}

function formatSidebarTimeTitle(date: Date | null): string {
  if (!date || Number.isNaN(date.getTime())) {
    return 'Last opened time unavailable'
  }

  return `Last opened ${date.toLocaleString([], {
    month: 'short',
    day: 'numeric',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  })}`
}

function sidebarPathName(path: string): string {
  const segments = path.replace(/\\/g, '/').replace(/\/+$/, '').split('/').filter(Boolean)
  return segments[segments.length - 1] ?? ''
}

function workspacePathIdentity(path: string): string {
  const normalized = path.replace(/\\/g, '/').replace(/\/+$/, '')
  return /^[a-z]:/i.test(normalized) ? normalized.toLowerCase() : normalized
}

interface SessionItemProps {
  sessionId: string
  session?: Session
  familySessionIds?: string[]
  selected?: boolean
  activityLayout?: boolean
  /** Activity card context: workspace folder, its icon and the machine it runs on. */
  activityFolderName?: string
  activityFolderIcon?: CustomIconValue
  activityHostLabel?: string
  activityHostRemote?: boolean
  /** Reports the resolved selected chat to a phone-local navigation owner. */
  onSessionOpen?: (sessionId: string) => void
  onSessionClick?: (sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => boolean
  /** Verified native subagent child: same row, robot glyph instead of the provider/custom icon. */
  subagentRow?: boolean
}

const ATTENTION_LABELS: Record<AcpAttentionKind, string> = {
  question: 'Needs answer',
  plan: 'Plan needs review',
  memory: 'Memory input needed',
  permission: 'Permission needed',
  command: 'Command approval needed',
  file: 'File approval needed',
  error: 'Needs attention',
  generic: 'Input needed',
}

export function sidebarStatusIcon(kind: AcpAttentionKind, size = 12) {
  const props = { size, 'aria-hidden': true } as const
  switch (kind) {
    case 'question': return <HelpCircle {...props} />
    case 'plan': return <ClipboardList {...props} />
    case 'memory': return <Brain {...props} />
    case 'permission': return <ShieldCheck {...props} />
    case 'command': return <SquareChevronRight {...props} />
    case 'file': return <FileText {...props} />
    case 'error': return <TriangleAlert {...props} />
    default: return <CircleAlert {...props} />
  }
}

/** Wake time for a snoozed row: clock only today, weekday added for later days. */
function formatWakeTime(untilMs: number): string {
  const wake = new Date(untilMs)
  const time = wake.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
  return wake.toDateString() === new Date().toDateString() ? time : `${wake.toLocaleDateString([], { weekday: 'short' })} ${time}`
}

const HOVER_FADE = 'transition-opacity duration-100 group-hover:opacity-0 group-focus-within:opacity-0'

export const SessionItem = memo(function SessionItem({ sessionId, session, familySessionIds: providedFamilySessionIds, selected = false, activityLayout = false, activityFolderName, activityFolderIcon, activityHostLabel, activityHostRemote = false, onSessionClick, onSessionOpen, subagentRow = false }: SessionItemProps) {
  useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  // Verified native lineage, not only the catalogue's prop, decides the robot icon on every sidebar path.
  const lineageSubagent = useAppStore(state => { const candidate = session ?? state.sessions.find(item => item.id === sessionId); return candidate ? hasNativeSubagentLineage(candidate) : false })
  const isSubagentRow = subagentRow || lineageSubagent
  // Fine-grained selectors — each only re-renders when its specific value changes
  const sessionSummary = useAppStore(useShallow((s) => {
    if (session) {
      return {
        name: session.name,
        attentionRevision: session.attentionRevision,
        snoozedUntilMs: session.snoozedUntilMs ?? 0,
        lastOpenedAt: session.lastOpenedAt ?? session.updatedAt ?? null,
        isPinned: session.isPinned ?? false,
        providerId: session.providerId,
        customIcon: session.customIcon,
        folderId: session.folderId,
        workspaceDirectory: session.workspaceDirectory ?? '',
        worktreeDirectory: session.workspaceWorktreeDirectory ?? '',
        sourceDirectory: session.workspaceSourceDirectory ?? '',
        branchName: session.workspaceBranchName ?? '',
        executionHostId: session.executionHostId || 'local',
      }
    }

    const storeSession = s.sessions.find((x) => x.id === sessionId)
    return {
      name: storeSession?.name ?? '',
      attentionRevision: storeSession?.attentionRevision,
      snoozedUntilMs: storeSession?.snoozedUntilMs ?? 0,
      lastOpenedAt: storeSession?.lastOpenedAt ?? storeSession?.updatedAt ?? null,
      isPinned: storeSession?.isPinned ?? false,
      providerId: storeSession?.providerId,
      customIcon: storeSession?.customIcon,
      folderId: storeSession?.folderId ?? null,
      workspaceDirectory: storeSession?.workspaceDirectory ?? '',
      worktreeDirectory: storeSession?.workspaceWorktreeDirectory ?? '',
      sourceDirectory: storeSession?.workspaceSourceDirectory ?? '',
      branchName: storeSession?.workspaceBranchName ?? '',
      executionHostId: storeSession?.executionHostId || 'local',
    }
  }))
  const sourceFolderName = useAppStore((s) => sessionSummary.sourceDirectory
    ? s.folders.find((folder) => (folder.executionHostId || 'local') === sessionSummary.executionHostId
      && workspacePathIdentity(folder.directory) === workspacePathIdentity(sessionSummary.sourceDirectory))?.name ?? ''
    : '')
  const sourceWorkspaceName = sourceFolderName || sidebarPathName(sessionSummary.sourceDirectory) || activityFolderName || 'Unsorted'
  const worktreeName = sessionSummary.branchName.trim() || sidebarPathName(sessionSummary.worktreeDirectory)
  const sessionName = sessionSummary.name
  const sessionLastOpenedAt = sessionSummary.lastOpenedAt
  const isPinned = sessionSummary.isPinned
  const showProviderIcon = useAppStore((s) => s.showProviderIconsInSidebar)
  const showWorktreePath = useAppStore((s) => s.showWorktreePathInSidebar)
  const addSession = useAppStore((s) => s.addSession)
  const familySessionIds = useAppStore(useShallow((s) => providedFamilySessionIds ?? s.sessions
    .filter((candidate) => candidate.id === sessionId || (!subagentParentFor(candidate, s.sessions) && (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === sessionId))
    .map((candidate) => candidate.id)))
  const isActive = useAppStore((s) => familySessionIds.includes(s.activeSessionId ?? ''))
  // Subscribe to this row's selection result, so unrelated selection changes do not render every row.
  const [childSelected, isSelectedChat, hasNavigationRoute] = useAppStore(useShallow(state => {
    const activeId = state.activeSessionId
    if (activeId !== sessionId && !familySessionIds.includes(activeId ?? '')) {
      const candidate = session ?? state.sessions.find(candidate => candidate.id === sessionId)
      const parentId = candidate?.subagentParentChatId
      if (parentId !== activeId && (!candidate || !hasNativeSubagentLineage(candidate))) return [false, false, false] as const
    }
    const currentSession = session ?? state.sessions.find(candidate => candidate.id === sessionId)
    const navigationParent = currentSession && subagentParentFor(currentSession, state.sessions)
    const route = currentSession && subagentRouteFor(readChatGridLayout().activeLeafId, navigationParent || currentSession)
    const selectedChild = (navigationParent?.id === activeId || activeId === sessionId) && route?.kind === 'native' && route.chatId === sessionId
    return [selectedChild, selectedChild || (activeId === sessionId && !route), Boolean(route)] as const
  }))
  const lifecycleStatus = useAppStore(useShallow((s) => {
    const acpBindings = familySessionIds.flatMap((id) => s.acpBindingBySessionId[id] ? [s.acpBindingBySessionId[id]] : [])
    const cliBindings = familySessionIds.flatMap((id) => s.cliBindingBySessionId[id] ? [s.cliBindingBySessionId[id]] : [])
    return displayedChatStatus(cliBindings, acpBindings)
  }))
  const workingStartedAtMs = useAppStore((s) => {
    if (!activityLayout) return null
    const starts = familySessionIds.flatMap((id) => {
      const binding = s.acpBindingBySessionId[id]
      return binding?.processing && binding.processingStartedAtMs ? [binding.processingStartedAtMs] : []
    })
    return starts.length > 0 ? Math.min(...starts) : null
  })
  const setChatSettled = useAppStore((s) => s.setChatSettled)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const selectChat = () => {
    const current = useAppStore.getState()
    const child = current.sessions.find(candidate => candidate.id === sessionId)
    const parent = child && subagentParentFor(child, current.sessions)
    if (parent) { setActiveSession(sessionId); onSessionOpen?.(sessionId); return }
    returnToSubagentParent(sessionId)
    if (sessionSummary.attentionRevision) void useAppStore.getState().acknowledgeChatAttention(sessionId, sessionSummary.attentionRevision)
    const state = useAppStore.getState()
    const sessions = state.sessions
    const clickedSession = session ?? sessions.find((candidate) => candidate.id === sessionId)
    const branchRootId = clickedSession?.branchRootChatId || clickedSession?.parentChatId || clickedSession?.id
    if (clickedSession && branchRootId === sessionId) {
      const activeFamilyChatId = state.activeSessionId !== sessionId && familySessionIds.includes(state.activeSessionId ?? '')
        ? state.activeSessionId
        : null
      const rememberedChatId = preferredFamilyChat(branchRootId, familySessionIds)
      let preferredChatId = rememberedChatId ?? activeFamilyChatId
      if (activityLayout) {
        const familyActivity = familySessionIds.map((id) => ({ id, status: displayedChatStatus(
          [state.cliBindingBySessionId[id]], [state.acpBindingBySessionId[id]]
        ) }))
        const status = familyActivity.find((candidate) => candidate.status?.type === 'attention')?.status ??
          familyActivity.find((candidate) => candidate.status?.type === 'processing')?.status
        if (status) {
          const matchingIds = familyActivity.filter((candidate) => candidate.status?.type === status.type &&
            (status.type !== 'attention' || (candidate.status.type === 'attention' && candidate.status.kind === status.kind)) &&
            sessions.some((chat) => chat.id === candidate.id)).map((candidate) => candidate.id)
          // Open the branch supplying the family status without moving a conversation while it is being read.
          preferredChatId = [state.activeSessionId, rememberedChatId, ...matchingIds].find((id) => id && matchingIds.includes(id)) ?? preferredChatId
        }
      }
      const preferredChat = preferredChatId ? sessions.find((candidate) => candidate.id === preferredChatId) : undefined
      let deepestChat: Session | undefined = preferredChat
      if (!deepestChat) {
        const familyIds = familySessionIds.length > 0 ? new Set(familySessionIds) : undefined
        const family = new Map<string, Session>()
        for (const candidate of sessions) {
          if (familyIds ? familyIds.has(candidate.id) : (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === branchRootId) family.set(candidate.id, candidate)
        }
        const depths = new Map<string, number>([[branchRootId, 0]])
        const depth = (chat: Session): number => {
          const path: string[] = []
          const visited = new Set<string>()
          let current: Session | undefined = chat
          while (current && !depths.has(current.id) && !visited.has(current.id)) {
            visited.add(current.id)
            path.push(current.id)
            current = current.parentChatId ? family.get(current.parentChatId) : undefined
          }
          let value = current ? depths.get(current.id) ?? -1 : -1
          for (let index = path.length - 1; index >= 0; index--) {
            if (value >= 0) value++
            depths.set(path[index], value)
          }
          return depths.get(chat.id) ?? -1
        }
        const compareDepth = (a: Session, b: Session) =>
          depth(a) - depth(b) ||
          (a.messageCount ?? 0) - (b.messageCount ?? 0) ||
          (a.updatedAt?.getTime() ?? 0) - (b.updatedAt?.getTime() ?? 0) ||
          b.id.localeCompare(a.id)
        for (const candidate of family.values()) {
          if (!deepestChat || compareDepth(candidate, deepestChat) > 0) {
            deepestChat = candidate
          }
        }
      }
      const chatId = deepestChat?.id ?? sessionId
      activateExistingChatPane(chatId)
      setActiveSession(chatId)
      onSessionOpen?.(chatId)
      return
    }
    activateExistingChatPane(sessionId)
    setActiveSession(sessionId)
    onSessionOpen?.(sessionId)
  }
  const setSessionPinned = useAppStore((s) => s.setSessionPinned)
  const renameSession = useAppStore((s) => s.renameSession)
  const deleteSessions = useAppStore((s) => s.deleteSessions)

  const [editing, setEditing] = useState(false)
  const [editValue, setEditValue] = useState(sessionName)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  // Context/overflow menu anchored to a viewport position (cursor or button).
  const canReuseWorktree = useAppStore((state) => {
    const source = state.sessions.find((candidate) => candidate.id === sessionId)
    return Boolean(source && !source.importedReadOnly && source.workspaceIsolationKind === 'gitWorktree' && source.workspaceWorktreeDirectory && source.folderId && (source.executionHostId || 'local') === 'local')
  })
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null)
  // Resting row text (wake time, age, status) gives way whenever the hover actions show, so they never overlap.
  const restingFade = menuPos ? 'transition-opacity duration-100 opacity-0' : HOVER_FADE
  // Rows of the multi-selection the open menu acts on; empty while the menu targets only this row.
  const [menuSelectionIds, setMenuSelectionIds] = useState<string[]>([])
  const chatSelection = useContext(ChatSelectionContext)
  const selectionIds = () => {
    const ids = selected && chatSelection ? chatSelection.selectedIds() : []
    return ids.length > 1 ? ids : []
  }
  const openMenu = (point: { x: number; y: number }) => {
    setMenuSelectionIds(selectionIds())
    setMenuPos(point)
  }
  const [snoozePos, setSnoozePos] = useState<{ x: number; y: number } | null>(null)
  const snoozeRef = useRef<HTMLDivElement>(null)
  const setChatSnoozedUntil = useAppStore((s) => s.setChatSnoozedUntil)
  const snoozedUntilMs = sessionSummary.snoozedUntilMs
  const isSnoozed = snoozedUntilMs > Date.now()
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [deleting, setDeleting] = useState(false)
  const [deleteError, setDeleteError] = useState('')
  const iconPicker = useCustomIconPicker()
  const chatIcon = resolveCustomIcon(sessionSummary.customIcon)
  const nativeSessionId = useAppStore((s) => s.acpBindingBySessionId[sessionId]?.sessionId ?? '')
  const workspacePath = sessionSummary.worktreeDirectory || sessionSummary.workspaceDirectory
  const branchName = sessionSummary.branchName.trim()
  const copyItems: CopyDetail[] = [
    { label: 'Chat name', value: sessionName },
    { label: 'Chat ID', value: sessionId },
    ...(workspacePath ? [{ label: 'Workspace path', value: workspacePath }] : []),
    ...(branchName ? [{ label: 'Branch', value: branchName }] : []),
    ...(nativeSessionId ? [{ label: 'Native session ID', value: nativeSessionId }] : []),
  ]
  const [generatingTitle, setGeneratingTitle] = useState(false)
  const [cancellingTitle, setCancellingTitle] = useState(false)
  const [titleFeedback, setTitleFeedback] = useState<{ error: boolean; text: string } | null>(null)
  const titleRequestPending = useRef(false)
  const regenerateTitle = async () => {
    if (titleRequestPending.current) return
    if (!isCefContext()) { setTitleFeedback({ error: true, text: 'Title generation is available in the desktop app.' }); return }
    titleRequestPending.current = true
    setGeneratingTitle(true)
    setTitleFeedback(null)
    try {
      const response = await sendToCEF<{ chatId: string; title: string }>({ action: 'regenerateChatTitle', payload: { chatId: sessionId } }, false)
      if (!response.ok) throw new Error(response.error || 'Could not generate a title. Try again.')
      if (response.data?.chatId !== sessionId || typeof response.data.title !== 'string' || !response.data.title.trim() || new TextEncoder().encode(response.data.title).length > 240 || /[\x00-\x1f\x7f]/.test(response.data.title)) throw new Error('The title worker returned an invalid response.')
      // The native state push owns the rename, including concurrent-change checks.
      setTitleFeedback({ error: false, text: 'Chat title updated.' })
    } catch (error) {
      setTitleFeedback({ error: true, text: error instanceof Error ? error.message : 'Could not generate a title. Try again.' })
    } finally {
      titleRequestPending.current = false
      setGeneratingTitle(false)
      setCancellingTitle(false)
    }
  }
  const cancelTitle = async () => {
    if (cancellingTitle) return
    setCancellingTitle(true)
    try {
      const response = await sendToCEF({ action: 'cancelChatTitle', payload: { chatId: sessionId } }, false)
      if (!response.ok) throw new Error(response.error || 'Could not cancel title generation.')
    } catch (error) {
      setCancellingTitle(false)
      setTitleFeedback({ error: true, text: error instanceof Error ? error.message : 'Could not cancel title generation.' })
    }
  }
  const showMenu = menuPos !== null
  const inputRef = useRef<HTMLInputElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const rowRef = useRef<HTMLDivElement>(null)
  const menuReturnFocusRef = useRef<HTMLElement | null>(null)
  const activityDate = activityLayout ? session?.updatedAt ?? sessionLastOpenedAt : sessionLastOpenedAt
  const lastOpenedLabel = formatSidebarTime(activityDate)
  const activityBranch = useActivityBranch(activityLayout ? session : null, rowRef)
  const activityWorktree = activityLayout ? sessionSummary.worktreeDirectory.trim() : ''
  const activityStatus = activityLayout ? activityStatusLabel(lifecycleStatus) : null
  const lastOpenedTitle = formatSidebarTimeTitle(activityDate)
  const gridLeaves = chatGridLeaves(gridLayout.root)
  const paneIndexes = gridLeaves.flatMap((leaf, index) => familySessionIds.includes(leaf.sessionId) ? [index] : [])
  const paneNumbers = paneIndexes.map((index) => index + 1)
  const paneLabel = paneNumbers.length === 1
    ? `Shown in pane ${paneNumbers[0]}`
    : `Shown in panes ${paneNumbers.slice(0, -1).join(', ')} and ${paneNumbers[paneNumbers.length - 1]}`

  useEffect(() => {
    if (editing && inputRef.current) {
      inputRef.current.focus()
      inputRef.current.select()
    }
  }, [editing])

  // Rows consume only pane count and family indexes; other layout changes need no render.
  useLayoutEffect(() => {
    const refresh = (layout: ReturnType<typeof readChatGridLayout>) => setGridLayout((previous) => {
      const oldLeaves = chatGridLeaves(previous.root)
      const nextLeaves = chatGridLeaves(layout.root)
      const indexes = (leaves: typeof oldLeaves) => leaves.flatMap((leaf, index) => familySessionIds.includes(leaf.sessionId) ? [index] : [])
      return oldLeaves.length === nextLeaves.length && shallow(indexes(oldLeaves), indexes(nextLeaves)) ? previous : layout
    })
    refresh(readChatGridLayout())
    return subscribeChatGridLayout(refresh)
  }, [familySessionIds])

  // Close context menu on outside click
  useEffect(() => {
    if (!showMenu) return
    menuRef.current?.querySelector<HTMLButtonElement>('button:not([aria-disabled="true"])')?.focus()
    const handler = (e: MouseEvent) => {
      if (e.target instanceof Element && e.target.closest('[data-viewport-menu]')) return
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuPos(null)
      }
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      setMenuPos(null)
      menuReturnFocusRef.current?.focus()
    }
    document.addEventListener('mousedown', handler)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', handler)
      document.removeEventListener('keydown', onKey)
    }
  }, [showMenu])

  // Snooze popover closes on outside click or Escape, like the row menu.
  useEffect(() => {
    if (!snoozePos) return
    snoozeRef.current?.querySelector<HTMLButtonElement>('button')?.focus()
    const handler = (e: MouseEvent) => {
      if (e.target instanceof Element && e.target.closest('[data-viewport-menu]')) return
      if (snoozeRef.current && !snoozeRef.current.contains(e.target as Node)) setSnoozePos(null)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setSnoozePos(null) }
    document.addEventListener('mousedown', handler)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', handler)
      document.removeEventListener('keydown', onKey)
    }
  }, [snoozePos])

  const commitRename = () => {
    const trimmed = editValue.trim()
    if (trimmed && trimmed !== sessionName) renameSession(sessionId, trimmed)
    else setEditValue(sessionName)
    setEditing(false)
  }

  const confirmSessionDelete = async () => {
    if (deleting) return
    setDeleting(true)
    setDeleteError('')
    try {
      if (await deleteSessions(familySessionIds)) {
        setConfirmDelete(false)
        rowRef.current?.focus()
      } else {
        setDeleteError('The chat could not be deleted. Finish active work and try again.')
      }
    } catch {
      setDeleteError('The chat could not be deleted. Finish active work and try again.')
    } finally {
      setDeleting(false)
    }
  }

  return (
    <div
      className="relative group"
      draggable={!editing}
      onDragStart={(event) => {
        event.dataTransfer.effectAllowed = 'copy'
        event.dataTransfer.setData('text/x-uam-chat-id', sessionId)
        event.stopPropagation()
      }}
    >
      <div
        ref={rowRef}
        role="button"
        tabIndex={0}
        aria-current={isSelectedChat ? 'page' : undefined}
        aria-label={`Open chat ${sessionName}`}
        data-testid={`session-row-${sessionId}`}
        data-session-id={sessionId}
        data-selected={selected}
        className={`uam-sb-row relative flex ${activityLayout ? 'gap-2 px-2.5 py-1.5' : 'min-h-[26px] gap-1.5 px-2.5 py-1'} items-center rounded-md mx-1 cursor-pointer transition-all duration-100${selected || childSelected || (isActive && !hasNavigationRoute) ? ' uam-sb-row--selected' : ''}`}
        style={{
          // Running activity rows recede a touch so chats that need the user stand out.
          opacity: activityLayout && lifecycleStatus?.type === 'processing' && !isActive ? 0.95 : undefined,
          // Hover and selected backgrounds come from .uam-sb-row in sidebar.css so they share one box.
          boxShadow: selected ? 'inset 2px 0 0 var(--accent)' : 'none',
        }}
        onClick={(event) => {
          if (editing || onSessionClick?.(sessionId, event)) return
          selectChat()
        }}
        onDoubleClick={() => {
          if (isCompanionContext()) return
          setEditing(true)
          setEditValue(sessionName)
        }}
        onKeyDown={(event) => {
          if (event.target !== event.currentTarget || editing) return
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault()
            selectChat()
          } else if (!isCompanionContext() && (event.key === 'Delete' || (event.key === 'Backspace' && (event.metaKey || event.ctrlKey)))) {
            event.preventDefault()
            if (selectionIds().length > 0) { chatSelection?.deleteSelected(); return }
            setDeleteError('')
            setConfirmDelete(true)
          } else if (!isCompanionContext() && event.key === 'F2') {
            event.preventDefault()
            // Rename is single-chat only; it never silently renames one row of a multi-selection.
            if (selectionIds().length > 0) return
            setEditing(true)
            setEditValue(sessionName)
          } else if (!isCompanionContext() && (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10'))) {
            event.preventDefault()
            const rect = event.currentTarget.getBoundingClientRect()
            menuReturnFocusRef.current = event.currentTarget
            openMenu({ x: rect.left + 16, y: rect.bottom })
          }
        }}
        onContextMenu={(e) => {
          e.preventDefault()
          if (isCompanionContext()) return
          menuReturnFocusRef.current = e.currentTarget
          openMenu({ x: e.clientX, y: e.clientY })
        }}
      >
        {activityLayout && !editing ? (
          <div className="uam-sb-activity flex min-w-0 flex-1 flex-col gap-0.5">
            <div className="flex min-w-0 items-center gap-2 [&>img]:h-4">
              <CustomIcon icon={sessionSummary.customIcon} fallback={null} />
              <span className="block min-w-0 flex-1 truncate text-[13px] leading-5" style={{ color: isActive ? 'var(--text)' : 'var(--text-2)' }}>{sessionName}</span>
              {lifecycleStatus?.type === 'processing' ? (
                <span className="inline-flex shrink-0 items-center gap-1 text-[11px] font-medium tabular-nums" style={{ color: 'var(--info)' }}>
                  <CircleDashed size={12} className="uam-working-ring shrink-0" aria-hidden />
                  <span role="status">Working</span>
                  {workingStartedAtMs !== null && <span aria-hidden><WorkingDuration startedAtMs={workingStartedAtMs} /></span>}
                </span>
              ) : (
                <>
                  {/* Done overlays the status/time on hover so the row never reflows. */}
                  <span className="relative flex shrink-0 items-center justify-end">
                  {activityStatus ? (
                    <span className="inline-flex shrink-0 items-center gap-1 text-[11px] font-medium transition-opacity duration-100 group-hover:opacity-0 group-focus-within:opacity-0" style={{ color: activityStatus.color }}>
                      <activityStatus.Icon size={12} className="shrink-0" aria-hidden />
                      <span role="status">{activityStatus.label}</span>
                    </span>
                  ) : (
                    <span className="shrink-0 text-[11px] tabular-nums transition-opacity duration-100 group-hover:opacity-0 group-focus-within:opacity-0" style={{ color: 'var(--text-3)' }} title={`Updated ${activityDate?.toLocaleString() ?? ''}`}>{formatActivityAge(activityDate)}</span>
                  )}
                  {!isCompanionContext() && (
                    <span className="absolute right-0 top-1/2 -translate-y-1/2 inline-flex shrink-0 items-center gap-0.5 opacity-0 pointer-events-none transition-opacity duration-100 group-hover:opacity-100 group-hover:pointer-events-auto group-focus-within:opacity-100 group-focus-within:pointer-events-auto">
                      <button
                        type="button"
                        aria-label={`Mark ${sessionName} done`}
                        className="inline-flex shrink-0 items-center gap-1 rounded px-1 hover:bg-[var(--sidebar-item-hover)] focus-visible:outline focus-visible:outline-1 focus-visible:outline-[var(--accent)]"
                        style={{ color: 'var(--text-2)', background: 'transparent', border: 'none', cursor: 'pointer', fontSize: 11, lineHeight: 1.4 }}
                        onClick={(e) => {
                          e.stopPropagation()
                          if (sessionSummary.attentionRevision) void useAppStore.getState().acknowledgeChatAttention(sessionId, sessionSummary.attentionRevision)
                          void setChatSettled(sessionId, true)
                        }}
                        onDoubleClick={(e) => e.stopPropagation()}
                      >
                        <Check size={12} aria-hidden />
                        Done
                      </button>
                      <IconButton
                        size="sm"
                        label={`Snooze ${sessionName}`}
                        tooltip="Snooze"
                        icon={<Clock size={12} aria-hidden />}
                        aria-haspopup="menu"
                        aria-expanded={snoozePos !== null}
                        onClick={(e) => {
                          e.stopPropagation()
                          if (snoozePos) { setSnoozePos(null); return }
                          const r = e.currentTarget.getBoundingClientRect()
                          setSnoozePos({ x: r.right, y: r.bottom + 4 })
                        }}
                        onDoubleClick={(e) => e.stopPropagation()}
                      />
                    </span>
                  )}
                  </span>
                </>
              )}
            </div>
            <div className="flex min-w-0 items-center gap-1.5 text-[11px] leading-4" style={{ color: 'var(--text-3)' }}>
              <span className="flex min-w-0 flex-1 items-center gap-1" title={activityWorktree || undefined}>
                <CustomIcon icon={activityFolderIcon} fallback={<FolderIcon size={12} aria-hidden />} />
                <span className="truncate">{activityWorktree ? sourceWorkspaceName : activityFolderName || 'Unsorted'}</span>
                {activityWorktree && <><GitBranch size={11} className="shrink-0" aria-hidden /><span className="truncate">{worktreeName}</span></>}
                {!activityWorktree && activityBranch && <><GitBranch size={11} className="shrink-0" aria-hidden /><span className="truncate" title={activityBranch}>{activityBranch}</span></>}
              </span>
              <span role="img" aria-label={activityHostLabel || (SSH_ENABLED && activityHostRemote ? 'Remote host' : 'This computer')} title={activityHostLabel} className="inline-flex shrink-0">
                {activityHostRemote ? <Server size={12} aria-hidden /> : <Monitor size={12} aria-hidden />}
              </span>
              {sessionSummary.providerId && (
                <span role="img" aria-label={`Provider: ${providerShortName(undefined, sessionSummary.providerId)}`} title={providerShortName(undefined, sessionSummary.providerId)} className="inline-flex shrink-0">
                  <ProviderLogo providerId={sessionSummary.providerId} size={12} />
                </span>
              )}
            </div>
          </div>
        ) : (<>
        {(selected || (isCompanionContext() && isSelectedChat)) && (
          <span role="img" aria-label={selected ? 'Selected for bulk actions' : 'Selected chat'} className="inline-flex shrink-0" style={{ color: 'var(--accent)' }}>
            <Check size={12} aria-hidden />
          </span>
        )}
        {gridLeaves.length > 1 && paneIndexes.length > 0 && (
          <span role="img" aria-label={paneLabel} className="inline-flex shrink-0 items-center gap-0.5">
            {paneIndexes.map((paneIndex) => (
              <span
                key={paneIndex}
                data-testid="pane-indicator"
                className="h-1.5 w-1.5 rounded-full"
                style={{ background: chatPaneColors[paneIndex] }}
              />
            ))}
          </span>
        )}
        {!editing && isSubagentRow && (
          <span role="img" aria-label="Subagent" className="inline-flex shrink-0"><Bot size={16} aria-hidden /></span>
        )}
        {!editing && !isSubagentRow && (chatIcon || (showProviderIcon && sessionSummary.providerId)) && (
          // The chat's own icon replaces the provider glyph; the glyph stays the fallback.
          <span role="img" aria-label={chatIcon ? 'Chat icon' : `Provider: ${providerShortName(undefined, sessionSummary.providerId)}`} title={chatIcon ? undefined : providerShortName(undefined, sessionSummary.providerId)} className="inline-flex shrink-0">
            <CustomIcon icon={sessionSummary.customIcon} fallback={<ProviderLogo providerId={sessionSummary.providerId} size={16} />} />
          </span>
        )}
        {!editing && isPinned && (
          <button
            type="button"
            aria-label={`Unpin ${sessionName}`}
            title={`Unpin ${sessionName}`}
            draggable={false}
            className="inline-flex shrink-0 cursor-pointer rounded hover:bg-[var(--sidebar-item-hover)] focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1"
            style={{ color: 'var(--accent)' }}
            onPointerDown={(event) => event.stopPropagation()}
            onMouseDown={(event) => event.stopPropagation()}
            onDragStart={(event) => { event.preventDefault(); event.stopPropagation() }}
            onDoubleClick={(event) => event.stopPropagation()}
            onClick={(event) => { event.stopPropagation(); void setSessionPinned(sessionId, false) }}
          >
            <Pin size={12} fill="currentColor" aria-hidden />
          </button>
        )}

        {/* Name or edit input */}
        {editing ? (
          <input
            ref={inputRef}
            value={editValue}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={commitRename}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitRename()
              if (e.key === 'Escape') {
                setEditValue(sessionName)
                setEditing(false)
              }
            }}
            className="flex-1 bg-transparent text-xs outline-none min-w-0"
            style={{
              color: 'var(--text)',
              borderBottom: '1px solid var(--accent)',
              fontFamily: 'inherit',
            }}
            onClick={(e) => e.stopPropagation()}
          />
        ) : (
          <div className="min-w-0 flex-1">
            <span className="block truncate text-[13px]" style={{ color: isActive ? 'var(--text)' : 'var(--text-2)' }}>{sessionName}</span>
            {!activityLayout && showWorktreePath && sessionSummary.worktreeDirectory && (
              <span className="flex min-w-0 items-center gap-1 text-[10px]" title={sessionSummary.worktreeDirectory} style={{ color: 'var(--text-3)' }}>
                <span className="truncate">{sourceWorkspaceName}</span>
                <GitBranch size={11} className="shrink-0" aria-hidden />
                <span className="truncate">{worktreeName}</span>
              </span>
            )}
          </div>
        )}

        {!editing && (
          <>
            {!activityLayout && isSnoozed && (
              <span className={`inline-flex shrink-0 items-center gap-1 text-[10px] tabular-nums ${restingFade}`} style={{ color: 'var(--text-3)' }} title={`Wakes ${new Date(snoozedUntilMs).toLocaleString()}`}>
                <Clock size={11} aria-hidden />
                until {formatWakeTime(snoozedUntilMs)}
              </span>
            )}
            {!activityLayout && <div className={`ml-auto flex items-center gap-1 ${restingFade}`}>
              {lastOpenedLabel && (
                <span
                  className={`${activityLayout ? 'max-w-[100px]' : `max-w-[58px] ${HOVER_FADE}`} truncate text-[10px] tabular-nums`}
                  title={activityLayout ? `Updated ${activityDate?.toLocaleString() ?? ''}` : lastOpenedTitle}
                  style={{
                    color: isActive ? 'var(--text-2)' : 'var(--text-3)',
                    lineHeight: 1,
                  }}
                >
                  {lastOpenedLabel}
                </span>
              )}
              {!activityLayout && lifecycleStatus?.type === 'processing' && (
                <span className="session-status session-status--processing" aria-label="Agent running" title="Agent running">
                  <span />
                </span>
              )}
              {!activityLayout && lifecycleStatus?.type === 'attention' && (
                <span className={`session-status session-status--attention session-status--${lifecycleStatus.kind}`} aria-label={ATTENTION_LABELS[lifecycleStatus.kind]} title={ATTENTION_LABELS[lifecycleStatus.kind]}>
                  {sidebarStatusIcon(lifecycleStatus.kind)}
                </span>
              )}
              {!activityLayout && lifecycleStatus?.type === 'done' && (
                <span className="session-status session-status--idle" aria-label="Done" title="Done">
                  <span />
                </span>
              )}
            </div>}
            {!activityLayout && <div
              data-testid={`session-actions-${sessionId}`}
              style={isCompanionContext() ? { display: 'none' } : undefined}
              className={`absolute ${!activityLayout && lifecycleStatus ? 'right-[30px]' : 'right-2.5'} flex items-center gap-0.5 transition-opacity duration-100 ${
                menuPos
                  ? 'opacity-100 pointer-events-auto'
                  : 'opacity-0 pointer-events-none group-hover:opacity-100 group-hover:pointer-events-auto group-focus-within:opacity-100 group-focus-within:pointer-events-auto'
              }`}
            >
              {isSnoozed && (
                <IconButton
                  size="sm"
                  label={`Unsnooze ${sessionName}`}
                  tooltip={`Wake now · snoozed until ${formatWakeTime(snoozedUntilMs)}`}
                  icon={<AlarmClockOff size={13} aria-hidden />}
                  onClick={(e) => {
                    e.stopPropagation()
                    void setChatSnoozedUntil(sessionId, 0).then((result) => {
                      if (!result.ok) reportAppFailure(`Could not wake "${sessionName}".`, result.error || 'Wake request failed.')
                    })
                  }}
                  onDoubleClick={(e) => e.stopPropagation()}
                />
              )}
              <Tooltip label={isPinned ? 'Unpin chat' : 'Pin chat'} side="top">
                <button
                  type="button"
                  aria-label={isPinned ? 'Unpin chat' : 'Pin chat'}
                  className="flex flex-shrink-0 items-center justify-center rounded focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1"
                  style={{
                    width: 20,
                    height: 20,
                    background: 'var(--sidebar-item-hover)',
                    color: isPinned ? 'var(--accent)' : 'var(--text-3)',
                    border: 'none',
                    cursor: 'pointer',
                    padding: 0,
                    outlineColor: 'var(--accent)',
                  }}
                  onClick={(e) => {
                    e.stopPropagation()
                    void setSessionPinned(sessionId, !isPinned)
                  }}
                  onDoubleClick={(e) => e.stopPropagation()}
                >
                  <Pin size={13} fill={isPinned ? 'currentColor' : 'none'} aria-hidden />
                </button>
              </Tooltip>
              <Tooltip label="More actions" side="top">
                <button
                  type="button"
                  aria-label="More actions"
                  aria-haspopup="menu"
                  aria-expanded={showMenu}
                  className="flex flex-shrink-0 items-center justify-center rounded focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1"
                  style={{
                    width: 20,
                    height: 20,
                    background: 'var(--sidebar-item-hover)',
                    color: 'var(--text-3)',
                    border: 'none',
                    cursor: 'pointer',
                    outlineColor: 'var(--accent)',
                  }}
                  onClick={(e) => {
                    e.stopPropagation()
                    if (menuPos) { setMenuPos(null); return }
                    const r = e.currentTarget.getBoundingClientRect()
                    menuReturnFocusRef.current = e.currentTarget
                    openMenu({ x: r.right, y: r.bottom + 4 })
                  }}
                >
                  <MoreHorizontal size={14} aria-hidden />
                </button>
              </Tooltip>
            </div>}
          </>
        )}
        </>)}
      </div>

      {snoozePos && (
        <ViewportMenu
          ref={snoozeRef}
          point={snoozePos}
          role="menu"
          aria-label={`Snooze ${sessionName}`}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{ minWidth: 170, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
        >
          <SnoozeMenu chatId={sessionId} familyIds={familySessionIds} untilMs={snoozedUntilMs} />
        </ViewportMenu>
      )}

      {/* Context menu — anchored at the cursor / trigger position */}
      {menuPos && (
        <ViewportMenu
          ref={menuRef}
          point={menuPos}
          role="menu"
          aria-label={menuSelectionIds.length > 0 ? `Actions for ${menuSelectionIds.length} selected chats` : `Actions for ${sessionName}`}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            minWidth: 170,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
        >
          {menuSelectionIds.length > 0 ? (
            <ChatSelectionMenuItems
              ids={menuSelectionIds}
              onClose={() => setMenuPos(null)}
              onIcon={(targets) => iconPicker.open('chat', targets, `${targets.length} chats`)}
              onDelete={() => chatSelection?.deleteSelected()}
            />
          ) : <>
          <button type="button" role="menuitem" className="uam-sb-menu-item"
            disabled={!canReuseWorktree}
            onClick={() => {
              const current = useAppStore.getState()
              const source = current.sessions.find((candidate) => candidate.id === sessionId)
              if (!source || source.importedReadOnly || source.workspaceIsolationKind !== 'gitWorktree' || !source.workspaceWorktreeDirectory || !source.folderId || (source.executionHostId || 'local') !== 'local') return
              setMenuPos(null)
              current.setNewChatModalOpen(true, source.folderId, { sourceChatId: source.id, expectedWorkspaceDirectory: source.workspaceWorktreeDirectory, expectedExecutionHostId: source.executionHostId || 'local' })
            }}><MessageSquarePlus size={13} aria-hidden />New chat in this worktree</button>
          <MenuDivider />
          <button
            role="menuitem"
            className="uam-sb-menu-item"
            onClick={() => { setMenuPos(null); setEditing(true); setEditValue(sessionName) }}
          >
            <Pencil size={13} aria-hidden />
            Rename
            <kbd className="uam-menu-kbd">F2</kbd>
          </button>
          <button type="button" role="menuitem" disabled={generatingTitle}
            className="uam-sb-menu-item"
            onClick={() => { void regenerateTitle() }}>
            <RefreshCw size={13} aria-hidden />{generatingTitle ? 'Generating title…' : 'Regenerate title'}
          </button>
          {generatingTitle && <button type="button" role="menuitem" disabled={cancellingTitle}
            className="uam-sb-menu-item"
            onClick={() => { void cancelTitle() }}>{cancellingTitle ? 'Cancelling…' : 'Cancel title generation'}</button>}
          {titleFeedback && <p role={titleFeedback.error ? 'alert' : 'status'} className="px-3 py-1 text-xs" style={{ color: titleFeedback.error ? 'var(--error)' : 'var(--text-2)' }}>{titleFeedback.text}</p>}
          <button
            role="menuitem"
            className="uam-sb-menu-item"
            onClick={() => { setMenuPos(null); void setSessionPinned(sessionId, !isPinned) }}
          >
            <Pin size={13} fill={isPinned ? 'currentColor' : 'none'} aria-hidden />
            {isPinned ? 'Unpin chat' : 'Pin chat'}
          </button>
          <button type="button" role="menuitem" className="uam-sb-menu-item"
            onClick={() => { setMenuPos(null); iconPicker.open('chat', sessionId, sessionName) }}>
            <SmilePlus size={13} aria-hidden />Icon…
          </button>
          <MenuDivider />
          <RowSubmenu label="Copy" ariaLabel="Copy" icon={<Copy size={13} aria-hidden />}>
            <CopyDetailItems items={copyItems} />
          </RowSubmenu>
          <RowSubmenu label="Snooze" ariaLabel={`Snooze ${sessionName}`} icon={<Clock size={13} aria-hidden />}>
            <SnoozeMenu chatId={sessionId} familyIds={familySessionIds} untilMs={sessionSummary.snoozedUntilMs} onSnoozed={() => setMenuPos(null)} />
          </RowSubmenu>
          <CollectionMenuItems showDivider={false} type="chat" target={sessionId} label={sessionName} onAdded={() => setMenuPos(null)} />
          <MenuDivider />
          <button
            role="menuitem"
            className="uam-sb-menu-item uam-sb-menu-item--danger"
            onClick={() => { setMenuPos(null); setDeleteError(''); setConfirmDelete(true) }}
          >
            <Trash2 size={13} aria-hidden />
            Delete…
            <kbd className="uam-menu-kbd">Del</kbd>
          </button>
          </>}
        </ViewportMenu>
      )}
      {iconPicker.dialog}
      <ConfirmDialog
        open={confirmDelete}
        title="Delete chat?"
        label={`Delete ${sessionName}`}
        confirmLabel="Delete chat"
        busy={deleting}
        error={deleteError}
        onCancel={() => { setConfirmDelete(false); setDeleteError('') }}
        onConfirm={() => { void confirmSessionDelete() }}
      >
        {familySessionIds.length > 1
          ? `“${sessionName}” and its ${familySessionIds.length - 1} related branch${familySessionIds.length === 2 ? '' : 'es'} will be permanently deleted.`
          : `“${sessionName}” will be permanently deleted.`} This cannot be undone.
      </ConfirmDialog>
    </div>
  )
})

// One bulk title run at a time; each title is a provider request.
let bulkTitleRunning = false

/** Menu for a multi-selection: each action applies to every selected chat row; single-chat actions stay visible but unavailable. */
function ChatSelectionMenuItems({ ids, onClose, onIcon, onDelete }: { ids: string[]; onClose: () => void; onIcon: (targets: BulkTarget[]) => void; onDelete: () => void }) {
  const rows = useAppStore(useShallow((state) => ids.flatMap((id) => {
    const chat = state.sessions.find((candidate) => candidate.id === id)
    return chat ? [chat] : []
  })))
  const nativeIds = useAppStore(useShallow((state) => rows.map((chat) => state.acpBindingBySessionId[chat.id]?.sessionId ?? '')))
  const setSessionPinned = useAppStore((state) => state.setSessionPinned)
  const targets: BulkTarget[] = rows.map((chat) => ({ id: chat.id, label: chat.name }))
  const pin = !rows.every((chat) => chat.isPinned)
  const lines = (values: string[]) => values.filter(Boolean).join('\n')
  const copyItems: CopyDetail[] = [
    { label: 'Chat names', value: lines(rows.map((chat) => chat.name)) },
    { label: 'Chat IDs', value: lines(rows.map((chat) => chat.id)) },
    { label: 'Workspace paths', value: lines(rows.map((chat) => chat.workspaceWorktreeDirectory || chat.workspaceDirectory || '')) },
    { label: 'Branches', value: lines(rows.map((chat) => chat.workspaceBranchName?.trim() ?? '')) },
    { label: 'Native session IDs', value: lines(nativeIds) },
  ].filter((item) => item.value)
  const regenerateTitles = () => {
    if (bulkTitleRunning) return
    bulkTitleRunning = true
    onClose()
    // The native state push owns each rename, including concurrent-change checks.
    void runForEach(targets, async ({ id }) => {
      if (!isCefContext()) return 'Title generation is available in the desktop app.'
      const response = await sendToCEF({ action: 'regenerateChatTitle', payload: { chatId: id } }, false)
      return response.ok ? null : response.error || 'Could not generate a title. Try again.'
    }, 'Could not regenerate titles').finally(() => { bulkTitleRunning = false })
  }
  const togglePins = () => {
    onClose()
    void runForEach(targets.filter((_, index) => (rows[index].isPinned ?? false) !== pin),
      async ({ id }) => await setSessionPinned(id, pin) ? null : 'The pin could not be saved.', pin ? 'Could not pin chats' : 'Could not unpin chats')
  }
  return <>
    <SingleOnlyMenuItem noun="chat" icon={<MessageSquarePlus size={13} aria-hidden />} label="New chat in this worktree" />
    <MenuDivider />
    <SingleOnlyMenuItem noun="chat" icon={<Pencil size={13} aria-hidden />} label="Rename" />
    <button type="button" role="menuitem" className="uam-sb-menu-item" disabled={bulkTitleRunning} onClick={regenerateTitles}>
      <RefreshCw size={13} aria-hidden />{bulkTitleRunning ? 'Generating titles…' : `Regenerate ${targets.length} titles`}
    </button>
    <button type="button" role="menuitem" className="uam-sb-menu-item" onClick={togglePins}>
      <Pin size={13} fill={pin ? 'none' : 'currentColor'} aria-hidden />{pin ? 'Pin' : 'Unpin'} {targets.length} chats
    </button>
    <button type="button" role="menuitem" className="uam-sb-menu-item" onClick={() => { onClose(); onIcon(targets) }}>
      <SmilePlus size={13} aria-hidden />Icon…
    </button>
    <MenuDivider />
    <RowSubmenu label="Copy" ariaLabel="Copy" icon={<Copy size={13} aria-hidden />}>
      <CopyDetailItems items={copyItems} />
    </RowSubmenu>
    <RowSubmenu label="Snooze" ariaLabel={`Snooze ${targets.length} chats`} icon={<Clock size={13} aria-hidden />}>
      <SnoozeMenu chatId={ids[0]} familyIds={[]} targets={targets} onSnoozed={onClose} />
    </RowSubmenu>
    <CollectionMenuItems showDivider={false} type="chat" target={ids[0]} label={targets[0]?.label ?? ''} targets={targets} onAdded={onClose} />
    <MenuDivider />
    <button type="button" role="menuitem" className="uam-sb-menu-item uam-sb-menu-item--danger" onClick={() => { onClose(); onDelete() }}>
      <Trash2 size={13} aria-hidden />Delete {targets.length} chats…
      <kbd className="uam-menu-kbd">Del</kbd>
    </button>
  </>
}
