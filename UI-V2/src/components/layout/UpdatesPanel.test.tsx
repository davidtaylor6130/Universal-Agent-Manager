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
    hasCatalog: true,
    checking: false,
    error: '',
    lastCheckedAt: '2026-07-13T20:00:00.000Z',
    checkNow: vi.fn(async () => undefined),
    dismiss: vi.fn(),
    dismissAll: vi.fn(),
    applyCliProviderVersion: vi.fn(async () => true),
    applyRemoteHelperUpdate: vi.fn(async () => true),
    remoteHelperUpdatingId: '',
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

  it('surfaces a provider update that the backend refuses to start', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({ applyCliProviderVersion: vi.fn(async () => false) })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]') as HTMLButtonElement).click()
    })

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Codex CLI update could not be started')
    act(() => root.unmount())
  })

  it('installs an available SSH helper update from the updates panel', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [{
        id: 'remote-helper-lab', remoteHostId: 'lab', name: 'Homelab SSH helper',
        currentVersion: '4.8.0-alpha', latestVersion: '4.8.0-alpha-2', url: '', installable: true,
      }],
    })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    const update = host.querySelector('button[aria-label="Update Homelab SSH helper to 4.8.0-alpha-2"]') as HTMLButtonElement
    expect(update.textContent).toContain('Update helper')
    await act(async () => update.click())
    expect(state.applyRemoteHelperUpdate).toHaveBeenCalledWith('lab')
    expect(host.textContent).not.toContain('View release')
    act(() => root.unmount())
  })

  it('updates every installable item from one button', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [
        ...monitor().updates,
        { id: 'remote-helper-lab', remoteHostId: 'lab', name: 'Homelab SSH helper', currentVersion: '4.9.0-alpha-10', latestVersion: '4.9.0-alpha-11', url: '', installable: true },
      ],
    })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Update everything"]') as HTMLButtonElement).click()
      await Promise.resolve()
      await Promise.resolve()
    })
    expect(state.applyCliProviderVersion).toHaveBeenCalledWith('codex-cli', '0.130.0')
    expect(state.applyRemoteHelperUpdate).toHaveBeenCalledWith('lab')
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

  it('does not claim the app is current before a successful check or after a failure', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], hasCatalog: false, lastCheckedAt: '' })} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Updates have not been checked')
    expect(host.textContent).not.toContain('Everything is up to date')

    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], hasCatalog: false, error: 'Offline' })} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Could not confirm update status')
    expect(host.textContent).not.toContain('Everything is up to date')
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
    const panel = host.querySelector('[data-testid="updates-panel"]') as HTMLElement
    const output = host.querySelector('pre') as HTMLElement
    expect(panel.className).toContain('max-w-full')
    expect(output.className).toContain('max-w-full')
    expect(output.className).toContain('break-all')
    act(() => root.unmount())
  })

  it('contains long unbroken installer output at the 360px panel boundary', async () => {
    const host = document.createElement('div')
    host.style.width = '360px'
    const root = createRoot(host)
    await act(async () => root.render(<UpdatesPanel monitor={monitor({
      updates: [],
      providerUpdateResults: [{
        providerId: 'codex-cli', name: 'Codex CLI', status: 'failed', message: 'Failed',
        output: 'x'.repeat(2000), installedVersion: '0.124.0',
      }],
    })} onClose={vi.fn()} />))

    expect(host.querySelector('[data-testid="updates-panel"]')?.className).toContain('max-w-full')
    expect(host.querySelector('details')?.className).toContain('min-w-0')
    expect(host.querySelector('pre')?.className).toContain('break-all')
    act(() => root.unmount())
  })
})
