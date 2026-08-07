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
    providerStates: [],
    providerTaskRunning: false,
    providerUpdateResults: [],
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
    expect(host.textContent).toContain('1 update available')
    expect(host.textContent).toContain('Current')
    expect(host.textContent).toContain('Available')
    expect(host.textContent).toContain('0.124.0')
    expect(host.textContent).toContain('0.130.0')
    expect(host.querySelector('[data-update-available="true"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]')?.textContent).toContain('Install update')
    expect(host.querySelector('button[aria-label="Open Codex CLI update instructions"]')?.textContent).toContain('Release notes')

    await act(async () => {
      ;(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]') as HTMLButtonElement).click()
    })
    expect(state.applyCliProviderVersion).toHaveBeenCalledWith('codex-cli', '0.130.0')

    await act(async () => {
      ;(host.querySelector('button[aria-label^="Dismiss Codex CLI"]') as HTMLButtonElement).click()
    })
    expect(state.dismiss).toHaveBeenCalledWith('codex-cli', '0.130.0')
    act(() => root.unmount())
  })

  it('renders accessible icon-only footer actions and their loading state', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({ updates: [] })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Everything is up to date')
    expect(host.querySelector('header button[aria-label="Check for updates"]')).toBeTruthy()
    expect(host.querySelector('footer button[aria-label="Check for updates"]')).toBeNull()
    await act(async () => {
      ;(host.querySelector('button[aria-label="Check for updates"]') as HTMLButtonElement).click()
    })
    expect(state.checkNow).toHaveBeenCalledOnce()
    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], checking: true })} onClose={vi.fn()} />))
    const checking = host.querySelector('button[aria-label="Checking for updates"]') as HTMLButtonElement
    expect(checking.disabled).toBe(true)
    expect(checking.getAttribute('aria-busy')).toBe('true')
    expect(checking.querySelector('.animate-spin')).toBeTruthy()
    act(() => root.unmount())
  })

  it('animates only the provider being updated', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [
        ...monitor().updates,
        {
          id: 'opencode-cli',
          providerId: 'opencode-cli',
          name: 'OpenCode',
          currentVersion: '1.17.15',
          latestVersion: '1.18.0',
          url: 'https://example.test/opencode',
          installable: true,
        },
      ],
      providerStates: [
        {
          providerId: 'codex-cli',
          installedVersion: '0.124.0',
          selectedVersion: '0.130.0',
          availableVersions: [],
          preferredVersion: '',
          status: 'installing',
          message: 'Running Codex install command...',
          running: true,
          lastCommand: '',
          lastOutput: '',
          installMethod: 'npm',
          lastInstallStatus: 'running',
        },
      ],
      providerTaskRunning: true,
    })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    const codex = host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]') as HTMLButtonElement
    const opencode = host.querySelector('button[aria-label="Update OpenCode to 1.18.0"]') as HTMLButtonElement
    expect(codex.getAttribute('aria-busy')).toBe('true')
    expect(codex.textContent).toContain('Updating…')
    expect(opencode.getAttribute('aria-busy')).toBeNull()
    expect(opencode.textContent).toContain('Install update')
    expect(opencode.disabled).toBe(true)
    act(() => root.unmount())
  })

  it('shows the completed provider install result after its update row disappears', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [],
      providerUpdateResults: [{
        providerId: 'opencode-cli',
        name: 'OpenCode',
        status: 'failed',
        message: 'Update command failed.',
        output: 'EEXIST: /opt/homebrew/bin/opencode',
        installedVersion: '1.17.15',
      }],
    })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('OpenCode update failed')
    expect(host.textContent).toContain('EEXIST: /opt/homebrew/bin/opencode')
    act(() => root.unmount())
  })
})
