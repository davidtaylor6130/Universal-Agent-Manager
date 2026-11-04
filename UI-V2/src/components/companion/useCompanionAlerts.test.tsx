import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, expect, it, vi } from 'vitest'
import { useCompanionAlerts } from './useCompanionAlerts'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let alerts: ReturnType<typeof useCompanionAlerts>
function Observer() { alerts = useCompanionAlerts(true); return <div>{alerts.attention.length}</div> }

beforeEach(() => {
  const entries = new Map<string, string>()
  Object.defineProperty(window, 'localStorage', { configurable: true, value: {
    getItem: (key: string) => entries.get(key) ?? null, setItem: (key: string, value: string) => entries.set(key, value),
  } })
  useAppStore.setState({ sessions: [{ id: 'a', name: 'Chat A' }], acpBindingBySessionId: {}, cliBindingBySessionId: {} } as unknown as Parameters<typeof useAppStore.setState>[0])
})

it('alerts once per input request, including a new request in the same chat', async () => {
  const showNotification = vi.fn().mockResolvedValue(undefined)
  const registration = { showNotification }
  vi.stubGlobal('Notification', { permission: 'granted', requestPermission: vi.fn().mockResolvedValue('granted') })
  Object.defineProperty(navigator, 'serviceWorker', { configurable: true, value: { register: vi.fn().mockResolvedValue(registration), ready: Promise.resolve(registration), getRegistration: vi.fn().mockResolvedValue(registration) } })
  Object.defineProperty(window, 'isSecureContext', { configurable: true, value: true })
  const host = document.createElement('div'); const root = createRoot(host)
  const waiting = (id: string) => useAppStore.setState({ acpBindingBySessionId: { a: { attentionKind: 'permission', pendingPermission: { requestId: id } } } } as unknown as Parameters<typeof useAppStore.setState>[0])
  try {
    await act(async () => root.render(<Observer />))
    await act(async () => alerts.enableBrowser())
    await act(async () => waiting('request-1'))
    expect(alerts.attention.map((chat) => chat.id)).toEqual(['a'])
    expect(showNotification).toHaveBeenCalledTimes(1)
    await act(async () => waiting('request-1'))
    expect(showNotification).toHaveBeenCalledTimes(1)
    await act(async () => waiting('request-2'))
    expect(showNotification).toHaveBeenCalledTimes(2)
    await act(async () => alerts.setEnabled(false))
    await act(async () => waiting('request-3'))
    expect(showNotification).toHaveBeenCalledTimes(2)
    expect(window.localStorage.getItem('uam-companion-input-alerts')).toBe('false')
  } finally { await act(async () => root.unmount()); vi.unstubAllGlobals() }
})

it('keeps in-app alerts available when notification permission is denied', async () => {
  vi.stubGlobal('Notification', { permission: 'denied', requestPermission: vi.fn().mockResolvedValue('denied') })
  Object.defineProperty(navigator, 'serviceWorker', { configurable: true, value: {} })
  Object.defineProperty(window, 'isSecureContext', { configurable: true, value: true })
  useAppStore.setState({ acpBindingBySessionId: { a: { attentionKind: 'question' } } } as unknown as Parameters<typeof useAppStore.setState>[0])
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<Observer />))
    await act(async () => alerts.enableBrowser())
    expect(alerts.notificationError).toContain('blocked')
    expect(alerts.browserEnabled).toBe(false)
    expect(alerts.attention).toHaveLength(1)
  } finally { await act(async () => root.unmount()); vi.unstubAllGlobals() }
})
