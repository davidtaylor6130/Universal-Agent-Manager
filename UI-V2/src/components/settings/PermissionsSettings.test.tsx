import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { PermissionsSettings } from './PermissionsSettings'
import { sendToCEF } from '../../ipc/cefBridge'
import { reportAppFailure } from '../../utils/appFailure'
vi.mock('../../config/buildFeatures', async (importOriginal) => ({ ...await importOriginal<typeof import('../../config/buildFeatures')>(), COMPUTER_USE_ENABLED: false, MOBILE_COMPANION_ENABLED: true }))
vi.mock('../../ipc/cefBridge', () => ({ sendToCEF: vi.fn() }))
vi.mock('../../utils/appFailure', () => ({ reportAppFailure: vi.fn(), reportAppNotice: vi.fn() }))
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
    const network = [...host.querySelectorAll('.uam-set-row')].find((row) => row.textContent?.includes('Local network'))!
    expect(network.textContent).toContain('Check system settings')
    expect(network.querySelector('[aria-label="Allowed"]')).toBeNull()
    await act(async () => network.querySelector('button')!.click())
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'requestAppPermission', payload: { permission: 'localNetwork' } })
    expect(reportAppFailure).toHaveBeenCalledWith('Permission could not be requested.', 'Permission request failed.')
    const speech = [...host.querySelectorAll('.uam-set-row')].find((row) => row.textContent?.includes('Speech recognition'))!
    expect(speech.querySelector('[role="img"][aria-label="Allowed"]')).toBeTruthy()
    expect(speech.textContent).not.toContain('Request')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})
