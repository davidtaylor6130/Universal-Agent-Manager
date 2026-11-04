import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { UpdatesPanel } from './UpdatesPanel'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { availableUpdates } from '../../services/updateCatalog'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

// Check and install failures reach the shared alert path (reportAppFailure); collect them here.
const appFailures: AppFailure[] = []
const onAppFailure = (event: Event) => appFailures.push((event as CustomEvent<AppFailure>).detail)
const reportedText = () => appFailures.map((failure) => `${failure.message} ${failure.detail}`).join('\n')
beforeEach(() => { appFailures.length = 0; window.addEventListener(APP_FAILURE_EVENT, onAppFailure) })
afterEach(() => window.removeEventListener(APP_FAILURE_EVENT, onAppFailure))

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
    // Excerpts no longer render as a disclosure; they are only offered as the release-notes tooltip.
    expect(host.querySelector('details')).toBeNull()
    expect(rows).toHaveLength(3)
    const open = vi.spyOn(window, 'open').mockImplementation(() => null)
    try {
      await act(async () => host.querySelector<HTMLButtonElement>(`[aria-label="Release notes for ${updates[0].name} ${updates[0].latestVersion}"]`)!.click())
      expect(open).toHaveBeenLastCalledWith('https://example.test/uam', '_blank', 'noopener')
      await act(async () => host.querySelector<HTMLButtonElement>(`[aria-label="Release notes for ${updates[2].name} ${updates[2].latestVersion}"]`)!.click())
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
    // The excerpt is never rendered as markup; it only reaches the release-notes tooltip as text.
    expect(host.querySelector('details')).toBeNull()
    expect(host.querySelector('script, img[src="x"], article a')).toBeNull()
    expect(host.textContent).not.toContain('unsafe()')
    act(() => root.unmount())
  })

  it('drops the excerpt when the available release changes and keeps the actions for the newest release', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const base = monitor().updates[0]
    const render = (releaseNotesExcerpt?: string, latestVersion = base.latestVersion) => act(async () => root.render(
      <UpdatesPanel monitor={monitor({ updates: [{ ...base, latestVersion, releaseNotesExcerpt }] })} onClose={vi.fn()} />,
    ))
    await render('Older fixes.')
    expect(host.querySelector('details')).toBeNull()
    await render('Newer fixes.', '0.131.0')
    expect(host.querySelector('[aria-label="Release notes for Codex CLI 0.131.0"]')).not.toBeNull()
    expect(host.textContent).not.toContain('Older fixes.')
    for (const excerpt of [undefined, '', '   ']) {
      await render(excerpt)
      expect(host.querySelector('details')).toBeNull()
      expect(host.querySelector('[aria-label="Release notes for Codex CLI 0.130.0"]')).not.toBeNull()
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
    expect(host.querySelector('button[aria-label="Update Codex · alpha to 0.130.0"]')).toBeNull()
    expect(host.querySelector('[role="progressbar"][aria-busy="true"]')?.textContent).toContain('Codex · alpha')
    expect(host.querySelector('button[aria-label="Update Codex · beta to 0.130.0"]')).not.toBeNull()
    expect(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]')).not.toBeNull()
    act(() => root.unmount())
  })

  it('shows current/latest versions and exposes update and dismiss actions', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor()
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    expect(host.textContent).toContain('Codex CLI')
    expect(host.textContent).not.toContain('1 update available')
    expect(host.textContent).not.toContain('Current')
    expect(host.textContent).not.toContain('Available')
    expect(host.textContent).toContain('0.124.0')
    expect(host.textContent).toContain('0.130.0')
    expect(host.querySelector('[data-update-available="true"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Update Codex CLI to 0.130.0"]')).not.toBeNull()
    expect(host.querySelector('button[aria-label="Release notes for Codex CLI 0.130.0"]')).not.toBeNull()

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
    expect(host.querySelector('[aria-label="Update Codex CLI to 0.130.0"]')).toBeNull()
    expect(host.querySelector('[role="progressbar"][aria-busy="true"]')).not.toBeNull()
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Update everything"]')!.disabled).toBe(true)
    await act(async () => { if (throws) reject(new Error('Connection lost')); else resolve({ ok: false, error: 'Not admitted' }) })
    expect(host.querySelector('.uam-update-row--error')).not.toBeNull()
    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Codex CLI update failed')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Retry Codex CLI update to 0.130.0"]')!.click())
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

    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Codex CLI update failed')
    expect(host.querySelector('button[aria-label="Copy Codex CLI update logs"]')).not.toBeNull()
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.textContent).not.toContain('Finish active provider work')
    await act(async () => (host.querySelector('button[aria-label="Retry Codex CLI update to 0.130.0"]') as HTMLButtonElement).click())
    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Codex CLI update failed')
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
    expect(update.querySelector('svg')).not.toBeNull()
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
      expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Homelab SSH helper update failed')
      expect(host.querySelector('button[aria-label="Copy Homelab SSH helper update logs"]')).not.toBeNull()
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
    expect(host.querySelector('header button[aria-label="Check again"]')).toBeTruthy()
    expect(host.querySelector('footer')).toBeNull()
    await act(async () => {
      ;(host.querySelector('button[aria-label="Check again"]') as HTMLButtonElement).click()
    })
    expect(state.checkNow).toHaveBeenCalledOnce()
    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], checking: true })} onClose={vi.fn()} />))
    const checking = host.querySelector('button[aria-label="Check again"]') as HTMLButtonElement
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
    expect(appFailures.map((failure) => failure.message)).toEqual(['Update check failed'])
    expect(reportedText()).toContain('Offline')
    expect(host.textContent).not.toContain('Everything is up to date')
    act(() => root.unmount())
  })

  it('alerts once per distinct monitor error across panel remounts', async () => {
    const error = 'Remount check offline'
    for (let mount = 0; mount < 2; mount++) {
      const host = document.createElement('div')
      const root = createRoot(host)
      await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], hasCatalog: false, error })} onClose={vi.fn()} />))
      act(() => root.unmount())
    }
    expect(appFailures.filter((failure) => failure.detail === error)).toHaveLength(1)
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

    const [codex, opencode] = [...host.querySelectorAll<HTMLElement>('.uam-np-collapse > *')]
    expect(codex.getAttribute('aria-busy')).toBe('true')
    expect(codex.getAttribute('data-indeterminate')).toBe('true')
    expect(codex.classList.contains('uam-progress-fill')).toBe(true)
    expect(opencode.getAttribute('aria-busy')).toBeNull()
    expect(opencode.classList.contains('uam-progress-fill')).toBe(false)
    const install = opencode.querySelector<HTMLButtonElement>('button[aria-label="Update OpenCode to 1.18.0"]')!
    expect(install.disabled).toBe(false)
    act(() => root.unmount())
  })

  it('dismisses a failed provider install from its update row', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const state = monitor({
      updates: [{ ...monitor().updates[0], id: 'opencode-cli', providerId: 'opencode-cli', name: 'OpenCode', currentVersion: '1.17.15', latestVersion: '1.18.0' }],
      providerUpdateResults: [{
        providerId: 'opencode-cli',
        name: 'OpenCode',
        status: 'failed',
        message: 'Update command failed.',
        output: 'EEXIST: /opt/homebrew/bin/opencode',
        installedVersion: '1.17.15',
      }],
    })
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerUpdateResults: [] }} onClose={vi.fn()} />))
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))

    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('OpenCode update failed')
    expect(host.textContent).not.toContain('EEXIST: /opt/homebrew/bin/opencode')
    expect(host.querySelector('button[aria-label="Copy OpenCode update logs"]')).not.toBeNull()
    act(() => (host.querySelector('button[aria-label="Dismiss OpenCode 1.18.0"]') as HTMLButtonElement).click())
    expect(state.dismiss).toHaveBeenCalledWith('opencode-cli', '1.18.0')
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
    expect(reportedText()).toContain('Codex · Alpha check could not be started.')
    await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Retry Codex · Alpha version check"]')!.click())
    expect(appFailures.filter((failure) => failure.detail.includes('check could not be started.'))).toHaveLength(1)
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Dismiss Codex · Alpha version check error"]')!.click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.textContent).not.toContain('Everything is up to date')
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerCheckErrors: [], checking: true }} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Checking for updates')
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerCheckErrors: [], hasProviderCheckErrors: false }} onClose={vi.fn()} />))
    expect(host.textContent).toContain('Everything is up to date')
    act(() => root.unmount())
  })

  it('keeps the 360px panel boundary when a failed install is shown on its row', async () => {
    const host = document.createElement('div')
    host.style.width = '360px'
    const root = createRoot(host)
    const empty = monitor({ updates: [{ ...monitor().updates[0], name: 'Codex CLI' }] })
    await act(async () => root.render(<UpdatesPanel monitor={empty} onClose={vi.fn()} />))
    await act(async () => root.render(<UpdatesPanel monitor={monitor({
      updates: [{ ...monitor().updates[0], name: 'Codex CLI' }],
      providerUpdateResults: [{
        providerId: 'codex-cli', name: 'Codex CLI', status: 'failed', message: 'Failed',
        output: 'x'.repeat(2000), installedVersion: '0.124.0',
      }],
    })} onClose={vi.fn()} />))

    expect(host.querySelector('[data-testid="updates-panel"]')?.className).toContain('max-w-full')
    expect(host.querySelector('details')).toBeNull()
    expect(host.querySelector('pre')).toBeNull()
    expect(host.querySelector('.uam-update-row--error')).not.toBeNull()
    act(() => root.unmount())
  })

  it('keeps a failed result with no matching row visible until dismissed', async () => {
    vi.useFakeTimers()
    const host = document.createElement('div')
    const root = createRoot(host)
    const failed = { providerId: 'gemini-cli', name: 'Gemini CLI', status: 'failed' as const, message: 'Install refused.', output: 'EACCES: /usr/local/bin' }
    const state = monitor({ updates: [] })
    await act(async () => root.render(<UpdatesPanel monitor={state} onClose={vi.fn()} />))
    await act(async () => root.render(<UpdatesPanel monitor={{ ...state, providerUpdateResults: [failed] }} onClose={vi.fn()} />))

    const row = host.querySelector('.uam-update-row--error')
    expect(row?.textContent).toContain('Gemini CLI update failed')
    expect(host.querySelector('button[aria-label="Copy Gemini CLI update logs"]')).not.toBeNull()
    expect(host.querySelector('button[aria-label^="Retry Gemini CLI"]')).toBeNull()
    expect(host.textContent).not.toContain('Everything is up to date')
    await act(async () => vi.advanceTimersByTime(20_000))
    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Gemini CLI update failed')
    act(() => (host.querySelector('button[aria-label="Dismiss Gemini CLI update failure"]') as HTMLButtonElement).click())
    expect(host.querySelector('.uam-update-row--error')).toBeNull()
    act(() => root.unmount())
    vi.useRealTimers()
  })

  it('shows failures from before mount as rows but keeps earlier successes hidden', async () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const results = [
      { providerId: 'gemini-cli', name: 'Gemini CLI', status: 'failed' as const, message: 'Install refused.', output: 'EACCES' },
      { providerId: 'opencode-cli', name: 'OpenCode', status: 'succeeded' as const, message: 'Installed 1.18.0.', installedVersion: '1.18.0' },
    ]
    await act(async () => root.render(<UpdatesPanel monitor={monitor({ updates: [], providerUpdateResults: results })} onClose={vi.fn()} />))

    expect(host.querySelector('.uam-update-row--error')?.textContent).toContain('Gemini CLI update failed')
    expect(host.textContent).not.toContain('OpenCode')
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
  // ConfirmDialog portals to document.body, so its buttons are looked up there.
  await act(async () => [...document.body.querySelectorAll('button')].find((button) => button.textContent === 'Cancel')!.click())
  expect(state.stopSessionsAndUpdate).not.toHaveBeenCalled()
  await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Update Codex CLI to 0.130.0"]')!.click())
  await act(async () => [...document.body.querySelectorAll('button')].find((button) => button.textContent === 'Stop session and install update')!.click())
  expect(state.stopSessionsAndUpdate).toHaveBeenCalledWith('codex-cli', '0.130.0', undefined)
  await act(async () => root.unmount())
})
