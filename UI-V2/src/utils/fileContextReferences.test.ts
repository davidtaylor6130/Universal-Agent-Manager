import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../store/useAppStore'
import { captureWorkspaceFileReference, draftFileMentions } from './fileContextReferences'
import { CONTEXT_REFERENCE_MAX_BYTES, formatContextReferences } from './contextReferences'

const read = vi.fn()
const identity = { chatId: 'file-context', chatCreatedAt: '2026-10-23T12:00:00Z' }
beforeEach(() => {
  read.mockReset()
  useAppStore.setState({ getWorkspaceFilePreview: read, folders: [], sessions: [{
    id: 'file-context', name: 'Fixture', folderId: null, viewMode: 'chat', createdAt: new Date('2026-10-23T12:00:00Z'), updatedAt: new Date(),
    executionHostId: 'ssh-fixture', workspaceDirectory: '/base', workspaceWorktreeDirectory: '/worktree', workspaceIsolationKind: 'gitWorktree',
  }] })
})

describe('explicit workspace file snapshots', () => {
  it('offers bounded @-paths without reading files or treating email addresses as mentions', () => {
    expect(draftFileMentions('Email user@example.com; read @src/main.ts and @"docs/my note.md" @src/main.ts')).toEqual(['src/main.ts', 'docs/my note.md'])
    expect(draftFileMentions('@a @b @c @d @e')).toEqual(['a', 'b', 'c', 'd'])
    expect(read).not.toHaveBeenCalled()
  })

  it('captures native-resolved path, workspace and host with bounded UTF-8 delivery text', async () => {
    read.mockResolvedValue({ ...identity, text: '😀'.repeat(10000), path: 'src/main.ts', workspace: '/worktree', executionHostId: 'ssh-fixture' })
    const snapshot = await captureWorkspaceFileReference('file-context', 'src/main.ts')
    expect(read).toHaveBeenCalledWith('file-context', 'src/main.ts')
    expect(snapshot).toMatchObject({ kind: 'file', sourceChatId: 'file-context', sourcePath: 'src/main.ts', workspaceDirectory: '/worktree', executionHostId: 'ssh-fixture', truncated: true })
    expect(new TextEncoder().encode(snapshot.text)).toHaveLength(CONTEXT_REFERENCE_MAX_BYTES)
    expect(snapshot.text.endsWith('😀')).toBe(true)
    const payload = JSON.parse(formatContextReferences([snapshot]).split('\n')[1])
    expect(payload.text).toBe(snapshot.text)
    expect(payload.sourcePath).toBe('src/main.ts')
    expect(payload).not.toHaveProperty('terminalId')
  })

  it.each(['workspace', 'host', 'birth', 'removed', 'returned-host'])('refuses a stale %s without manufacturing a snapshot', async (change) => {
    let finish!: (file: unknown) => void
    read.mockImplementation(() => new Promise((resolve) => { finish = resolve }))
    const pending = captureWorkspaceFileReference('file-context', 'file.txt')
    useAppStore.setState((state) => ({ sessions: change === 'removed' ? [] : state.sessions.map((chat) => ({ ...chat,
      ...(change === 'workspace' ? { workspaceWorktreeDirectory: '/other' } : {}),
      ...(change === 'host' ? { executionHostId: 'local' } : {}),
      ...(change === 'birth' ? { createdAt: new Date('2026-10-23T13:00:00Z') } : {}),
    })) }))
    finish({ ...identity, text: 'Captured text', path: 'file.txt', workspace: '/worktree', executionHostId: change === 'returned-host' ? 'wrong' : 'ssh-fixture' })
    await expect(pending).rejects.toThrow('workspace or host changed')
  })

  it('retains the native resolved workspace rather than granting authority to frontend path text', async () => {
    useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, executionHostId: 'local', workspaceIsolationKind: '', workspaceDirectory: '~/project' })) }))
    read.mockResolvedValue({ ...identity, text: 'Native resolved text', path: 'readme.md', workspace: '/Users/fixture/project', executionHostId: 'local' })
    expect(await captureWorkspaceFileReference('file-context', 'readme.md')).toMatchObject({ sourcePath: 'readme.md', workspaceDirectory: '/Users/fixture/project' })
  })

  it.each(['chatId', 'chatCreatedAt'] as const)('refuses a native file capture without its %s identity witness', async (field) => {
    const response: Record<string, unknown> = { ...identity, text: 'Captured text', path: 'file.txt', workspace: '/worktree', executionHostId: 'ssh-fixture' }
    delete response[field]
    read.mockResolvedValue(response)
    await expect(captureWorkspaceFileReference('file-context', 'file.txt')).rejects.toThrow('workspace or host changed')
  })

  it('preserves empty files and surfaces native refusal instead of a misleading empty snapshot', async () => {
    read.mockResolvedValueOnce({ ...identity, text: '', path: '/worktree/empty', workspace: '/worktree', executionHostId: 'ssh-fixture' })
    expect(await captureWorkspaceFileReference('file-context', 'empty')).toMatchObject({ sourceChatId: identity.chatId, sourceChatCreatedAt: identity.chatCreatedAt, text: '', truncated: false })
    read.mockRejectedValueOnce(new Error('File is outside the workspace.'))
    await expect(captureWorkspaceFileReference('file-context', '../private')).rejects.toThrow('outside the workspace')
  })
})
