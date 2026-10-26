import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { UpdatesPanel } from './UpdatesPanel'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { availableUpdates } from '../../services/updateCatalog'

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
    requestCliProviderUpdate: vi.fn(async () => ({ ok: true })),
    installCliProviderVersion: vi.fn(async () => true),
    installProviderUpdates: vi.fn(async () => ({ ok: true })),
    automaticProviderUpdates: false,
    setUpdateSettings: vi.fn(async () => true),
    stopSessionsAndUpdate: vi.fn(async () => ({ ok: true })),
    applyRemoteHelperUpdate: vi.fn(async () => ({ ok: true })),
    remoteHelperUpdatingId: '',
    providerStates: [],
    providerTaskRunning: false,
    providerUpdateResults: [],
    providerCheckErrors: [],
    dismissProviderCheckError: vi.fn(),
    refreshCliProviderVersion: vi.fn(async () => true),
    ...overrides,
    hasProviderCheckErrors: overrides.hasProviderCheckErrors ?? (overrides.providerCheckErrors?.length ?? 0) > 0,
  }
}

describe('UpdatesPanel', () => {
  it('renders version-matched UAM, npm and Homebrew excerpts from the update projection', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const installed = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '' }
    const updates = availableUpdates({
      checkedAt: '2026-10-23T00:00:00Z',
      uam: { version: '5.0.0', url: 'https://example.test/uam', releaseNotesExcerpt: 'Application fixes.' },
      providers: { 'codex-cli': {
        version: '2.0.0', url: 'https://example.test/npm', releaseNotesExcerpt: 'npm release fixes.',
        homebrew: { version: '1.9.0', url: 'https://example.test/homebrew', releaseNotesExcerpt: 'Homebrew release fixes.' },
      } },
    }, '4.9.0', {
      providers: [{ ...installed, installMethod: 'npm' }],
      remoteProviders: [{ ...installed, installMethod: 'homebrew-cask', executionHostId: 'lab', executionHostName: 'Lab' }],
    }, [], {})
    const state = monitor({ updates })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    const rows = [...host.querySelectorAll('article[data-update-available]')]
    expect(rows).toHaveLength(3)
    for (const [index, excerpt] of ['Application fixes.', 'npm release fixes.', 'Homebrew release fixes.'].entries()) {
      const disclosure = rows[index].querySelector('details')!
      expect(disclosure.open).toBe(false)
      expect(disclosure.querySelector('p')?.textContent).toBe(excerpt)
      expect(disclosure.querySelector('summary')?.getAttribute('aria-label')).toBe(`Release notes excerpt for ${updates[index].name} ${updates[index].latestVersion}`)
      await act(async () => disclosure.querySelector('summary')!.click())
      expect(disclosure.open).toBe(true)
      await act(async () => disclosure.querySelector('summary')!.click())
      expect(disclosure.open).toBe(false)
    }
    const open = vi.spyOn(window, 'open').mockImplementation(() => null)
    try {
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="View Universal Agent Manager 5.0.0 release"]')!.click())
      expect(open).toHaveBeenLastCalledWith('https://example.test/uam', '_blank', 'noopener')
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Open codex-cli · Lab update instructions"]')!.click())
      expect(open).toHaveBeenLastCalledWith('https://example.test/homebrew', '_blank', 'noopener')
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Update codex-cli · Lab to 1.9.0"]')!.click())
      expect(state.requestCliProviderUpdate).toHaveBeenCalledExactlyOnceWith('codex-cli', '1.9.0', 'lab')
    } finally {
      open.mockRestore()
      act(() => root.unmount())
    }
  })

  it('renders excerpts as literal text without HTML or Markdown execution', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const excerpt = '<script>unsafe()</script><img src=x onerror=unsafe()> [link](javascript:unsafe())'
    const base = monitor().updates[0]
    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [{ ...base, releaseNotesExcerpt: excerpt }] })} onClose={vi.fn()} />))
    const disclosure = host.querySelector('details')!
    await act(async () => disclosure.querySelector('summary')!.click())
    expect(disclosure.querySelector('p')?.textContent).toBe(excerpt)
    expect(disclosure.querySelector('script, img, a')).toBeNull()
    act(() => root.unmount())
  })

  it('omits unavailable excerpts and resets disclosure when the available release changes', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const base = monitor().updates[0]
    const render = (releaseNotesExcerpt?: string, latestVersion = base.latestVersion) => act(async () => root.render(
      <UpdatesPanel monitor={monitor({ updates: [{ ...base, latestVersion, releaseNotesExcerpt }] })} onClose={vi.fn()} />,
    ))
    await render('Older fixes.')
    await act(async () => host.querySelector('summary')!.click())
    expect(host.querySelector('details')?.open).toBe(true)
    await render('Newer fixes.', '0.131.0')
    expect(host.querySelector('details')?.open).toBe(false)
    expect(host.querySelector('details p')?.textContent).toBe('Newer fixes.')
    expect(host.textContent).not.toContain('Older fixes.')
    for (const excerpt of [undefined, '', '   ']) {
      await render(excerpt)
      expect(host.querySelector('details')).toBeNull()
      expect(host.querySelector('[aria-label="Open Codex CLI update instructions"]')).not.toBeNull()
      expect(host.querySelector('[aria-label="Update Codex CLI to 0.130.0"]')).not.toBeNull()
    }
    act(() => root.unmount())
  })

  it('routes same-provider remote items and update-all to their own hosts', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const base = monitor().updates[0]
    const state = monitor({ updates: [base, ...['alpha', 'beta'].map((executionHostId) => ({ ...base, id: JSON.stringify([executionHostId, base.providerId]), executionHostId, name: `Codex · ${executionHostId}` })),
      { id: 'helper', remoteHostId: 'alpha', name: 'Alpha SSH helper', currentVersion: '1', latestVersion: '2', url: '', installable: true }],
    })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    await act(async () => (host.querySelector('button[aria-label="Update Codex · beta to 0.130.0"]') as HTMLButtonElement).click())
    expect(state.requestCliProviderUpdate).toHaveBeenCalledWith('codex-cli', '0.130.0', 'beta')
    await act(async () => (host.querySelector('button[aria-label="Dismiss Codex · alpha 0.130.0"]') as HTMLButtonElement).click())
    expect(state.dismiss).toHaveBeenCalledWith(JSON.stringify(['alpha', 'codex-cli']), '0.130.0')
    vi.mocked(state.requestCliProviderUpdate).mockClear()
    await act(async () => (host.querySelector('button[aria-label="Update everything"]') as HTMLButtonElement).click())
    expect(state.installProviderUpdates).toHaveBeenCalledWith(expect.arrayContaining([
      expect.objectContaining({ providerId: 'codex-cli', executionHostId: 'alpha' }),
      expect.objectContaining({ providerId: 'codex-cli', executionHostId: 'beta' }),
      expect.objectContaining({ providerId: 'codex-cli', id: 'codex-cli' }),
    ]))
    expect(state.applyRemoteHelperUpdate).toHaveBeenCalledWith('alpha')
    const providerState = { providerId: 'codex-cli', installedVersion: '0.124.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'installing' as const, message: '', running: true, lastCommand: '', lastOutput: '' }
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerStates: [{ ...providerState, executionHostId: 'alpha' }], providerTaskRunning: true }} onClose={vi.fn()} />))
    expect(host.querySelector('button[aria-label="Update Codex · alpha to 0.130.0"]')?.textContent).toContain('Updating')
    expect(host.querySelector('button[aria-label="Update Codex · beta to 0.130.0"]')?.textContent).toContain('Install update')
    expect(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]')?.textContent).toContain('Install update')
    act(() => root.unmount())
  })

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
    expect(state.requestCliProviderUpdate).toHaveBeenCalledWith('codex-cli', '0.130.0')

    await act(async () => {
      ;(host.querySelector('button[aria-label^="Dismiss Codex CLI"]') as HTMLButtonElement).click()
    })
    expect(state.dismiss).toHaveBeenCalledWith('codex-cli', '0.130.0')
    act(() => root.unmount())
  })

  it.each([false, true])('admits one manual install while awaiting native admission and permits retry after failure (throws: %s)', async (throws) => {
    const host = document.createElement('div')
    const root = createRoot(host)
    let resolve!: (value: { ok: boolean; error?: string }) => void
    let reject!: (reason: Error) => void
    const request = vi.fn(() => new Promise<{ ok: boolean; error?: string }>((done, fail) => { resolve = done; reject = fail }))
    const state = monitor({ requestCliProviderUpdate: request })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    const install = host.querySelector<HTMLButtonElement>('[aria-label="Update Codex CLI to 0.130.0"]')!
    act(() => { install.click(); install.click(); host.querySelector<HTMLButtonElement>('[aria-label="Update everything"]')!.click() })
    expect(request).toHaveBeenCalledTimes(1)
    expect(state.installProviderUpdates).not.toHaveBeenCalled()
    expect(install.disabled).toBe(true)
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Update everything"]')!.disabled).toBe(true)
    await act(async () => { if (throws) reject(new Error('Connection lost')); else resolve({ ok: false, error: 'Not admitted' }) })
    expect(install.disabled).toBe(false)
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('update could not be started')
    act(() => install.click())
    expect(request).toHaveBeenCalledTimes(2)
    await act(async () => resolve({ ok: true }))
    act(() => root.unmount())
  })

  it('surfaces a provider update that the backend refuses to start', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({ requestCliProviderUpdate: vi.fn(async () => ({ ok: false, error: 'The CLI installation method could not be identified.' })) })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]') as HTMLButtonElement).click()
    })

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Codex CLI update could not be started')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('The CLI installation method could not be identified.')
    expect(host.textContent).not.toContain('Finish active provider work')
    act(() => (host.querySelector('button[aria-label="Dismiss update error"]') as HTMLButtonElement).click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    await act(async () => (host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]') as HTMLButtonElement).click())
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

  it.each(['single', 'all'] as const)('keeps a fallback notice when a %s helper update has no error detail', async (mode) => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [{ id: 'remote-helper-lab', remoteHostId: 'lab', name: 'Homelab SSH helper', currentVersion: '4.9.0-alpha-9', latestVersion: '4.9.0-alpha-19', url: '', installable: true }],
      applyRemoteHelperUpdate: vi.fn(async () => ({ ok: false, error: '  ' })),
    })
    try {
      await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
      const label = mode === 'all' ? 'Update everything' : 'Update Homelab SSH helper to 4.9.0-alpha-19'
      await act(async () => host.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)!.click())
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Homelab SSH helper')
      expect(host.querySelector('[role="alert"]')?.textContent).toContain(mode === 'all' ? 'Check its update status' : 'Check its SSH connection')
    } finally {
      act(() => root.unmount())
    }
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
    expect(state.installProviderUpdates).toHaveBeenCalledWith(expect.arrayContaining([expect.objectContaining({ providerId: 'codex-cli', latestVersion: '0.130.0' })]))
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
    act(() => (host.querySelector('button[aria-label="Dismiss update check error"]') as HTMLButtonElement).click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
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
    expect(opencode.disabled).toBe(false)
    act(() => root.unmount())
  })

  it('dismisses a failed provider install after its update row disappears', async () => {
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
    act(() => (host.querySelector('button[aria-label="Dismiss OpenCode update error"]') as HTMLButtonElement).click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    act(() => root.unmount())
  })

  it('shows a retryable remote check failure without claiming an empty catalog is current', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const failure = { providerId: 'codex-cli', executionHostId: 'alpha', name: 'Codex · Alpha', message: 'SSH connection timed out.' }
    const state = monitor({ updates: [], providerCheckErrors: [failure] })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Codex · Alpha version check failed')
    expect(host.textContent).toContain('SSH connection timed out.')
    expect(host.textContent).not.toContain('Everything is up to date')
    await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Retry Codex · Alpha version check"]')!.click())
    expect(state.refreshCliProviderVersion).toHaveBeenCalledExactlyOnceWith('codex-cli', 'alpha')
    vi.mocked(state.refreshCliProviderVersion).mockResolvedValueOnce(false)
    await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Retry Codex · Alpha version check"]')!.click())
    expect(host.textContent).toContain('Codex · Alpha check could not be started.')
    await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Retry Codex · Alpha version check"]')!.click())
    expect(host.textContent).not.toContain('Codex · Alpha check could not be started.')
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Dismiss Codex · Alpha version check error"]')!.click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.textContent).not.toContain('Everything is up to date')
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerCheckErrors: [], checking: true }} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Checking for updates')
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerCheckErrors: [], hasProviderCheckErrors: false }} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Everything is up to date')
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

it('keeps a running session intact when cancelled and requests scoped shutdown only after confirmation', async () => {
  const host = document.createElement('div')
  const root = createRoot(host)
  const state = monitor({ providerStates: [{ providerId: 'codex-cli', installedVersion: '0.124.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified', message: '', running: false, blockingChatIds: ['chat-one'], lastCommand: '', lastOutput: '' }] })
  await act(async () => root.render(<UpdatesPanel monitor={state} onClose={() => {}} />))
  await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Update Codex CLI to 0.130.0"]')!.click())
  expect(state.requestCliProviderUpdate).not.toHaveBeenCalled()
  await act(async () => [...host.querySelectorAll('button')].find((button) => button.textContent === 'Cancel')!.click())
  expect(state.stopSessionsAndUpdate).not.toHaveBeenCalled()
  await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Update Codex CLI to 0.130.0"]')!.click())
  await act(async () => [...host.querySelectorAll('button')].find((button) => button.textContent === 'Stop session and install update')!.click())
  expect(state.stopSessionsAndUpdate).toHaveBeenCalledWith('codex-cli', '0.130.0', undefined)
  await act(async () => root.unmount())
})
