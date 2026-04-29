import type { Folder, Session } from '../../types/session'

export type ChatSearchIndex = Record<string, string>

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
  deepSearchSessionIds?: Set<string>
): ChatSearchSessionGroups {
  const isSearching = searchTokens.length > 0
  const sortedSessions = [...sessions].sort(compareSessionsByRecent)
  const matchingSessionIds = new Set(
    sortedSessions
      .filter((session) => deepSearchSessionIds ? deepSearchSessionIds.has(session.id) : sessionMatchesChatSearch(searchIndex[session.id], searchTokens))
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
  deepSearchSessionIds?: Set<string>
): ChatSearchModel {
  return buildChatSearchModelFromGroups(
    folders,
    buildChatSearchSessionGroups(sessions, searchIndex, searchTokens, deepSearchSessionIds)
  )
}
