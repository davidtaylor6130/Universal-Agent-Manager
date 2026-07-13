import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { UpdatesPanel } from './UpdatesPanel'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function monitor(overrides: Partial<UpdateMonitor> = {}): UpdateMonitor {
  return {
    updates: [{
      id: 'codex-cli',
      providerId: 'codex-cli',
      name: 'Codex CLI',
      currentVersion: '0.124.0',
      latestVersion: '0.130.0',
      url: 'https://example.test/codex',
      installable: true,
    }],
    checking: false,
    error: '',
    lastCheckedAt: '2026-07-13T20:00:00.000Z',
    checkNow: vi.fn(async () => undefined),
    dismiss: vi.fn(),
    dismissAll: vi.fn(),
    applyCliProviderVersion: vi.fn(async () => true),
    providerChecksRunning: false,
    ...overrides,
  }
}

describe('UpdatesPanel', () => {
  it('shows current/latest versions and exposes update and dismiss actions', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor()
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    expect(host.textContent).toContain('Codex CLI')
    expect(host.textContent).toContain('0.124.0')
    expect(host.textContent).toContain('0.130.0')

    await act(async () => {
      ;(Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Update now')) as HTMLButtonElement).click()
    })
    expect(state.applyCliProviderVersion).toHaveBeenCalledWith('codex-cli', '0.130.0')

    await act(async () => {
      ;(host.querySelector('button[aria-label^="Dismiss Codex CLI"]') as HTMLButtonElement).click()
    })
    expect(state.dismiss).toHaveBeenCalledWith('codex-cli', '0.130.0')
    act(() => root.unmount())
  })

  it('renders an empty state and manual check action', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({ updates: [] })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Everything is up to date')
    await act(async () => {
      ;(Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Check now')) as HTMLButtonElement).click()
    })
    expect(state.checkNow).toHaveBeenCalledOnce()
    act(() => root.unmount())
  })
})
