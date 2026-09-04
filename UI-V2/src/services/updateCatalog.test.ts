import { afterEach, describe, expect, it, vi } from 'vitest'
import { UPDATE_REQUEST_TIMEOUT_MS, availableRemoteHelperUpdates, availableUpdates, compareVersions, fetchLatestUpdateCatalog, readCachedUpdateCatalog } from './updateCatalog'

describe('update catalog', () => {
  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
  })

  it('compares normalized semantic versions', () => {
    expect(compareVersions('V4.2.0', '4.1.9')).toBeGreaterThan(0)
    expect(compareVersions('1.2', '1.2.0')).toBe(0)
    expect(compareVersions('0.9.9', '1.0.0')).toBeLessThan(0)
    expect(compareVersions('4.5.7-alpha2', '4.5.7')).toBeLessThan(0)
    expect(compareVersions('4.5.7-alpha.10', '4.5.7-alpha.2')).toBeGreaterThan(0)
    expect(compareVersions('4.8.0-alpha-2', '4.8.0-alpha')).toBeGreaterThan(0)
	expect(compareVersions('4.9.0-alpha-10', '4.9.0-alpha-9')).toBeGreaterThan(0)
  })

  it('reports an older SSH helper independently of the online catalog', () => {
    expect(availableRemoteHelperUpdates('4.8.0-alpha-2', 2, [
      { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
      { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
    ], {})).toEqual([expect.objectContaining({
      id: 'remote-helper-lab',
      remoteHostId: 'lab',
      currentVersion: '4.8.0-alpha',
      latestVersion: '4.8.0-alpha-2 · helper protocol 2',
    })])
  })

  it('reports an older hyphenated alpha SSH helper', () => {
    expect(availableRemoteHelperUpdates('4.9.0-alpha-11', 3, [
      { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '4.9.0-alpha-10', runnerProtocolVersion: 3, platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
    ], {})).toHaveLength(1)
  })

  it('reports a same-version or undetected helper with an obsolete protocol', () => {
    expect(availableRemoteHelperUpdates('4.8.0-alpha-2', 2, [
      { id: 'same', label: 'Same version', transport: 'ssh', sshAlias: 'same', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha-2', runnerProtocolVersion: 1, platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
      { id: 'missing', label: 'Undetected', transport: 'ssh', sshAlias: 'missing', runnerStatus: 'error', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
      { id: 'current', label: 'Current', transport: 'ssh', sshAlias: 'current', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha-2', runnerProtocolVersion: 2, platform: 'windows', architecture: 'x86_64', lastSeenAt: '' },
    ], {}).map((update) => update.id)).toEqual(['remote-helper-same', 'remote-helper-missing'])
  })

  it('rejects malformed cached catalog entries', () => {
    const values = new Map<string, string>()
    Object.defineProperty(window, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => values.get(key) ?? null,
        setItem: (key: string, value: string) => values.set(key, value),
      },
    })
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-07-13T00:00:00.000Z',
      uam: { version: '4.2.0' },
      providers: { 'codex-cli': { version: 130, url: 'https://example.test' } },
    }))
    expect(readCachedUpdateCatalog()).toBeNull()
  })

  it('aborts an update check that exceeds the request deadline', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('fetch', vi.fn((_input: RequestInfo | URL, init?: RequestInit) => new Promise<Response>((_resolve, reject) => {
      init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')))
    })))

    const result = expect(fetchLatestUpdateCatalog()).rejects.toThrow('Update check timed out.')
    await vi.advanceTimersByTimeAsync(UPDATE_REQUEST_TIMEOUT_MS)
    await result
  })

  it('reports UAM and installed provider updates while respecting dismissals', () => {
    const updates = availableUpdates(
      {
        checkedAt: '2026-07-13T00:00:00.000Z',
        uam: { version: 'V4.2.0', url: 'https://example.test/uam' },
        providers: {
          'codex-cli': { version: '0.130.0', url: 'https://example.test/codex' },
          'gemini-cli': { version: '0.40.0', url: 'https://example.test/gemini' },
        },
      },
      'V4.1.0',
      {
        providers: [
          { providerId: 'codex-cli', installedVersion: '0.124.0', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' },
          { providerId: 'gemini-cli', installedVersion: '0.38.1', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' },
        ],
      },
      [
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' },
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', description: '', color: '#fff' },
      ],
      { 'gemini-cli': '0.40.0' },
    )

    expect(updates.map((update) => update.id)).toEqual(['uam', 'codex-cli'])
    expect(updates[1]).toMatchObject({ currentVersion: '0.124.0', latestVersion: '0.130.0' })
  })

  it('never presents an older curated version as an update', () => {
    const updates = availableUpdates(
      {
        checkedAt: '2026-07-13T00:00:00.000Z',
        uam: { version: 'V4.1.0', url: 'https://example.test/uam' },
        providers: { 'codex-cli': { version: '0.130.0', url: 'https://example.test/codex' } },
      },
      'V4.1.0',
      {
        providers: [{
          providerId: 'codex-cli', installedVersion: '0.140.0', selectedVersion: '',
          availableVersions: [{ version: '0.124.0', preferred: true }], preferredVersion: '0.124.0',
          status: 'known-incompatible', message: '', running: false, lastCommand: '', lastOutput: '',
        }],
      },
      [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' }],
      {},
    )

    expect(updates).toEqual([])
  })

  it('uses the Homebrew release channel for Homebrew-managed providers', () => {
    const updates = availableUpdates(
      {
        checkedAt: '2026-07-25T00:00:00.000Z',
        uam: { version: 'V4.5.0', url: 'https://example.test/uam' },
        providers: {
          'opencode-cli': {
            version: '1.18.5',
            url: 'https://www.npmjs.com/package/opencode-ai',
            homebrew: { version: '1.18.0', url: 'https://formulae.brew.sh/formula/opencode' },
          },
        },
      },
      'V4.5.0',
      {
        providers: [{
          providerId: 'opencode-cli',
          installedVersion: '1.17.15',
          selectedVersion: '',
          availableVersions: [{ version: 'latest', preferred: true }],
          preferredVersion: 'latest',
          status: 'verified',
          message: '',
          running: false,
          lastCommand: '',
          lastOutput: '',
          installMethod: 'homebrew-formula',
          lastInstallStatus: 'none',
        }],
      },
      [{ id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', description: '', color: '#fff' }],
      {},
    )

    expect(updates).toEqual([expect.objectContaining({
      providerId: 'opencode-cli',
      latestVersion: '1.18.0',
      url: 'https://formulae.brew.sh/formula/opencode',
      installable: true,
    })])
  })

  it('fetches the GitHub release and all provider package releases', async () => {
    const responses = [
      { tag_name: 'V4.2.0', html_url: 'https://example.test/release' },
      { version: '0.40.0' },
      { version: '0.130.0' },
      { version: '2.1.0' },
      { version: '1.2.0' },
      { version: '0.0.400' },
      { versions: { stable: '0.40.0' } },
      { version: '0.130.0' },
      { version: '2.1.0' },
      { versions: { stable: '1.18.0' } },
      { version: '0.0.400' },
    ]
    vi.stubGlobal('fetch', vi.fn(async () => ({
      ok: true,
      status: 200,
      json: async () => responses.shift(),
    })))

    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.uam.version).toBe('V4.2.0')
    expect(catalog.providers['codex-cli'].version).toBe('0.130.0')
    expect(catalog.providers['opencode-cli'].homebrew?.version).toBe('1.18.0')
    expect(fetch).toHaveBeenCalledTimes(11)
  })

  it('does not report a successful catalog when the UAM release cannot be checked', async () => {
    let call = 0
    vi.stubGlobal('fetch', vi.fn(async () => {
      call += 1
      if (call === 1 || call === 3) throw new Error('offline')
      return {
        ok: true,
        status: 200,
        json: async () => ({ version: `1.0.${call}` }),
      }
    }))

    await expect(fetchLatestUpdateCatalog()).rejects.toThrow('UAM update service is currently unavailable.')
  })

  it('keeps successful results when one service returns invalid JSON', async () => {
    let call = 0
    vi.stubGlobal('fetch', vi.fn(async () => {
      const currentCall = ++call
      return {
        ok: true,
        status: 200,
        json: async () => {
          if (currentCall === 2) throw new SyntaxError('invalid JSON')
          return currentCall === 1
            ? { tag_name: 'V4.2.0', html_url: 'https://example.test/release' }
            : { version: `1.0.${currentCall}` }
        },
      }
    }))

    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.uam.version).toBe('V4.2.0')
    expect(Object.keys(catalog.providers)).toHaveLength(5)
  })
})
