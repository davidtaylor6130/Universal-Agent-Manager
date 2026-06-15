import type { Folder, Session } from '../../types/session'
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
  attentionKind?: string | null
  lastError?: string
}

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
  folderRows: ChatSearchFolderRow[]
  unfolderedSessionIds: string[]
  hasMatches: boolean
}

export interface ChatSearchSessionGroups {
  isSearching: boolean
  sessionIdsByFolderId: Map<string, string[]>
  pinnedSessionIds: string[]
  unfolderedSessionIds: string[]
}

const CLI_RUNNING_LIFECYCLE_STATES = new Set(['busy', 'shuttingDown'])
const ACP_RUNNING_LIFECYCLE_STATES = new Set(['starting', 'processing'])
const ACP_ATTENTION_LIFECYCLE_STATES = new Set(['waitingPermission', 'waitingUserInput', 'error'])

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

function sessionHasAttention(acpBinding: ChatSearchAcpBinding | undefined): boolean {
  return Boolean(
    acpBinding?.attentionKind ||
    ACP_ATTENTION_LIFECYCLE_STATES.has(acpBinding?.lifecycleState ?? '') ||
    acpBinding?.lastError
  )
}

function sessionIsRunning(cliBinding: ChatSearchCliBinding | undefined, acpBinding: ChatSearchAcpBinding | undefined): boolean {
  return Boolean(
    cliBinding?.running ||
    cliBinding?.processing ||
    CLI_RUNNING_LIFECYCLE_STATES.has(cliBinding?.lifecycleState ?? '') ||
    acpBinding?.running ||
    acpBinding?.processing ||
    ACP_RUNNING_LIFECYCLE_STATES.has(acpBinding?.lifecycleState ?? '')
  )
}

function sessionIsDone(cliBinding: ChatSearchCliBinding | undefined, acpBinding: ChatSearchAcpBinding | undefined): boolean {
  return Boolean(cliBinding?.readySinceLastSelect || acpBinding?.readySinceLastSelect)
}

function sessionMatchesStatusFilter(
  session: Session,
  statusId: ChatStatusFilterId,
  context: ChatSearchFilterContext
): boolean {
  const cliBinding = context.cliBindingBySessionId?.[session.id]
  const acpBinding = context.acpBindingBySessionId?.[session.id]
  const hasAttention = sessionHasAttention(acpBinding)
  const isRunning = sessionIsRunning(cliBinding, acpBinding)
  const isDone = sessionIsDone(cliBinding, acpBinding)

  if (statusId === 'pinned') {
    return Boolean(session.isPinned)
  }

  if (statusId === 'running') {
    return isRunning
  }

  if (statusId === 'attention') {
    return hasAttention
  }

  if (statusId === 'done') {
    return isDone
  }

  return !session.isPinned && !hasAttention && !isRunning && !isDone
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
  const lastOpenedAt = session.lastOpenedAt?.getTime()
  if (typeof lastOpenedAt === 'number' && Number.isFinite(lastOpenedAt)) {
    return lastOpenedAt
  }

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

  const updatedDelta = b.updatedAt.getTime() - a.updatedAt.getTime()
  if (updatedDelta !== 0) {
    return updatedDelta
  }

  return b.createdAt.getTime() - a.createdAt.getTime()
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
  const sortedSessions = [...sessions].sort(compareSessionsByRecent)
  const matchingSessionIds = new Set(
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
      .map((session) => session.id)
  )

  const sessionIdsByFolderId = new Map<string, string[]>()
  const pinnedSessionIds: string[] = []
  const unfolderedSessionIds: string[] = []

  for (const session of sortedSessions) {
    if (isSearching && !matchingSessionIds.has(session.id)) {
      continue
    }

    if (session.isPinned) {
      pinnedSessionIds.push(session.id)
      continue
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
    folderRows.some((row) => row.sessionIds.length > 0) ||
    unfolderedSessionIds.length > 0

  return {
    isSearching: groups.isSearching,
    pinnedSessionIds: groups.pinnedSessionIds,
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
