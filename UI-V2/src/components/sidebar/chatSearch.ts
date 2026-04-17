import type { Message } from '../../types/message'
import type { Folder, Session } from '../../types/session'

export type ChatSearchIndex = Record<string, string>

export interface ChatSearchFolderRow {
  folder: Folder
  sessionIds: string[]
  shouldShowSessions: boolean
}

export interface ChatSearchModel {
  isSearching: boolean
  folderRows: ChatSearchFolderRow[]
  unfolderedSessionIds: string[]
  hasMatches: boolean
}

function normalizeSearchText(value: string): string {
  return value.trim().toLowerCase()
}

export function tokenizeChatSearchQuery(query: string): string[] {
  const normalized = normalizeSearchText(query)
  return normalized.length === 0 ? [] : normalized.split(/\s+/)
}

export function buildChatSearchIndex(
  sessions: Session[],
  messages: Record<string, Message[]>
): ChatSearchIndex {
  return Object.fromEntries(
    sessions.map((session) => {
      const messageText = (messages[session.id] ?? [])
        .filter((message) => !message.isStreaming)
        .map((message) => message.content)
        .join(' ')

      return [session.id, normalizeSearchText(`${session.name} ${messageText}`)]
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

export function buildChatSearchModel(
  folders: Folder[],
  sessions: Session[],
  searchIndex: ChatSearchIndex,
  searchTokens: string[]
): ChatSearchModel {
  const isSearching = searchTokens.length > 0
  const rootFolders = folders.filter((folder) => folder.parentId === null)
  const matchingSessionIds = new Set(
    sessions
      .filter((session) => sessionMatchesChatSearch(searchIndex[session.id], searchTokens))
      .map((session) => session.id)
  )

  const sessionIdsByFolderId = new Map<string, string[]>()
  const unfolderedSessionIds: string[] = []

  for (const session of sessions) {
    if (isSearching && !matchingSessionIds.has(session.id)) {
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

  const folderRows = rootFolders
    .map((folder) => {
      const sessionIds = sessionIdsByFolderId.get(folder.id) ?? []
      return {
        folder,
        sessionIds,
        shouldShowSessions: isSearching || folder.isExpanded,
      } satisfies ChatSearchFolderRow
    })
    .filter((row) => !isSearching || row.sessionIds.length > 0)

  const hasMatches =
    folderRows.some((row) => row.sessionIds.length > 0) ||
    unfolderedSessionIds.length > 0

  return {
    isSearching,
    folderRows,
    unfolderedSessionIds,
    hasMatches,
  }
}
