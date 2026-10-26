import { beforeEach, expect, it, vi } from 'vitest'
import { useAppStore } from '../store/useAppStore'
import { sendToCEF, isCefContext } from '../ipc/cefBridge'
import { captureRepositoryContextReference } from './repositoryContextReferences'
import { formatContextReferences, sanitizeContextReferences } from './contextReferences'

// Store construction must remain in demo mode; each test explicitly enables its native capture mock.
vi.mock('../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../ipc/cefBridge')>(), sendToCEF: vi.fn(), isCefContext: vi.fn(() => false) }))
const native = { kind: 'branch', chatId: 'repository-source', chatCreatedAt: '2026-10-23T12:00:00Z', executionHostId: 'ssh-fixture',
  workspaceDirectory: '/native/worktree', branchName: 'feature/context', worktreeDirectory: '', sourceDirectory: '/native/source', baseRef: 'main', capturedAt: '2026-10-23T12:01:00Z' }
beforeEach(() => {
  vi.mocked(sendToCEF).mockReset()
  vi.mocked(isCefContext).mockReturnValue(true)
  useAppStore.setState({ folders: [], sessions: [{ id: native.chatId, name: 'Fixture', folderId: null, createdAt: new Date(native.chatCreatedAt), updatedAt: new Date(),
    executionHostId: native.executionHostId, workspaceDirectory: '/configured/source', workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/configured/worktree' }] })
})

it.each(['branch', 'worktree'] as const)('captures a native %s snapshot without submitting any display label or path as a selector', async (kind) => {
  const returned = { ...native, kind, worktreeDirectory: kind === 'worktree' ? native.workspaceDirectory : '' }
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: returned })
  const snapshot = await captureRepositoryContextReference(native.chatId, kind)
  expect(sendToCEF).toHaveBeenCalledWith({ action: 'captureChatReferenceContext', payload: { chatId: native.chatId, kind } }, false)
  expect(snapshot).toMatchObject({ kind, sourceChatId: native.chatId, sourceChatCreatedAt: native.chatCreatedAt, executionHostId: native.executionHostId,
    workspaceDirectory: native.workspaceDirectory, branchName: native.branchName, capturedAt: native.capturedAt })
  expect(JSON.parse(formatContextReferences([snapshot]).split('\n')[1])).toEqual(snapshot)
  expect(JSON.parse(snapshot.text).branchName).toBe(native.branchName)
  expect(sanitizeContextReferences([{ ...snapshot, inputWriteCompleted: true, checkoutToken: 'invented' }])).toEqual([snapshot])
})

it.each(['birth', 'host', 'workspace', 'association', 'removed', 'returned-birth', 'returned-host', 'returned-chat'])('refuses changed %s before retaining repository context', async (change) => {
  let finish!: (response: { ok: true; data: typeof native }) => void
  vi.mocked(sendToCEF).mockImplementation(() => new Promise((resolve) => { finish = resolve }))
  const capture = captureRepositoryContextReference(native.chatId, 'branch')
  useAppStore.setState((state) => ({ sessions: change === 'removed' ? [] : state.sessions.map((chat) => ({ ...chat,
    ...(change === 'birth' ? { createdAt: new Date('2026-10-23T13:00:00Z') } : {}),
    ...(change === 'host' ? { executionHostId: 'local' } : {}),
    ...(change === 'workspace' ? { workspaceWorktreeDirectory: '/other' } : {}),
    ...(change === 'association' ? { workspaceBaseRef: 'different-base' } : {}),
  })) }))
  finish({ ok: true, data: { ...native, ...(change === 'returned-birth' ? { chatCreatedAt: '2026-10-23T13:00:00Z' } : {}),
    ...(change === 'returned-host' ? { executionHostId: 'other' } : {}), ...(change === 'returned-chat' ? { chatId: 'other' } : {}) } })
  await expect(capture).rejects.toThrow('changed')
})

it.each([
  { branchName: '' }, { branchName: 'x'.repeat(1025) }, { sourceDirectory: '/bad\0path' }, { capturedAt: 'unknown' },
])('refuses incomplete or excessive native branch metadata: %j', async (fields) => {
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { ...native, ...fields } })
  await expect(captureRepositoryContextReference(native.chatId, 'branch')).rejects.toThrow('incomplete')
})

it('retains a worktree observation at detached HEAD and refuses unrelated worktree paths', async () => {
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { ...native, kind: 'worktree', branchName: '', worktreeDirectory: native.workspaceDirectory } })
  expect(await captureRepositoryContextReference(native.chatId, 'worktree')).toMatchObject({ kind: 'worktree', branchName: '', worktreeDirectory: native.workspaceDirectory })
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { ...native, kind: 'worktree', worktreeDirectory: '/foreign' } })
  await expect(captureRepositoryContextReference(native.chatId, 'worktree')).rejects.toThrow('incomplete')
})

it('surfaces native refusal and refuses demo capture without manufacturing repository context', async () => {
  vi.mocked(sendToCEF).mockResolvedValue({ ok: false, error: 'No named current branch.' })
  await expect(captureRepositoryContextReference(native.chatId, 'branch')).rejects.toThrow('No named current branch')
  vi.mocked(isCefContext).mockReturnValue(false)
  await expect(captureRepositoryContextReference(native.chatId, 'worktree')).rejects.toThrow('desktop chat')
  expect(sendToCEF).toHaveBeenCalledTimes(1)
})
