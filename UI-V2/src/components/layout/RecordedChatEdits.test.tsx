import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { ChatFileChangeReceipt, ChatFileChangeReceiptsResponse } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { VcsCommitPanel } from './VcsCommitPanel'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
let getReceipts: ReturnType<typeof vi.fn<(id: string) => Promise<ChatFileChangeReceiptsResponse>>>
const receipt = { chatId: 'chat-a', messageIndex: 2, messageCreatedAt: '2026-10-07', providerId: 'codex-cli', toolCallId: 'tool-edit', reportedPath: 'old.ts', path: 'old.ts', destinationPath: 'new.ts', kind: { type: 'update' }, patch: '-historical old\n+historical new' }
function response(receipts: ChatFileChangeReceipt[] = [receipt], omittedCount = 0) {
  return { chatId: 'chat-a', scope: 'recorded-provider-edits' as const, receipts, omittedCount }
}
async function openRecorded() {
  await act(async () => root.render(<VcsCommitPanel />))
  await act(async () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Recorded chat edits')!.click())
}
beforeEach(() => {
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
  getReceipts = vi.fn(async () => response())
  useAppStore.setState({
    activeSessionId: 'chat-a',
    getChatFileChangeReceipts: getReceipts,
    sessions: [{ id: 'chat-a', name: 'A', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/a', executionHostId: 'local', createdAt: new Date(), updatedAt: new Date() }],
    getVcsCommitStatus: vi.fn(async () => ({ available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/tmp/a', branchOrRevision: 'main', changedFiles: [{ path: 'workspace.ts', status: ' M', staged: false, additions: 1, deletions: 0, binary: false }], lineStatsReady: true, warning: '', error: '' })),
    getVcsFileDiff: vi.fn(async () => '+current workspace content'),
    commitVcsChanges: vi.fn(async () => ({ ok: true, message: '' })),
  })
})
afterEach(() => { act(() => root.unmount()); host.remove() })
describe('recorded chat edit scope', () => {
  it('reviews historical rename patches without fetching current diffs or enabling commits', async () => {
    await openRecorded()
    expect(getReceipts).toHaveBeenCalledWith('chat-a')
    expect(host.textContent).toContain('old.ts → new.ts')
    expect(host.textContent).toContain('Message 3')
    expect(host.querySelector('[aria-label="Commit summary"]')).toBeNull()
    expect(host.querySelector('[role="checkbox"]')).toBeNull()
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Review recorded edit 1: old.ts"]')!.click())
    expect(document.querySelector('[aria-label="Changes in Recorded edit: old.ts"]')?.textContent).toContain('historical new')
    expect(useAppStore.getState().getVcsFileDiff).not.toHaveBeenCalled()
    expect(useAppStore.getState().commitVcsChanges).not.toHaveBeenCalled()
    await act(async () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Workspace')!.click())
    expect(host.querySelector('[aria-label="Commit summary"]')).not.toBeNull()
    expect(document.querySelector('[aria-label="Changes in Recorded edit: old.ts"]')).toBeNull()
  })
  it('shows honest empty and omitted receipt states', async () => {
    getReceipts.mockResolvedValue(response([], 3))
    await openRecorded()
    expect(host.textContent).toContain('No recorded completed Codex edits')
    expect(host.textContent).toContain('3 saved edits could not be shown')
    expect(host.textContent).toContain('Shell changes and other providers are not attributed')
  })
  it('shows load failures and refreshes explicitly', async () => {
    getReceipts.mockRejectedValueOnce(new Error('History changed. Refresh to retry.')).mockResolvedValueOnce(response())
    await openRecorded()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('History changed')
    await act(async () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Refresh recorded edits')!.click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.textContent).toContain('old.ts')
  })
  it('retries from the error alert and guards malformed receipt timestamps', async () => {
    const invalidTimestamp = 'not-a-timestamp'
    getReceipts.mockRejectedValueOnce(new Error('Receipt history is temporarily unavailable.'))
      .mockResolvedValueOnce(response([{ ...receipt, messageCreatedAt: invalidTimestamp }]))
    await openRecorded()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('temporarily unavailable')
    await act(async () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Retry')!.click())
    expect(getReceipts).toHaveBeenCalledTimes(2)
    expect(host.querySelector('[role="alert"]')).toBeNull()
    const time = host.querySelector('time')!
    expect(time.textContent).toBe('Time unavailable')
    expect(time.getAttribute('title')).toBe(invalidTimestamp)
    expect(host.textContent).not.toContain('Invalid Date')
  })
  it.each(['chat', 'workspace', 'worktree', 'host'] as const)('drops a late response after %s ownership changes', async (change) => {
    let resolve!: (value: ReturnType<typeof response>) => void
    getReceipts.mockImplementationOnce(() => new Promise((done) => { resolve = done }))
    getReceipts.mockResolvedValue({ chatId: change === 'chat' ? 'chat-b' : 'chat-a', scope: 'recorded-provider-edits', receipts: [], omittedCount: 0 })
    await openRecorded()
    expect(host.textContent).toContain('Loading recorded chat edits')
    await act(async () => useAppStore.setState((state) => change === 'chat' ? { activeSessionId: 'chat-b', sessions: [...state.sessions, { ...state.sessions[0], id: 'chat-b' }] } : { sessions: state.sessions.map((session) => ({ ...session, ...(change === 'workspace' ? { workspaceDirectory: '/tmp/other' } : change === 'worktree' ? { workspaceWorktreeDirectory: '/tmp/new-worktree' } : { executionHostId: 'remote' }) })) }))
    await act(async () => resolve(response()))
    expect(host.textContent).not.toContain('old.ts')
    expect(host.textContent).toContain('No recorded completed Codex edits')
  })
  it('rejects receipts attributed to a different chat', async () => {
    getReceipts.mockResolvedValue(response([{ ...receipt, chatId: 'other-chat' }]))
    await openRecorded()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('invalid response')
    expect(host.textContent).not.toContain('old.ts')
  })
  it('reviews a saved deletion with a string kind', async () => {
    getReceipts.mockResolvedValue(response([{ ...receipt, kind: 'delete', destinationPath: '' }]))
    await openRecorded()
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Review recorded edit 1: old.ts"]')!.click())
    const dialog = document.querySelector('[aria-label="Changes in Recorded edit: old.ts"]')!
    expect(dialog.textContent).toContain('historical old')
    expect(dialog.querySelector('header')?.textContent).toContain('D')
  })
  it.each(['host', 'worktree'] as const)('closes an open historical patch when its %s changes', async (change) => {
    await openRecorded()
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Review recorded edit 1: old.ts"]')!.click())
    expect(document.querySelector('[aria-label="Changes in Recorded edit: old.ts"]')).not.toBeNull()
    await act(async () => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, ...(change === 'host' ? { executionHostId: 'remote' } : { workspaceWorktreeDirectory: '/tmp/other-worktree' }) })) })))
    expect(document.querySelector('[aria-label="Changes in Recorded edit: old.ts"]')).toBeNull()
  })

  it('drops a pending receipt response when the active chat is removed', async () => {
    let resolve!: (value: ReturnType<typeof response>) => void
    getReceipts.mockImplementationOnce(() => new Promise((done) => { resolve = done }))
    await openRecorded()
    await act(async () => useAppStore.setState({ sessions: [] }))
    await act(async () => resolve(response()))
    expect(host.textContent).not.toContain('old.ts')
    expect(host.textContent).toContain('Select a chat to review recorded edits')
  })

})
