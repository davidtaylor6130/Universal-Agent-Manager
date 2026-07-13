import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { UPDATE_CHECK_INTERVAL_MS } from '../services/updateCatalog'
import { useAppStore } from '../store/useAppStore'
import { useUpdateMonitor } from './useUpdateMonitor'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function Probe() {
  useUpdateMonitor()
  return null
}

describe('useUpdateMonitor', () => {
  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
  })

  it('checks again every 24 hours while the app remains open', async () => {
    vi.useFakeTimers()
    vi.setSystemTime(new Date('2026-07-13T12:00:00.000Z'))
    let request = 0
    vi.stubGlobal('fetch', vi.fn(async () => {
      const isUamRequest = request % 6 === 0
      request += 1
      return {
        ok: true,
        json: async () => isUamRequest
          ? { tag_name: 'V4.1.0', html_url: 'https://example.test/uam' }
          : { version: '1.0.0' },
      }
    }))
    useAppStore.setState({
      appVersion: 'V4.1.0',
      providers: [],
      cliVersionManager: { providers: [] },
      updateChecksEnabled: true,
      updateLastCheckedAt: new Date().toISOString(),
      dismissedUpdateVersions: {},
    })

    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<Probe />))

    await act(async () => { await vi.advanceTimersByTimeAsync(UPDATE_CHECK_INTERVAL_MS - 1) })
    expect(fetch).not.toHaveBeenCalled()
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(fetch).toHaveBeenCalledTimes(6)
    await act(async () => { await vi.advanceTimersByTimeAsync(UPDATE_CHECK_INTERVAL_MS) })
    expect(fetch).toHaveBeenCalledTimes(12)

    act(() => root.unmount())
  })
})
