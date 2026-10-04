import type { Folder, Session } from '../../types/session'
import type { AcpAttentionKind } from '../../store/useAppStore'
import { DEFAULT_PROVIDER_ID } from '../../utils/providerMetadata'

export type ChatSearchIndex = Record<string, string>
export type ChatStatusFilterId = 'pinned' | 'running' | 'attention' | 'done' | 'idle'

export interface ChatSearchFilters {
  providerIds: string[]
  statusIds: ChatStatusFilterId[]
}

interface ChatSearchCliBinding {
  running?: boolean
  processing?: boolean
  lifecycleState?: string
  readySinceLastSelect?: boolean
}

interface ChatSearchAcpBinding {
  running?: boolean
  processing?: boolean
  lifecycleState?: string
  readySinceLastSelect?: boolean
  attentionKind?: AcpAttentionKind | null
  lastError?: string
}

export type DisplayedChatStatus =
  | { type: 'attention'; kind: AcpAttentionKind }
  | { type: 'processing' }
  | { type: 'done' }
  | null

export interface ChatSearchFilterContext {
  cliBindingBySessionId?: Record<string, ChatSearchCliBinding>
  acpBindingBySessionId?: Record<string, ChatSearchAcpBinding>
}

export interface ChatSearchFolderRow {
  folder: Folder
  sessionIds: string[]
  shouldShowSessions: boolean
}

export interface ChatSearchModel {
  isSearching: boolean
  pinnedSessionIds: string[]
  /** Running and open chats in one list, newest activity first. */
  activeSessionIds: string[]
  folderRows: ChatSearchFolderRow[]
  unfolderedSessionIds: string[]
  hasMatches: boolean
}

export interface ChatSearchSessionGroups {
  isSearching: boolean
  sessionIdsByFolderId: Map<string, string[]>
  pinnedSessionIds: string[]
  activeSessionIds: string[]
  unfolderedSessionIds: string[]
}

// ponytail: fixed window; promote to a setting if people want longer inboxes.
export const ACTIVE_CHAT_AUTO_DONE_MS = 3 * 24 * 60 * 60 * 1000

/** Unsettled = touched since the user last marked it done, and recently enough to still matter. */
export function isUnsettledChat(updatedAtMs: number, settledAt: string | undefined, nowMs = Date.now()): boolean {
  if (updatedAtMs < nowMs - ACTIVE_CHAT_AUTO_DONE_MS) return false
  const settledMs = settledAt ? Date.parse(settledAt) : NaN
  return Number.isNaN(settledMs) || updatedAtMs > settledMs
}

function mergeDisplayedStatus(a: DisplayedChatStatus, b: DisplayedChatStatus): DisplayedChatStatus {
  const rank = (status: DisplayedChatStatus) => status?.type === 'attention' ? 3 : status?.type === 'processing' ? 2 : status ? 1 : 0
  return rank(b) > rank(a) ? b : a
}

function hasActiveChatSearchFilters(filters?: ChatSearchFilters): boolean {
  return Boolean(filters && (filters.providerIds.length > 0 || filters.statusIds.length > 0))
}

function normalizeSearchText(value: string): string {
  return value.trim().toLowerCase()
}

export function tokenizeChatSearchQuery(query: string): string[] {
  const normalized = normalizeSearchText(query)
  return normalized.length === 0 ? [] : normalized.split(/\s+/)
}

export function buildChatSearchIndex(
  sessions: Session[]
): ChatSearchIndex {
  return Object.fromEntries(
    sessions.map((session) => {
      return [session.id, normalizeSearchText(`${session.name} ${session.providerId ?? ''} ${session.workspaceDirectory ?? ''}`)]
    })
  )
}

export function sessionMatchesChatSearch(
  indexedText: string | undefined,
  searchTokens: string[]
): boolean {
  if (searchTokens.length === 0) {
    return true
  }

  if (!indexedText) {
    return false
  }

  return searchTokens.every((token) => indexedText.includes(token))
}

export function displayedChatStatus(
  cliBindings: readonly (ChatSearchCliBinding | undefined)[],
  acpBindings: readonly (ChatSearchAcpBinding | undefined)[]
): DisplayedChatStatus {
  const attention = acpBindings.find((binding) => binding?.attentionKind && binding.attentionKind !== 'error')?.attentionKind
  if (attention) return { type: 'attention', kind: attention }
  if (
    acpBindings.some((binding) => binding?.processing || binding?.lifecycleState === 'waitingPermission') ||
    cliBindings.some((binding) => binding?.processing || binding?.lifecycleState === 'busy' || binding?.lifecycleState === 'shuttingDown')
  ) return { type: 'processing' }
  if (acpBindings.some((binding) => binding?.readySinceLastSelect) || cliBindings.some((binding) => binding?.readySinceLastSelect)) {
    return { type: 'done' }
  }
  return null
}

function sessionMatchesStatusFilter(
  session: Session,
  statusId: ChatStatusFilterId,
  context: ChatSearchFilterContext
): boolean {
  const cliBinding = context.cliBindingBySessionId?.[session.id]
  const acpBinding = context.acpBindingBySessionId?.[session.id]
  const status = displayedChatStatus([cliBinding], [acpBinding])

  if (statusId === 'pinned') {
    return Boolean(session.isPinned)
  }

  if (statusId === 'running') {
    return status?.type === 'processing'
  }

  if (statusId === 'attention') {
    return status?.type === 'attention'
  }

  if (statusId === 'done') {
    return status?.type === 'done'
  }

  return !session.isPinned && status === null
}

export function sessionMatchesChatSearchFilters(
  session: Session,
  filters: ChatSearchFilters | undefined,
  context: ChatSearchFilterContext = {}
): boolean {
  if (!hasActiveChatSearchFilters(filters)) {
    return true
  }

  const providerMatch = !filters?.providerIds.length ||
    filters.providerIds.some((providerId) => (session.providerId || DEFAULT_PROVIDER_ID) === providerId)
  const statusMatch = !filters?.statusIds.length ||
    filters.statusIds.some((statusId) => sessionMatchesStatusFilter(session, statusId, context))

  return providerMatch && statusMatch
}

function sessionRecentTime(session: Session): number {
  // Streaming and selection do not change interaction recency. Older chats retain
  // their saved recency until their first user input or runtime state event.
  const updatedAt = (session.interactionAt ?? session.updatedAt).getTime()
  if (Number.isFinite(updatedAt)) {
    return updatedAt
  }

  const createdAt = session.createdAt.getTime()
  return Number.isFinite(createdAt) ? createdAt : 0
}

export function compareSessionsByRecent(a: Session, b: Session): number {
  const recentDelta = sessionRecentTime(b) - sessionRecentTime(a)
  if (recentDelta !== 0) {
    return recentDelta
  }

  const createdDelta = b.createdAt.getTime() - a.createdAt.getTime()
  if (createdDelta !== 0) {
    return createdDelta
  }

  // Final tiebreak on a stable id so equal-timestamp chats keep a fixed order,
  // independent of the backend's chatOrder — selecting a chat can't shuffle them.
  return a.id.localeCompare(b.id)
}

export function buildChatSearchSessionGroups(
  sessions: Session[],
  searchIndex: ChatSearchIndex,
  searchTokens: string[],
  deepSearchSessionIds?: Set<string>,
  filters?: ChatSearchFilters,
  filterContext: ChatSearchFilterContext = {}
): ChatSearchSessionGroups {
  const isTextSearching = searchTokens.length > 0
  const isFiltering = hasActiveChatSearchFilters(filters)
  const isSearching = isTextSearching || isFiltering
  const branchRootId = (session: Session) => session.branchRootChatId || session.parentChatId || session.id
  const familyActivity = new Map<string, number>()
  for (const session of sessions) {
    const rootId = branchRootId(session)
    familyActivity.set(rootId, Math.max(familyActivity.get(rootId) ?? 0, sessionRecentTime(session)))
  }
  const sortedSessions = sessions.filter((session) => !session.temporaryParentChatId).sort((a, b) =>
    (familyActivity.get(branchRootId(b)) ?? 0) - (familyActivity.get(branchRootId(a)) ?? 0) || compareSessionsByRecent(a, b)
  )
  const matchingSessionIds = new Set(
    sortedSessions
      .filter((session) => {
        const searchMatch = isTextSearching
          ? deepSearchSessionIds
            ? deepSearchSessionIds.has(session.id)
            : sessionMatchesChatSearch(searchIndex[session.id], searchTokens)
          : true
        const filterMatch = sessionMatchesChatSearchFilters(session, filters, filterContext)
        return searchMatch && filterMatch
      })
      .map((session) => session.id)
  )

  const sessionIdsByFolderId = new Map<string, string[]>()
  const pinnedSessionIds: string[] = []
  const activeSessionIds: string[] = []
  const unfolderedSessionIds: string[] = []
  // Sections follow structured runtimes only: a CLI terminal looks busy as soon as it
  // launches, which would park idle CLI chats in Working. CLI chats still surface
  // through recent activity.
  const sessionStatus = (session: Session) => displayedChatStatus([], [filterContext.acpBindingBySessionId?.[session.id]])
  const familyStatus = new Map<string, DisplayedChatStatus>()
  const familyUpdatedMs = new Map<string, number>()
  for (const candidate of sortedSessions) {
    const rootId = branchRootId(candidate)
    familyStatus.set(rootId, mergeDisplayedStatus(familyStatus.get(rootId) ?? null, sessionStatus(candidate)))
    familyUpdatedMs.set(rootId, Math.max(familyUpdatedMs.get(rootId) ?? 0, sessionRecentTime(candidate)))
  }
  const nowMs = Date.now()
  const returnedAtMs = new Map<string, number>()

  for (const session of sortedSessions) {
    const rootId = branchRootId(session)
    if ((!isSearching && session.id !== rootId) || (isSearching && !matchingSessionIds.has(session.id))) {
      continue
    }

    const status = isSearching ? sessionStatus(session) : familyStatus.get(rootId) ?? null
    const updatedMs = isSearching ? sessionRecentTime(session) : familyUpdatedMs.get(rootId) ?? 0
    // Marking done clears a stale "ready" too; only running turns and questions outrank it.
    const unsettled = isUnsettledChat(updatedMs, session.settledAt, nowMs)
    if (status?.type === 'processing' || status?.type === 'attention' || unsettled || (status?.type === 'done' && !session.settledAt)) {
      activeSessionIds.push(session.id)
      returnedAtMs.set(session.id, updatedMs)
    }

    if (session.isPinned) {
      pinnedSessionIds.push(session.id)
    }

    if (session.folderId === null) {
      unfolderedSessionIds.push(session.id)
      continue
    }

    const sessionIds = sessionIdsByFolderId.get(session.folderId) ?? []
    sessionIds.push(session.id)
    sessionIdsByFolderId.set(session.folderId, sessionIds)
  }

  // One history: most recent activity on top, running chats included.
  activeSessionIds.sort((a, b) => (returnedAtMs.get(b) ?? 0) - (returnedAtMs.get(a) ?? 0))

  return {
    isSearching,
    sessionIdsByFolderId,
    pinnedSessionIds,
    activeSessionIds,
    unfolderedSessionIds,
  }
}

export function buildChatSearchModelFromGroups(
  folders: Folder[],
  groups: ChatSearchSessionGroups
): ChatSearchModel {
  const rootFolders = folders.filter((folder) => folder.parentId === null)
  const rootFolderIds = new Set(rootFolders.map((folder) => folder.id))
  const unfolderedSessionIds = [
    ...groups.unfolderedSessionIds,
    ...Array.from(groups.sessionIdsByFolderId.entries())
      .filter(([folderId]) => !rootFolderIds.has(folderId))
      .flatMap(([, sessionIds]) => sessionIds),
  ]

  const folderRows = rootFolders
    .map((folder) => {
      const sessionIds = groups.sessionIdsByFolderId.get(folder.id) ?? []
      return {
        folder,
        sessionIds,
        shouldShowSessions: groups.isSearching || folder.isExpanded,
      } satisfies ChatSearchFolderRow
    })
    .filter((row) => !groups.isSearching || row.sessionIds.length > 0)

  const hasMatches =
    groups.pinnedSessionIds.length > 0 ||
    groups.activeSessionIds.length > 0 ||
    folderRows.some((row) => row.sessionIds.length > 0) ||
    unfolderedSessionIds.length > 0

  return {
    isSearching: groups.isSearching,
    pinnedSessionIds: groups.pinnedSessionIds,
    activeSessionIds: groups.activeSessionIds,
    folderRows,
    unfolderedSessionIds,
    hasMatches,
  }
}

export function buildChatSearchModel(
  folders: Folder[],
  sessions: Session[],
  searchIndex: ChatSearchIndex,
  searchTokens: string[],
  deepSearchSessionIds?: Set<string>,
  filters?: ChatSearchFilters,
  filterContext: ChatSearchFilterContext = {}
): ChatSearchModel {
  return buildChatSearchModelFromGroups(
    folders,
    buildChatSearchSessionGroups(sessions, searchIndex, searchTokens, deepSearchSessionIds, filters, filterContext)
  )
}
