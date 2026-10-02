import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'

const { requests } = vi.hoisted(() => ({ requests: [] as string[] }))
vi.mock('../../config/buildFeatures', () => ({ COMPUTER_USE_ENABLED: false, SSH_ENABLED: false, MOBILE_COMPANION_ENABLED: false }))
vi.mock('../../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../ipc/cefBridge')>(),
  sendToCEF: vi.fn(async (request: { action: string }) => { requests.push(request.action); return { ok: true } }),
}))
import { SettingsModal } from './SettingsModal'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('omits disabled feature controls and mobile settings requests', async () => {
  window.cefQuery = undefined
  useAppStore.setState({ providers: [], sessions: [], folders: [], activeSessionId: null, providerModelCatalogs: [] })
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<SettingsModal />))
    expect(host.textContent).not.toContain('Computer Use')
    expect(host.textContent).not.toContain('Remote Hosts')
    const defaults = [...host.querySelectorAll('button')].find((button) => button.textContent?.trim() === 'Chat Defaults')
    expect(defaults).toBeDefined()
    await act(async () => defaults!.click())
    expect(host.textContent).not.toContain('Enable phone access')
    expect(requests).not.toContain('getCompanionSettings')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})
