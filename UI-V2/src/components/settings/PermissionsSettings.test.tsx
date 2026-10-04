import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { PermissionsSettings } from './PermissionsSettings'
import { sendToCEF } from '../../ipc/cefBridge'
vi.mock('../../config/buildFeatures', () => ({ COMPUTER_USE_ENABLED: false, MOBILE_COMPANION_ENABLED: true }))
vi.mock('../../ipc/cefBridge', () => ({ sendToCEF: vi.fn() }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('keeps network status unverified and reports native request failures', async () => {
  vi.mocked(sendToCEF).mockImplementation(async (request) => request.action === 'getAppPermissions'
    ? { ok: true, data: { platform: 'macos', localNetwork: 'system_managed', microphone: 'denied', speechRecognition: 'allowed' } }
    : { ok: false, error: 'Permission request failed.' })
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<PermissionsSettings />))
    const network = [...host.querySelectorAll('tr')].find((row) => row.textContent?.includes('Local Network'))!
    expect(network.textContent).toContain('Check system settings')
    expect(network.textContent).not.toContain('Allowed')
    await act(async () => network.querySelector('button')!.click())
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'requestAppPermission', payload: { permission: 'localNetwork' } })
    expect(host.querySelector('[role="status"]')?.textContent).toBe('Permission request failed.')
    const speech = [...host.querySelectorAll('tr')].find((row) => row.textContent?.includes('Speech Recognition'))!
    expect(speech.textContent).toContain('Allowed')
    expect(speech.textContent).not.toContain('Request')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})
