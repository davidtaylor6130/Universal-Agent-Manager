import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { UPDATE_CHECK_INTERVAL_MS } from '../services/updateCatalog'
import { useAppStore } from '../store/useAppStore'
import { useUpdateMonitor } from './useUpdateMonitor'
import { UpdatesPanel } from '../components/layout/UpdatesPanel'
import type { CliVersionProviderState } from '../store/cpp/types'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function Probe() {
  useUpdateMonitor()
  return null
}

describe('useUpdateMonitor', () => {
  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
    delete window.cefQuery
  })

  it.each(['single', 'all'] as const)('shows the native helper installation error for %s updates', async (mode) => {
    const previous = useAppStore.getState()
    const remoteHost = { id: 'alpha', label: 'Gaming AI Desktop', transport: 'ssh' as const, sshAlias: 'alpha', runnerStatus: 'ready' as const, runnerVersion: '4.9.0-alpha-9', platform: 'windows', architecture: 'x86_64', lastSeenAt: '' }
    useAppStore.setState({ updateChecksEnabled: false, executionHosts: [remoteHost] })
    const detail = 'Stop or finish work running on this host before updating its helper.'
    window.cefQuery = ({ onFailure }) => onFailure(409, detail)
    const updates = [{ id: 'remote-helper-alpha', remoteHostId: 'alpha', name: 'Gaming AI Desktop SSH helper', currentVersion: '4.9.0-alpha-9', latestVersion: '4.9.0-alpha-19', url: '', installable: true }]
    function Panel() { return <UpdatesPanel monitor={{ ...useUpdateMonitor(), updates }} onClose={() => {}} /> }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => root.render(<Panel />))
      const label = mode === 'all' ? 'Update everything' : 'Update Gaming AI Desktop SSH helper to 4.9.0-alpha-19'
      await act(async () => host.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)!.click())
      expect(host.querySelector('[role="alert"]')?.textContent).toContain(detail)
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Gaming AI Desktop SSH helper')
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Dismiss update error"]')!.click())
      expect(host.querySelector('[role="alert"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      useAppStore.setState(previous, true)
    }
  })

  it.each(['close', 'failure'] as const)('sequences update-all through native completion and stops the remaining queue on %s', async (ending) => {
    const previous = useAppStore.getState()
    const provider = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '2.0.0', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '', lastInstallStatus: 'succeeded' as const }
    const updates = ['alpha', 'beta', 'gamma'].map((executionHostId) => ({ id: executionHostId, providerId: provider.providerId, executionHostId, name: executionHostId, currentVersion: '1.0.0', latestVersion: '2.0.0', url: '', installable: true }))
    useAppStore.setState({ updateChecksEnabled: false, cliVersionManager: { providers: [], remoteProviders: updates.map((update) => ({ ...provider, executionHostId: update.executionHostId })) } })
    const requests: string[] = []
    const push = (executionHostId: string, changes: Partial<CliVersionProviderState>) => {
      const manager = useAppStore.getState().cliVersionManager
      useAppStore.setState({ cliVersionManager: { ...manager, remoteProviders: manager.remoteProviders!.map((entry) => entry.executionHostId === executionHostId ? { ...entry, ...changes } : entry) } })
    }
    window.cefQuery = ({ request, onSuccess }) => {
      const { payload } = JSON.parse(request)
      requests.push(payload.executionHostId)
      push(payload.executionHostId, { status: 'installing', running: true, lastInstallStatus: 'running' })
      onSuccess('{}')
    }
    function Panel() { return <UpdatesPanel monitor={{ ...useUpdateMonitor(), updates }} onClose={() => {}} /> }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => root.render(<Panel />))
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Update everything"]')!.click())
      expect(requests).toEqual(['alpha'])
      await act(async () => push('beta', { installedVersion: '2.0.0' }))
      expect(requests).toEqual(['alpha'])
      await act(async () => push('alpha', { status: 'checking', running: true, lastInstallStatus: 'succeeded' }))
      expect(requests).toEqual(['alpha'])
      await act(async () => push('alpha', { status: 'verified', running: false, installedVersion: '2.0.0' }))
      expect(requests).toEqual(['alpha', 'beta'])
      expect(host.querySelector<HTMLButtonElement>('button[aria-label="Update everything"]')!.disabled).toBe(true)
      if (ending === 'close') {
        // Closing cancels the remaining queue, without stopping or replaying the active install.
        await act(async () => root.render(null))
        await act(async () => push('beta', { status: 'verified', running: false, installedVersion: '2.0.0' }))
      } else {
        await act(async () => push('beta', { status: 'verified', running: false, lastInstallStatus: 'failed' }))
        expect(host.querySelector('[role="alert"]')?.textContent).toContain('Updates stopped at beta')
        expect(host.querySelector<HTMLButtonElement>('button[aria-label="Update everything"]')!.disabled).toBe(false)
      }
      expect(requests).toEqual(['alpha', 'beta'])
    } finally {
      act(() => root.unmount())
      useAppStore.setState(previous, true)
    }
  })

  it.each(['rejected', 'failed', 'verification failed', 'missing', 'timeout', 'aborted', 'fast completion'] as const)('settles a CLI update on %s without replay or a leaked subscription', async (outcome) => {
    vi.useFakeTimers()
    const previous = useAppStore.getState()
    const provider = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '2.0.0', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '', lastInstallStatus: 'succeeded' as const }
    useAppStore.setState({ updateChecksEnabled: false, cliVersionManager: { providers: [provider] } })
    window.cefQuery = vi.fn(({ onSuccess }) => {
      if (outcome === 'rejected') { onSuccess('{"ok":false}'); return }
      if (outcome !== 'timeout') useAppStore.setState({ cliVersionManager: { providers: [{ ...provider, status: 'installing', running: true, lastInstallStatus: 'running' }] } })
      if (outcome === 'fast completion') useAppStore.setState({ cliVersionManager: { providers: [{ ...provider, installedVersion: '2.0.0' }] } })
      onSuccess('{}')
    })
    const subscribe = useAppStore.subscribe
    const unsubscribe = vi.fn()
    const subscription = vi.spyOn(useAppStore, 'subscribe').mockImplementation((listener) => {
      const remove = subscribe(listener)
      return () => { unsubscribe(); remove() }
    })
    let result: ReturnType<typeof useUpdateMonitor> | undefined
    function Results() { result = useUpdateMonitor(); return null }
    const root = createRoot(document.createElement('div'))
    try {
      act(() => root.render(<Results />))
      const controller = new AbortController()
      let completed!: Promise<boolean>
      await act(async () => { completed = result!.installCliProviderVersion('codex-cli', '2.0.0', undefined, controller.signal) })
      await act(async () => {
        if (outcome === 'aborted') controller.abort()
        else if (outcome === 'timeout') vi.advanceTimersByTime(16 * 60 * 1000)
        else if (outcome === 'missing') useAppStore.setState({ cliVersionManager: { providers: [] } })
        else if (outcome === 'failed' || outcome === 'verification failed') useAppStore.setState({ cliVersionManager: { providers: [{ ...provider, lastInstallStatus: outcome === 'failed' ? 'failed' : 'succeeded', checkError: outcome === 'verification failed' ? 'Disconnected' : '' }] } })
      })
      expect(await completed).toBe(outcome === 'fast completion')
      expect(window.cefQuery).toHaveBeenCalledTimes(1)
      expect(unsubscribe).toHaveBeenCalledTimes(1)
      expect(vi.getTimerCount()).toBe(0)
    } finally {
      act(() => root.unmount())
      subscription.mockRestore()
      useAppStore.setState(previous, true)
    }
  })

  it('reports same-provider remote failures with their host identity', () => {
    const state = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: 'Install failed', running: false, lastInstallStatus: 'failed' as const, lastCommand: '', lastOutput: 'installer output' }
    useAppStore.setState({ updateChecksEnabled: false, cliVersionManager: { providers: [state], remoteProviders: [
      { ...state, executionHostId: 'alpha', executionHostName: 'Alpha' },
      { ...state, executionHostId: 'beta', executionHostName: 'Beta' },
    ] } })
    let result: ReturnType<typeof useUpdateMonitor> | undefined
    function Results() { result = useUpdateMonitor(); return null }
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<Results />))
    expect(result?.providerUpdateResults.map((entry) => entry.executionHostId)).toEqual([undefined, 'alpha', 'beta'])
    expect(result?.providerUpdateResults[1].name).toContain('Alpha')
    expect(result?.providerUpdateResults[2].name).toContain('Beta')
    act(() => root.unmount())
  })

  it('keeps failed empty-version remote checks separate from missing CLIs and other hosts', () => {
    const provider = { providerId: 'codex-cli', installedVersion: '', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'unavailable' as const, message: 'Could not check Codex version.', checkError: 'SSH connection timed out.', running: false, lastCommand: '', lastOutput: '' }
    const remoteHost = { id: 'alpha', label: 'Alpha server', transport: 'ssh' as const, sshAlias: 'alpha', runnerStatus: 'ready' as const, runnerVersion: '4.9.0', platform: 'windows', architecture: 'x64', lastSeenAt: '' }
    useAppStore.setState({ updateChecksEnabled: false, executionHosts: [remoteHost], cliVersionManager: {
      providers: [provider], remoteProviders: [
        { ...provider, executionHostId: 'alpha' },
        { ...provider, providerId: 'opencode-cli', executionHostId: 'alpha', checkError: '' },
        { ...provider, executionHostId: 'deleted' },
      ],
    } })
    let result: ReturnType<typeof useUpdateMonitor> | undefined
    function Results() { result = useUpdateMonitor(); return null }
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<Results />))
    expect(result!.providerCheckErrors).toEqual([{ providerId: 'codex-cli', executionHostId: 'alpha', name: expect.stringContaining('Alpha server'), message: 'SSH connection timed out.' }])
    act(() => useAppStore.setState({ cliVersionManager: { providers: [], remoteProviders: [{ ...provider, executionHostId: 'alpha', status: 'checking', running: true }] } }))
    expect(result!.providerCheckErrors).toEqual([])
    act(() => root.unmount())
  })

  it('persists dismissal of a remote check error across panel remounts and shows a changed failure', async () => {
    const previous = useAppStore.getState()
    const failurePrefix = 'SSH connection timed out while checking remote CLI version; verify SSH transport and runner status. '.repeat(5)
    const firstFailure = `${failurePrefix}certificate detail A`
    const nextFailure = `${failurePrefix}certificate detail B`
    const provider = { providerId: 'codex-cli', installedVersion: '', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'unavailable' as const, message: 'Could not check Codex version.', checkError: firstFailure, running: false, lastCommand: '', lastOutput: '' }
    const remoteHost = { id: 'alpha', label: 'Alpha server', transport: 'ssh' as const, sshAlias: 'alpha', runnerStatus: 'ready' as const, runnerVersion: '4.9.0', platform: 'windows', architecture: 'x64', lastSeenAt: '' }
    useAppStore.setState({ updateChecksEnabled: false, dismissedUpdateVersions: {}, executionHosts: [remoteHost], providers: [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '', description: '' }], setUpdateSettings: async (settings) => {
      const dismissedUpdateVersions = settings.dismissedUpdateVersions ?? useAppStore.getState().dismissedUpdateVersions
      useAppStore.setState({ dismissedUpdateVersions: Object.fromEntries(Object.entries(dismissedUpdateVersions).map(([key, value]) => [key, value.slice(0, 128)])) })
      return true
    }, cliVersionManager: {
      providers: [], remoteProviders: [{ ...provider, executionHostId: 'alpha' }],
    } })
    function Panel() { return <UpdatesPanel monitor={useUpdateMonitor()} onClose={() => {}} /> }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => root.render(<Panel />))
      expect(host.textContent).toContain('Codex · Alpha server version check failed')
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Dismiss Codex · Alpha server version check error"]')!.click())
      const persistedIdentity = useAppStore.getState().dismissedUpdateVersions['["alpha","codex-cli"]']
      expect(firstFailure.length).toBeGreaterThan(128)
      expect(persistedIdentity).toMatch(/^error:[a-f0-9]{16}$/)
      expect(persistedIdentity.length).toBeLessThanOrEqual(128)
      await act(async () => root.render(null))
      await act(async () => root.render(<Panel />))
      expect(host.textContent).not.toContain('Codex · Alpha server version check failed')
      expect(host.textContent).toContain('Could not confirm update status')
      expect(host.textContent).not.toContain('Everything is up to date')

      act(() => useAppStore.setState({ cliVersionManager: { providers: [], remoteProviders: [{ ...provider, executionHostId: 'alpha', checkError: nextFailure }] } }))
      expect(host.textContent).toContain('Codex · Alpha server version check failed')
      expect(host.textContent).toContain('certificate detail B')
    } finally {
      act(() => root.unmount())
      useAppStore.setState(previous, true)
    }
  })

  it('waits for persisted CEF settings before attempting an automatic check', async () => {
    window.cefQuery = vi.fn()
    vi.stubGlobal('fetch', vi.fn())
    useAppStore.setState({
      lastAppliedStateRevision: -1,
      updateChecksEnabled: true,
      updateLastCheckedAt: '',
    })

    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<Probe />))
    await act(async () => { await Promise.resolve() })
    expect(fetch).not.toHaveBeenCalled()

    act(() => useAppStore.setState({ lastAppliedStateRevision: 0, updateChecksEnabled: false }))
    await act(async () => { await Promise.resolve() })
    expect(fetch).not.toHaveBeenCalled()
    act(() => root.unmount())
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
    expect(fetch).toHaveBeenCalledTimes(11)
    await act(async () => { await vi.advanceTimersByTimeAsync(UPDATE_CHECK_INTERVAL_MS) })
    expect(fetch).toHaveBeenCalledTimes(22)

    act(() => root.unmount())
  })

  it('starts only one update check before the checking state rerenders', async () => {
    let monitor: ReturnType<typeof useUpdateMonitor> | null = null
    function MonitorProbe() {
      monitor = useUpdateMonitor()
      return null
    }
    const finishFetches: Array<(response: { ok: boolean; json: () => Promise<{ version: string }> }) => void> = []
    vi.stubGlobal('fetch', vi.fn(() => new Promise((resolve) => finishFetches.push(resolve))))
    useAppStore.setState({
      providers: [],
      cliVersionManager: { providers: [] },
      updateChecksEnabled: false,
      dismissedUpdateVersions: {},
    })
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<MonitorProbe />))

    const checks: Array<Promise<void>> = []
    act(() => {
      if (monitor) {
        checks.push(monitor.checkNow(), monitor.checkNow())
      }
    })

    expect(fetch).toHaveBeenCalledTimes(11)
    finishFetches.forEach((finish) => finish({ ok: true, json: async () => ({ version: '1.0.0' }) }))
    await act(async () => { await Promise.all(checks) })
    act(() => root.unmount())
  })

  it('keeps checking until the native local and remote probe queue drains', async () => {
    const previousSettings = useAppStore.getState().setUpdateSettings
    const provider = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '' }
    vi.stubGlobal('fetch', vi.fn(async (url: string) => ({ ok: true, json: async () => url.includes('api.github.com')
      ? { tag_name: 'V4.9.0', html_url: 'https://example.test/uam' } : { version: '1.0.0' } })))
    useAppStore.setState({ updateChecksEnabled: false, cliVersionManager: { providers: [provider] },
      setUpdateSettings: vi.fn(async () => true) })
    window.cefQuery = ({ onSuccess }) => {
      useAppStore.setState({ cliVersionManager: { providers: [{ ...provider, status: 'checking', running: true }] } })
      onSuccess('{}')
    }
    let result: ReturnType<typeof useUpdateMonitor> | undefined
    function Results() { result = useUpdateMonitor(); return null }
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<Results />))
    await act(async () => result!.checkNow())
    expect(result!.checking).toBe(true)
    await act(async () => result!.checkNow())
    expect(fetch).toHaveBeenCalledTimes(11)
    act(() => useAppStore.setState({ cliVersionManager: { providers: [provider], remoteProviders: [
      { ...provider, executionHostId: 'alpha', status: 'checking', running: true },
    ] } }))
    expect(result!.checking).toBe(true)
    act(() => useAppStore.setState({ cliVersionManager: { providers: [provider], remoteProviders: [
      { ...provider, executionHostId: 'alpha' },
    ] } }))
    expect(result!.checking).toBe(false)
    act(() => root.unmount())
    useAppStore.setState({ setUpdateSettings: previousSettings })
  })
})
