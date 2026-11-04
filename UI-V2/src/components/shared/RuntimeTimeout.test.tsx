import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { RuntimeTimeout } from './RuntimeTimeout'
import { useAppStore } from '../../store/useAppStore'
import { useRuntimeActivity } from '../../hooks/useRuntimeActivity'
vi.mock('../../ipc/cefBridge', async (importOriginal) => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCefContext: () => true, sendToCEF: vi.fn().mockResolvedValue({ ok: true }) }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
afterEach(() => vi.useRealTimers())
it('hides during grace, counts the shared deadline, and restarts grace on deliberate activity', () => {
  vi.useFakeTimers()
  vi.setSystemTime(1_000_000)
  useAppStore.setState({ cliBindingBySessionId: {}, acpBindingBySessionId: {} })
  useAppStore.setState((state) => ({ cliBindingBySessionId: { ...state.cliBindingBySessionId, chat: {
    terminalId: 'term', boundChatId: 'chat', running: true, lifecycleState: 'idle', processing: false, turnState: 'idle', readySinceLastSelect: false, active: true, lastError: '',
    idleCountdownStartsAtMs: 1_060_000, idleShutdownAtMs: 1_660_000, idleShutdownTimeoutSeconds: 600,
  } } }))
  function Pane() { const activity = useRuntimeActivity('chat'); return <div onClick={activity}><RuntimeTimeout chatId="chat" /><button>Interact</button></div> }
  const host = document.createElement('div')
  const root = createRoot(host)
  const rendered = vi.fn()
  act(() => root.render(<Profiler id="timeout" onRender={rendered}><Pane /></Profiler>))
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  rendered.mockClear()
  act(() => vi.advanceTimersByTime(59_999))
  expect(rendered).not.toHaveBeenCalled()
  act(() => vi.advanceTimersByTime(1))
  expect(host.textContent).toContain('Stops in 10:00')
  act(() => vi.advanceTimersByTime(1000))
  expect(host.textContent).toContain('Stops in 9:59')
  vi.setSystemTime(1_031_000)
  act(() => window.dispatchEvent(new Event('focus')))
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  act(() => vi.advanceTimersByTime(29_000))
  expect(host.textContent).toContain('Stops in 10:00')
  act(() => host.querySelector('button')!.click())
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  expect(vi.getTimerCount()).toBe(1)
  const scheduled = useAppStore.getState().cliBindingBySessionId.chat
  act(() => useAppStore.setState({ cliBindingBySessionId: { chat: { ...scheduled, running: false } } }))
  expect(vi.getTimerCount()).toBe(0)
  act(() => useAppStore.setState({ cliBindingBySessionId: { chat: scheduled } }))
  expect(vi.getTimerCount()).toBe(1)
  act(() => vi.advanceTimersByTime(60_000))
  expect(host.textContent).toContain('Stops in 10:00')
  act(() => vi.advanceTimersByTime(600_000))
  expect(host.textContent).toContain('Stops in 0:00')
  rendered.mockClear()
  act(() => vi.advanceTimersByTime(5000))
  expect(rendered).not.toHaveBeenCalled()
  act(() => useAppStore.setState(state => ({ cliBindingBySessionId: {
    ...state.cliBindingBySessionId, chat: { ...state.cliBindingBySessionId.chat, running: false },
  } })))
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  expect(vi.getTimerCount()).toBe(0)
  act(() => useAppStore.setState({ cliBindingBySessionId: { chat: {
    ...scheduled, running: true, idleCountdownStartsAtMs: Date.now() + 60_000, idleShutdownAtMs: Date.now() + 660_000,
  } } }))
  expect(vi.getTimerCount()).toBe(1)
  act(() => root.unmount())
  expect(vi.getTimerCount()).toBe(0)
  window.dispatchEvent(new Event('focus'))
  expect(vi.getTimerCount()).toBe(0)
})
