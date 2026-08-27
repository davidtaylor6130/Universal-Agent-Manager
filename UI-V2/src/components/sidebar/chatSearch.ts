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
    return false
  }

  const providerMatch =
    filters?.providerIds.some((providerId) => (session.providerId || DEFAULT_PROVIDER_ID) === providerId) ?? false
  const statusMatch =
    filters?.statusIds.some((statusId) => sessionMatchesStatusFilter(session, statusId, context)) ?? false

  return providerMatch || statusMatch
}

function sessionRecentTime(session: Session): number {
  // Sort by last interaction (updatedAt, bumped by C++ on every message append),
  // NOT by selection time. lastOpenedAt is deliberately ignored: selecting a chat
  // must not reorder the sidebar — chats move only on real activity. See issue #49.
  const updatedAt = session.updatedAt.getTime()
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
  const sortedSessions = [...sessions].sort((a, b) =>
    (familyActivity.get(branchRootId(b)) ?? 0) - (familyActivity.get(branchRootId(a)) ?? 0) || compareSessionsByRecent(a, b)
  )
  const matchingBranchRootIds = new Set(
    sortedSessions
      .filter((session) => {
        const searchMatch = isTextSearching
          ? deepSearchSessionIds
            ? deepSearchSessionIds.has(session.id)
            : sessionMatchesChatSearch(searchIndex[session.id], searchTokens)
          : false
        const filterMatch = sessionMatchesChatSearchFilters(session, filters, filterContext)
        return isSearching ? searchMatch || filterMatch : true
      })
      .map(branchRootId)
  )

  const sessionIdsByFolderId = new Map<string, string[]>()
  const pinnedSessionIds: string[] = []
  const activeSessionIds: string[] = []
  const unfolderedSessionIds: string[] = []
  const activeRootIds = new Set(sortedSessions.flatMap((candidate) => {
    const cli = filterContext.cliBindingBySessionId?.[candidate.id]
    const acp = filterContext.acpBindingBySessionId?.[candidate.id]
    return displayedChatStatus([cli], [acp]) ? [branchRootId(candidate)] : []
  }))

  for (const session of sortedSessions) {
    const rootId = branchRootId(session)
    if (session.id !== rootId || (isSearching && !matchingBranchRootIds.has(rootId))) {
      continue
    }

    if (activeRootIds.has(rootId)) {
      activeSessionIds.push(session.id)
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
