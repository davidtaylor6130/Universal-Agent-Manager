import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import { sendToCEF } from '../../ipc/cefBridge'
import type { AppState } from '../storeTypes'
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: () => true, sendToCEF: vi.fn() }))

function actions() {
  const state = { sessions: [{ id: 'chat', workspaceDirectory: '/remote' }] } as unknown as AppState
  return createSessionsSlice(vi.fn(), () => state, true)
}

describe('getVcsCommitStatus failure reporting', () => {
  beforeEach(() => vi.resetAllMocks())

  it('keeps null for ordinary callers when the bridge read fails', async () => {
    vi.mocked(sendToCEF).mockResolvedValue({ ok: false, error: 'The remote runner request failed.' })
    await expect(actions().getVcsCommitStatus('chat', 'git', { requestId: 'r' })).resolves.toBeNull()
  })

  it('returns a failed bridge read as an error status when asked, so a dropped SSH host is not "no changes"', async () => {
    vi.mocked(sendToCEF).mockResolvedValue({ ok: false, error: 'The remote runner request failed.' })
    const status = await actions().getVcsCommitStatus('chat', 'git', { comparisonRef: 'chat', reportFailure: true })
    expect(status).toEqual(expect.objectContaining({ available: false, changedFiles: [], warning: '', error: 'The remote runner request failed.' }))
  })

  it('reports a malformed or empty failure with a fallback message', async () => {
    vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: 'not a status' }).mockResolvedValueOnce({ ok: false, error: '  ' })
    const slice = actions()
    expect((await slice.getVcsCommitStatus('chat', 'git', { reportFailure: true }))?.error).toBe('UAM could not read the repository status.')
    expect((await slice.getVcsCommitStatus('chat', 'git', { reportFailure: true }))?.error).toBe('UAM could not read the repository status.')
  })
})
