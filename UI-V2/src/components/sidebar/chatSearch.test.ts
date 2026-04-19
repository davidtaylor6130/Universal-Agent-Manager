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

function makeSession(
  id: string,
  name: string,
  folderId: string | null,
  lastOpenedAt = now,
  updatedAt = now,
  isPinned = false
): Session {
  return {
    id,
    name,
    viewMode: 'cli',
    folderId,
    isPinned,
    createdAt: now,
    updatedAt,
    lastOpenedAt,
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
    ...model.pinnedSessionIds,
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

  it('orders folder chats by most recent opened time', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-old', 'Old Chat', 'general', new Date('2026-01-01T09:00:00.000Z')),
      makeSession('s-new', 'New Chat', 'general', new Date('2026-01-01T11:00:00.000Z')),
      makeSession('s-middle', 'Middle Chat', 'general', new Date('2026-01-01T10:00:00.000Z')),
    ]

    const model = searchModel('', folders, sessions)

    expect(model.folderRows[0].sessionIds).toEqual(['s-new', 's-middle', 's-old'])
  })

  it('lifts pinned chats into the top section without duplicating them in folders', () => {
    const folders = [makeFolder('general'), makeFolder('work')]
    const sessions = [
      makeSession('s-pinned', 'Pinned Chat', 'general', now, now, true),
      makeSession('s-folder', 'Folder Chat', 'general'),
      makeSession('s-work', 'Work Chat', 'work'),
    ]

    const model = searchModel('', folders, sessions)

    expect(model.pinnedSessionIds).toEqual(['s-pinned'])
    expect(model.folderRows.map((row) => ({
      folderId: row.folder.id,
      sessionIds: row.sessionIds,
    }))).toEqual([
      { folderId: 'general', sessionIds: ['s-folder'] },
      { folderId: 'work', sessionIds: ['s-work'] },
    ])
  })

  it('searches pinned chats and hides the pinned section when none match', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-pinned-match', 'Pinned Needle', 'general', now, now, true),
      makeSession('s-pinned-miss', 'Pinned Other', 'general', now, now, true),
      makeSession('s-folder-match', 'Folder Needle', 'general'),
    ]

    const matchModel = searchModel('needle', folders, sessions)
    expect(matchModel.pinnedSessionIds).toEqual(['s-pinned-match'])
    expect(matchModel.folderRows[0].sessionIds).toEqual(['s-folder-match'])

    const folderOnlyModel = searchModel('folder', folders, sessions)
    expect(folderOnlyModel.pinnedSessionIds).toEqual([])
    expect(folderOnlyModel.folderRows[0].sessionIds).toEqual(['s-folder-match'])
  })

  it('keeps chats with missing folders in unsorted instead of dropping them', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-valid', 'Valid Chat', 'general'),
      makeSession('s-missing-folder', 'Missing Folder Chat', 'deleted-folder'),
    ]

    const model = searchModel('', folders, sessions)

    expect(model.folderRows[0].sessionIds).toEqual(['s-valid'])
    expect(model.unfolderedSessionIds).toEqual(['s-missing-folder'])
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
