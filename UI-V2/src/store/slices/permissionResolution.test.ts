import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import { sendToCEF } from '../../ipc/cefBridge'
import type { AppState } from '../storeTypes'
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: () => true, sendToCEF: vi.fn() }))

describe('permission responses', () => {
  beforeEach(() => vi.resetAllMocks())
  const actions = () => createSessionsSlice(vi.fn(), () => ({ acpBindingBySessionId: {} }) as unknown as AppState, true)

  it('returns the backend reason when a response is refused, and true when it is accepted', async () => {
    vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'ACP permission request is no longer active.', requestId: 'r' })
    await expect(actions().resolveAcpPermission('chat', '7', 'allow_once')).resolves.toBe('ACP permission request is no longer active.')
    vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, requestId: 'r' })
    await expect(actions().resolveAcpPermission('chat', '7', 'allow_once')).resolves.toBe(false)
    vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: {}, requestId: 'r' })
    await expect(actions().resolveAcpPermission('chat', '7', 'allow_once')).resolves.toBe(true)
    expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'resolveAcpPermission', payload: { chatId: 'chat', requestId: '7', optionId: 'allow_once', cancelled: false } })
  })
})
