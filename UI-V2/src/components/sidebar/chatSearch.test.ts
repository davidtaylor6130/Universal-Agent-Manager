import { describe, expect, it } from 'vitest'
import type { Message } from '../../types/message'
import type { Folder, Session } from '../../types/session'
import {
  buildChatSearchIndex,
  buildChatSearchModel,
  tokenizeChatSearchQuery,
} from './chatSearch'

const now = new Date('2026-01-01T00:00:00.000Z')

function makeFolder(id: string, isExpanded = true): Folder {
  return {
    id,
    name: id,
    parentId: null,
    directory: `/tmp/${id}`,
    isExpanded,
    createdAt: now,
  }
}

function makeSession(id: string, name: string, folderId: string | null): Session {
  return {
    id,
    name,
    viewMode: 'cli',
    folderId,
    createdAt: now,
    updatedAt: now,
  }
}

function makeMessage(sessionId: string, content: string, isStreaming = false): Message {
  return {
    id: `${sessionId}-${content}`,
    sessionId,
    role: 'user',
    content,
    isStreaming,
    createdAt: now,
  }
}

function searchModel(
  query: string,
  folders: Folder[],
  sessions: Session[],
  messages: Record<string, Message[]> = {}
) {
  return buildChatSearchModel(
    folders,
    sessions,
    buildChatSearchIndex(sessions, messages),
    tokenizeChatSearchQuery(query)
  )
}

function visibleSessionIds(model: ReturnType<typeof searchModel>): string[] {
  return [
    ...model.folderRows.flatMap((row) => row.sessionIds),
    ...model.unfolderedSessionIds,
  ]
}

describe('chatSearch', () => {
  it('keeps all chats and current folder expansion state with an empty query', () => {
    const folders = [makeFolder('alpha', true), makeFolder('beta', false)]
    const sessions = [
      makeSession('s-alpha', 'Alpha Chat', 'alpha'),
      makeSession('s-beta', 'Beta Chat', 'beta'),
      makeSession('s-loose', 'Loose Chat', null),
    ]

    const model = searchModel('', folders, sessions)

    expect(model.isSearching).toBe(false)
    expect(model.folderRows.map((row) => ({
      folderId: row.folder.id,
      sessionIds: row.sessionIds,
      shouldShowSessions: row.shouldShowSessions,
    }))).toEqual([
      { folderId: 'alpha', sessionIds: ['s-alpha'], shouldShowSessions: true },
      { folderId: 'beta', sessionIds: ['s-beta'], shouldShowSessions: false },
    ])
    expect(model.unfolderedSessionIds).toEqual(['s-loose'])
  })

  it('matches chat titles case-insensitively', () => {
    const folders = [makeFolder('general'), makeFolder('work')]
    const sessions = [
      makeSession('s-gemini', 'Gemini Session', 'general'),
      makeSession('s-codex', 'Codex Session', 'work'),
    ]

    const model = searchModel('gEmInI', folders, sessions)

    expect(model.folderRows.map((row) => row.folder.id)).toEqual(['general'])
    expect(visibleSessionIds(model)).toEqual(['s-gemini'])
  })

  it('matches saved message content', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-one', 'Planning', 'general'),
      makeSession('s-two', 'Release', 'general'),
    ]
    const messages = {
      's-one': [makeMessage('s-one', 'Investigate persisted terminal output')],
      's-two': [makeMessage('s-two', 'Prepare the installer')],
    }

    const model = searchModel('persisted', folders, sessions, messages)

    expect(visibleSessionIds(model)).toEqual(['s-one'])
  })

  it('hides nonmatching folders and unfoldered chats while searching', () => {
    const folders = [makeFolder('general'), makeFolder('work')]
    const sessions = [
      makeSession('s-match', 'Needle Session', 'general'),
      makeSession('s-folder-miss', 'Other Folder', 'work'),
      makeSession('s-unfoldered-miss', 'Loose Chat', null),
    ]

    const model = searchModel('needle', folders, sessions)

    expect(model.folderRows.map((row) => row.folder.id)).toEqual(['general'])
    expect(model.unfolderedSessionIds).toEqual([])
    expect(visibleSessionIds(model)).toEqual(['s-match'])
  })

  it('reveals matching chats in collapsed folders while searching', () => {
    const folders = [makeFolder('collapsed', false)]
    const sessions = [makeSession('s-match', 'Collapsed Match', 'collapsed')]

    const model = searchModel('match', folders, sessions)

    expect(model.folderRows).toHaveLength(1)
    expect(model.folderRows[0].shouldShowSessions).toBe(true)
    expect(model.folderRows[0].sessionIds).toEqual(['s-match'])
  })

  it('requires every query token to match', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-match', 'Alpha Project', 'general'),
      makeSession('s-miss', 'Alpha Notes', 'general'),
    ]
    const messages = {
      's-match': [makeMessage('s-match', 'terminal restart steps')],
      's-miss': [makeMessage('s-miss', 'build checklist')],
    }

    expect(visibleSessionIds(searchModel('alpha terminal', folders, sessions, messages))).toEqual(['s-match'])
    expect(visibleSessionIds(searchModel('alpha missing', folders, sessions, messages))).toEqual([])
  })
})
