import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { useAppStore } from '../useAppStore'
import { sanitizeCppAppState, sanitizeCppChat, sanitizeGitBaseBranch } from './sanitizers'
import { sessionFromCppChat } from './reconcile'
import type { CreateSessionOptions } from './types'

describe('new chat Git base transport', () => {
  const initial = useAppStore.getState()
  beforeEach(() => {
    useAppStore.setState(initial, true)
    useAppStore.getState().loadFromCef(sanitizeCppAppState({ stateRevision: initial.lastAppliedStateRevision + 1, folders: [{ id: 'workspace', title: 'Workspace', directory: '/tmp/private-fixture' }], chats: [], providers: [{ id: 'codex-cli', name: 'Codex' }], settings: { gitEnabled: true, gitAutoWorktree: true, gitDefaultBaseBranch: 'main' } })!)
  })
  afterEach(() => { delete window.cefQuery; useAppStore.setState(initial, true) })
  const create = (options?: CreateSessionOptions) => useAppStore.getState().addSession('Fixture', 'workspace', 'codex-cli', undefined, undefined, 'chat', 'local', '', 'explicit', options)

  it('omits inherited base and sends explicit HEAD or named branch without changing global defaults', async () => {
    const payloads: Array<Record<string, unknown>> = []
    window.cefQuery = ({ request, onSuccess }) => { payloads.push(JSON.parse(request).payload); onSuccess('{}') }
    await expect(create()).resolves.toBe(true)
    await expect(create({ gitBaseBranch: '' })).resolves.toBe(true)
    await expect(create({ gitBaseBranch: ' feature/base ' })).resolves.toBe(true)
    expect(Object.prototype.hasOwnProperty.call(payloads[0], 'gitBaseBranch')).toBe(false)
    expect(payloads[1].gitBaseBranch).toBe('')
    expect(payloads[2].gitBaseBranch).toBe('feature/base')
    expect(useAppStore.getState().gitDefaultBaseBranch).toBe('main')
  })

  it('rejects null, malformed and oversized request values before any callback request', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    for (const options of [{ gitBaseBranch: null }, { gitBaseBranch: 42 }, { gitBaseBranch: 'main\nInjected' }, { gitBaseBranch: 'é'.repeat(128) }, { other: 'main' }]) {
      await expect(create(options as never)).resolves.toBe(false)
    }
    expect(requests).toBe(0)
    expect(sanitizeGitBaseBranch('é'.repeat(127))).toBe('é'.repeat(127))
    expect(sanitizeGitBaseBranch(' \u00a0main\u00a0 ')).toBe('\u00a0main\u00a0')
    expect(useAppStore.getState().gitDefaultBaseBranch).toBe('main')
  })

  it('refuses overrides for disabled Git, automatic routing, remote execution or offline mock creation', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    useAppStore.setState({ gitEnabled: false })
    await expect(create({ gitBaseBranch: '' })).resolves.toBe(false)
    useAppStore.setState({ gitEnabled: true, gitAutoWorktree: false })
    await expect(create({ gitBaseBranch: '' })).resolves.toBe(false)
    useAppStore.setState({ gitAutoWorktree: true })
    await expect(useAppStore.getState().addSession('Fixture', 'workspace', 'codex-cli', undefined, undefined, 'chat', 'local', '', 'automatic', { gitBaseBranch: '' })).resolves.toBe(false)
    await expect(useAppStore.getState().addSession('Fixture', null, 'codex-cli', undefined, undefined, 'chat', 'remote', '/tmp/fixture', 'explicit', { gitBaseBranch: '' })).resolves.toBe(false)
    expect(requests).toBe(0)
    delete window.cefQuery
    await expect(create({ gitBaseBranch: '' })).resolves.toBe(false)
  })

  it('projects nullable inherited and explicit bases while omitted or malformed partial chats preserve prior overrides', () => {
    const identity = { id: 'fixture', createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:00.000Z', lastOpenedAt: '2026-01-01T00:00:00.000Z' }
    const named = sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: 'feature/base' })!, undefined, [])
    expect(named.gitBaseBranch).toBe('feature/base')
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity })!, named, []).gitBaseBranch).toBe('feature/base')
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: 42 })!, named, []).gitBaseBranch).toBe('feature/base')
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: null })!, named, []).gitBaseBranch).toBeNull()
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: '' })!, named, []).gitBaseBranch).toBe('')
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity })!, undefined, []).gitBaseBranch).toBeUndefined()
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: 'feature/base' })!, named, [])).toBe(named)
    const inherited = sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: null })!, named, [])
    expect(inherited).not.toBe(named)
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: null })!, inherited, [])).toBe(inherited)
    const head = sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: '' })!, named, [])
    expect(head).not.toBe(named)
    expect(sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: '' })!, head, [])).toBe(head)
    const unicode = sessionFromCppChat(sanitizeCppChat({ ...identity, gitBaseBranch: '\u00a0feature/base\u00a0' })!, named, [])
    expect(unicode).not.toBe(named)
    expect(unicode.gitBaseBranch).toBe('\u00a0feature/base\u00a0')
  })
  it('transports only a complete current worktree context and refuses conflicting or stale creation', async () => {
    const context = { sourceChatId: 'source', expectedWorkspaceDirectory: '/tmp/selected-worktree', expectedExecutionHostId: 'local' }
    const source = { id: 'source', name: 'Source', viewMode: 'chat' as const, folderId: 'workspace', createdAt: new Date(), updatedAt: new Date(), workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: context.expectedWorkspaceDirectory, executionHostId: 'local' }
    useAppStore.setState({ sessions: [source] })
    const payloads: Record<string, unknown>[] = []
    window.cefQuery = ({ request, onSuccess }) => { payloads.push(JSON.parse(request).payload); onSuccess('{}') }
    await expect(create(context)).resolves.toBe(true)
    expect(payloads).toHaveLength(1)
    expect(payloads[0]).toMatchObject(context)
    expect(Object.prototype.hasOwnProperty.call(payloads[0], 'gitBaseBranch')).toBe(false)
    for (const invalid of [{ sourceChatId: 'source' }, { ...context, gitBaseBranch: '' }, { ...context, expectedWorkspaceDirectory: '/tmp/stale-worktree' }, { ...context, expectedExecutionHostId: 'remote' }])
      await expect(create(invalid)).resolves.toBe(false)
    expect(payloads).toHaveLength(1)
    useAppStore.setState({ sessions: [{ ...source, importedReadOnly: true }] })
    await expect(create(context)).resolves.toBe(false)
    expect(payloads).toHaveLength(1)
  })

})
