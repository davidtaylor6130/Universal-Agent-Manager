import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ProviderMcpWarning } from './ProviderMcpWarning'
import { useAppStore } from '../../store/useAppStore'

const bridge = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: bridge, isCompanionContext: () => false }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
beforeEach(() => {
  bridge.mockReset()
  useAppStore.setState({ activeSessionId: 'one', sessions: [{ id: 'one', providerId: 'claude-cli', executionHostId: 'local', viewMode: 'chat', name: 'Test', messages: [] } as never] })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
})
afterEach(async () => { await act(async () => root.unmount()); host.remove(); vi.useRealTimers() })
const render = async () => { await act(async () => root.render(<ProviderMcpWarning />)) }
it('shows missing exact-chat connection and permits deliberate continuation', async () => {
  bridge.mockResolvedValue({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'missing', detail: 'No UAM tools configured.' }] } })
  await render()
  expect(bridge.mock.calls[0][0].payload.chatId).toBe('one')
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Continue without it')!.click())
  expect(host.textContent).toBe('')
})
it.each(['connected', 'configured-unverified', 'unsupported'])('does not call %s a missing connection', async status => {
  bridge.mockResolvedValue({ ok: true, data: { providers: [{ providerId: 'claude-cli', status, detail: 'Status' }] } })
  await render()
  expect(host.querySelector('[role="alert"]')).toBeNull()
})
it('ignores stale status from a different chat', async () => {
  let resolve!: (value: unknown) => void
  bridge.mockReturnValueOnce(new Promise(done => { resolve = done })).mockResolvedValue({ ok: true, data: { providers: [] } })
  await render()
  await act(async () => useAppStore.setState({ activeSessionId: null }))
  await act(async () => resolve({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'missing', detail: 'Old chat' }] } }))
  expect(host.querySelector('[role="alert"]')).toBeNull()
})


it('checks a new workspace even after deliberate continuation in the old workspace', async () => {
  bridge.mockResolvedValue({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'missing', detail: 'Missing' }] } })
  await render()
  await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Continue without it')!.click())
  await act(async () => useAppStore.setState({ sessions: useAppStore.getState().sessions.map(session => ({ ...session, workspaceDirectory: '/another/workspace' })) }))
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  expect(bridge.mock.calls.at(-1)?.[0].payload.workspaceDirectory).toBe('/another/workspace')
})


it('clears a missing warning when the same chat later confirms its handshake', async () => {
  vi.useFakeTimers()
  bridge.mockResolvedValueOnce({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'missing', detail: 'Missing' }] } }).mockResolvedValue({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'handshake-confirmed', detail: 'Confirmed' }] } })
  await render()
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  await act(async () => vi.advanceTimersByTimeAsync(5000))
  expect(host.querySelector('[role="alert"]')).toBeNull()
})

it('does not enable a chat whose provider changed while repair was saving', async () => {
  bridge.mockResolvedValue({ ok: true, data: { providers: [{ providerId: 'claude-cli', status: 'missing', detail: 'Missing' }] } })
  let finish!: (value: { ok: boolean }) => void
  const save = vi.fn(() => new Promise<{ ok: boolean }>(resolve => { finish = resolve }))
  const enableChat = vi.fn(async () => true)
  useAppStore.setState({ setCentralProviderConfiguration: save, setSessionUamControlEnabled: enableChat })
  await render()
  await act(async () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Enable UAM connection')!.click())
  await act(async () => {
    useAppStore.setState({ sessions: useAppStore.getState().sessions.map(session => ({ ...session, providerId: 'codex-cli' })) })
    finish({ ok: true })
  })
  expect(enableChat).not.toHaveBeenCalled()
})
