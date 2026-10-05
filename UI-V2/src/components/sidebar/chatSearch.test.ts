import { describe, expect, it } from 'vitest'
import type { Folder, Session } from '../../types/session'
import {
  type ChatSearchFilters,
  buildChatSearchIndex,
  buildChatSearchModel,
  tokenizeChatSearchQuery,
  isUnsettledChat,
  ACTIVE_CHAT_AUTO_DONE_MS,
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
  isPinned = false,
  providerId?: string
): Session {
  return {
    id,
    name,
    viewMode: 'cli',
    folderId,
    isPinned,
    providerId,
    createdAt: now,
    updatedAt,
    lastOpenedAt,
  }
}

function searchModel(
  query: string,
  folders: Folder[],
  sessions: Session[],
  filters?: ChatSearchFilters,
  filterContext = {}
) {
  return buildChatSearchModel(
    folders,
    sessions,
    buildChatSearchIndex(sessions),
    tokenizeChatSearchQuery(query),
    undefined,
    filters,
    filterContext
  )
}

function visibleSessionIds(model: ReturnType<typeof searchModel>): string[] {
  return [...new Set([
    ...model.pinnedSessionIds,
    ...model.activeSessionIds,
    ...model.folderRows.flatMap((row) => row.sessionIds),
    ...model.unfolderedSessionIds,
  ])]
}

  it('excludes temporary side chats from every sidebar group and search result', () => {
    const folders = [makeFolder('general')]
    const sessions = [makeSession('main', 'Main', 'general', now, now, false),
      { ...makeSession('side', 'Side question', 'general', now, now, true), temporaryParentChatId: 'main' }]
    expect(visibleSessionIds(searchModel('', folders, sessions))).not.toContain('side')
    expect(visibleSessionIds(searchModel('Side', folders, sessions))).not.toContain('side')
  })

  it('duplicates every displayed status and pinned chat without removing it from all chats', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('pinned', 'Pinned', 'general', now, now, true),
      makeSession('running', 'Running', 'general'),
      makeSession('done', 'Done', 'general'),
      makeSession('idle', 'Idle', 'general'),
    ]
    const model = searchModel('', folders, sessions, undefined, { acpBindingBySessionId: {
      pinned: { processing: true },
      running: { processing: true },
      done: { readySinceLastSelect: true },
    } })
    expect(model.pinnedSessionIds).toEqual(['pinned'])
    expect(model.activeSessionIds.sort()).toEqual(['done', 'pinned', 'running'])
    expect(model.folderRows[0].sessionIds).toEqual(['done', 'idle', 'pinned', 'running'])
    expect(new Set(visibleSessionIds(model)).size).toBe(4)
  })

it('lets Done clear stale ready chats and ignores CLI launch noise for Working', () => {
  const folders = [makeFolder('general')]
  const sessions = [
    { ...makeSession('settled', 'Settled', 'general'), settledAt: '2030-01-01T00:00:00.000Z' },
    makeSession('ready', 'Ready', 'general'),
    makeSession('cli', 'CLI', 'general'),
    { ...makeSession('asking', 'Asking', 'general'), settledAt: '2030-01-01T00:00:00.000Z' },
  ]
  const model = searchModel('', folders, sessions, undefined, {
    acpBindingBySessionId: {
      settled: { readySinceLastSelect: true },
      ready: { readySinceLastSelect: true },
      asking: { attentionKind: 'question' },
    },
    cliBindingBySessionId: { cli: { processing: true, lifecycleState: 'busy' } },
  })
  expect(model.activeSessionIds.sort()).toEqual(['asking', 'ready'])
})

describe('active chat settlement', () => {
  const day = 24 * 60 * 60 * 1000
  const nowMs = Date.parse('2026-01-10T00:00:00.000Z')
  it('keeps recently touched chats active until marked done after their last update', () => {
    const updated = nowMs - day
    expect(isUnsettledChat(updated, undefined, nowMs)).toBe(true)
    expect(isUnsettledChat(updated, new Date(updated + 1).toISOString(), nowMs)).toBe(false)
    expect(isUnsettledChat(updated, new Date(updated - 1).toISOString(), nowMs)).toBe(true)
    expect(isUnsettledChat(nowMs - ACTIVE_CHAT_AUTO_DONE_MS - 1, undefined, nowMs)).toBe(false)
  })
})

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

  it('keeps branches in one sidebar row until search identifies a branch', () => {
    const folders = [makeFolder('general')]
    const root = { ...makeSession('root', 'Original chat', 'general'), branchRootChatId: 'root' }
    const branch = {
      ...makeSession('branch', 'Edited deployment prompt', 'general', now, new Date('2026-01-01T01:00:00.000Z')),
      parentChatId: 'root',
      branchRootChatId: 'root',
    }

    expect(visibleSessionIds(searchModel('', folders, [root, branch]))).toEqual(['root'])
    expect(visibleSessionIds(searchModel('deployment', folders, [root, branch]))).toEqual(['branch'])
    expect(visibleSessionIds(searchModel('original', folders, [root, branch]))).toEqual(['root'])
  })

  it('sorts by activity (updatedAt), ignoring selection time (issue #49)', () => {
    const early = new Date('2026-01-01T00:00:00.000Z')
    const mid = new Date('2026-01-01T01:00:00.000Z')
    const late = new Date('2026-01-01T02:00:00.000Z')
    const folders = [makeFolder('general')]
    const sessions = [
      // Just selected (recent lastOpenedAt) but no new messages since.
      makeSession('s-selected', 'Selected Chat', 'general', late, early),
      // Selected earlier, but received a new message afterwards (newer updatedAt).
      makeSession('s-active', 'Active Chat', 'general', mid, late),
    ]

    // Selecting must NOT float a chat to the top; only activity reorders.
    const model = searchModel('', folders, sessions)

    expect(model.folderRows[0].sessionIds).toEqual(['s-active', 's-selected'])
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

  it('does not require message content for default search', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-one', 'Persisted Planning', 'general'),
      makeSession('s-two', 'Release', 'general'),
    ]

    const model = searchModel('persisted', folders, sessions)

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

  it('orders folder chats by most recent activity (updatedAt)', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-old', 'Old Chat', 'general', now, new Date('2026-01-01T09:00:00.000Z')),
      makeSession('s-new', 'New Chat', 'general', now, new Date('2026-01-01T11:00:00.000Z')),
      makeSession('s-middle', 'Middle Chat', 'general', now, new Date('2026-01-01T10:00:00.000Z')),
    ]

    const model = searchModel('', folders, sessions)

    expect(model.folderRows[0].sessionIds).toEqual(['s-new', 's-middle', 's-old'])
  })

  it('duplicates pinned chats into the top section and their normal folder', () => {
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
      { folderId: 'general', sessionIds: ['s-folder', 's-pinned'] },
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
    expect(matchModel.folderRows[0].sessionIds).toEqual(['s-folder-match', 's-pinned-match'])

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
    expect(visibleSessionIds(searchModel('alpha project', folders, sessions))).toEqual(['s-match'])
    expect(visibleSessionIds(searchModel('alpha missing', folders, sessions))).toEqual([])
  })

  it('filters by provider id', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-gemini', 'Gemini Chat', 'general', now, now, false, 'gemini-cli'),
      makeSession('s-codex', 'Codex Chat', 'general', now, now, false, 'codex-cli'),
    ]

    const model = searchModel('', folders, sessions, { providerIds: ['codex-cli'], statusIds: [] })

    expect(model.isSearching).toBe(true)
    expect(visibleSessionIds(model)).toEqual(['s-codex'])
  })

  it('filters by session state from runtime bindings', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-pinned', 'Pinned', 'general', now, now, true),
      makeSession('s-running', 'Running', 'general'),
      makeSession('s-attention', 'Attention', 'general'),
      makeSession('s-done', 'Done', 'general'),
      makeSession('s-idle', 'Idle', 'general'),
    ]

    const context = {
      acpBindingBySessionId: {
        's-running': { running: true, processing: true, lifecycleState: 'processing' },
        's-attention': { running: true, processing: false, lifecycleState: 'waitingPermission', attentionKind: 'permission' },
        's-done': { running: true, processing: false, lifecycleState: 'ready', readySinceLastSelect: true },
      },
      cliBindingBySessionId: {},
    }

    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['pinned'] }, context))).toEqual(['s-pinned'])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['running'] }, context))).toEqual(['s-running'])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['attention'] }, context))).toEqual(['s-attention'])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['done'] }, context))).toEqual(['s-done'])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['idle'] }, context))).toEqual(['s-idle'])
  })

  it('does not invent a displayed status from a bound process or hidden error', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-bound', 'Bound', 'general'),
      makeSession('s-error', 'Error', 'general'),
    ]
    const context = { acpBindingBySessionId: {
      's-bound': { running: true, lifecycleState: 'ready' },
      's-error': { lastError: 'hidden failure', lifecycleState: 'error', attentionKind: 'error' },
    } }

    const model = searchModel('', folders, sessions, undefined, context)
    expect(model.activeSessionIds).toEqual([])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['running'] }, context))).toEqual([])
    expect(visibleSessionIds(searchModel('', folders, sessions, { providerIds: [], statusIds: ['attention'] }, context))).toEqual([])
  })

  it('requires search text and active filters to match the same chat', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('s-search', 'Needle Chat', 'general', now, now, false, 'gemini-cli'),
      makeSession('s-match', 'Needle Codex Chat', 'general', now, now, false, 'codex-cli'),
      makeSession('s-provider', 'Other Chat', 'general', now, now, false, 'codex-cli'),
      makeSession('s-miss', 'Plain Chat', 'general', now, now, false, 'gemini-cli'),
    ]

    const model = searchModel('needle', folders, sessions, { providerIds: ['codex-cli'], statusIds: [] })

    expect(visibleSessionIds(model)).toEqual(['s-match'])
  })
  it('ANDs provider and state groups while ORing selections within each group, including deep search', () => {
    const folders = [makeFolder('general')]
    const sessions = [
      makeSession('codex', 'Match', 'general', now, now, false, 'codex-cli'),
      makeSession('gemini', 'Match', 'general', now, now, true, 'gemini-cli'),
      makeSession('other', 'Match', 'general', now, now, true, 'claude-code'),
      makeSession('idle', 'Match', 'general', now, now, false, 'codex-cli'),
    ]
    const filters: ChatSearchFilters = { providerIds: ['codex-cli', 'gemini-cli'], statusIds: ['running', 'pinned'] }
    const context = { acpBindingBySessionId: { codex: { processing: true } } }
    expect(visibleSessionIds(searchModel('match', folders, sessions, filters, context))).toEqual(['gemini', 'codex'])
    const model = buildChatSearchModel(folders, sessions, buildChatSearchIndex(sessions), ['contents'], new Set(['gemini', 'idle', 'other']), filters, context)
    expect(visibleSessionIds(model)).toEqual(['gemini'])
    expect(visibleSessionIds(buildChatSearchModel(folders, sessions, buildChatSearchIndex(sessions), ['contents'], new Set(), filters, context))).toEqual([])
  })

})


describe('interaction recency', () => {
  it('orders events within a second and keeps equal event times stable', () => {
    const a = { ...makeSession('a', 'A', null), interactionAt: new Date('2026-01-01T00:01:00.001Z') }
    const b = { ...makeSession('b', 'B', null), interactionAt: new Date('2026-01-01T00:01:00.002Z') }
    const context = { acpBindingBySessionId: { a: { processing: true }, b: { processing: true } } }
    expect(searchModel('', [], [a, b], undefined, context).activeSessionIds).toEqual(['b', 'a'])
    a.interactionAt = b.interactionAt
    expect(searchModel('', [], [b, a], undefined, context).activeSessionIds).toEqual(['a', 'b'])
    expect(searchModel('', [], [a, b], undefined, context).activeSessionIds).toEqual(['a', 'b'])
  })

  it('keeps streamed tools and assistant text from moving active chats', () => {
    const a = { ...makeSession('a', 'A', null), interactionAt: new Date('2026-01-01T00:01:00Z') }
    const b = { ...makeSession('b', 'B', null), interactionAt: new Date('2026-01-01T00:02:00Z') }
    const context = { acpBindingBySessionId: { a: { processing: true }, b: { processing: true } } }
    expect(searchModel('', [], [a, b], undefined, context).activeSessionIds).toEqual(['b', 'a'])
    a.updatedAt = new Date('2026-01-01T00:05:00Z')
    expect(searchModel('', [], [a, b], undefined, context).activeSessionIds).toEqual(['b', 'a'])
    a.interactionAt = new Date('2026-01-01T00:06:00Z')
    expect(searchModel('', [], [a, b], undefined, context).activeSessionIds).toEqual(['a', 'b'])
  })

  it('uses state event recency for branch families and keeps equal events stable', () => {
    const a = { ...makeSession('a', 'A', null), interactionAt: new Date('2026-01-01T00:01:00Z') }
    const b = { ...makeSession('b', 'B', null), interactionAt: new Date('2026-01-01T00:02:00Z') }
    const branch = { ...makeSession('branch', 'Branch', null), branchRootChatId: 'a', interactionAt: new Date('2026-01-01T00:03:00Z') }
    const context = { acpBindingBySessionId: { branch: { processing: true }, b: { processing: true } } }
    expect(searchModel('', [], [b, branch, a], undefined, context).activeSessionIds).toEqual(['a', 'b'])
    b.updatedAt = new Date('2026-01-01T00:09:00Z')
    expect(searchModel('', [], [a, branch, b], undefined, context).activeSessionIds).toEqual(['a', 'b'])
  })
})

it('keeps external history discoverable but inactive until UAM interaction', () => {
  for (const providerId of ['gemini', 'codex', 'opencode', 'claude', 'copilot']) {
    const imported: Session = { ...makeSession(providerId, 'External history', 'general', new Date(), new Date(), false, providerId), interactionAt: null }
    const folders = [makeFolder('general')]
    expect(searchModel('', folders, [imported]).activeSessionIds).toEqual([])
    expect(visibleSessionIds(searchModel('external', folders, [imported]))).toEqual([providerId])
    expect(searchModel('external', folders, [imported]).activeSessionIds).toEqual([])
    imported.updatedAt = new Date(Date.now() + 1000)
    imported.lastOpenedAt = new Date()
    expect(searchModel('', folders, [imported]).activeSessionIds).toEqual([])
    imported.interactionAt = new Date()
    expect(searchModel('', folders, [imported]).activeSessionIds).toEqual([providerId])
  }
})

it('activates an imported family when its UAM branch is used', () => {
  const root: Session = { ...makeSession('root', 'Imported', null, new Date(), new Date()), interactionAt: null }
  const branch: Session = { ...makeSession('branch', 'UAM branch', null), branchRootChatId: 'root', interactionAt: new Date() }
  expect(searchModel('', [], [root, branch]).activeSessionIds).toEqual(['root'])
  expect(searchModel('imported', [], [root, branch]).activeSessionIds).toEqual([])
})
