import { act } from 'react'
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
  act(() => root.render(<Pane />))
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  act(() => vi.advanceTimersByTime(60_000))
  expect(host.textContent).toContain('Stops in 10:00')
  act(() => vi.advanceTimersByTime(1000))
  expect(host.textContent).toContain('Stops in 9:59')
  act(() => host.querySelector('button')!.click())
  expect(host.querySelector('[aria-label="Runtime idle timeout"]')).toBeNull()
  act(() => vi.advanceTimersByTime(60_000))
  expect(host.textContent).toContain('Stops in 10:00')
  act(() => root.unmount())
})
