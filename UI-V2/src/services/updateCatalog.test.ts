import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { UPDATE_REQUEST_TIMEOUT_MS, availableRemoteHelperUpdates, availableUpdates, compareVersions, fetchLatestUpdateCatalog, readCachedUpdateCatalog } from './updateCatalog'

describe('update catalog', () => {
  beforeEach(() => {
    const values = new Map<string, string>()
    Object.defineProperty(window, 'localStorage', { configurable: true, value: {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
      removeItem: (key: string) => values.delete(key),
    } })
  })

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

  it('fetches provider notes for exact npm and Homebrew versions and projects the selected method', async () => {
    const noteUrls: string[] = []
    vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input)
      if (url.includes('/releases/tags/')) {
        noteUrls.push(url)
        const tag = decodeURIComponent(url.split('/').at(-1)!)
        return { ok: true, json: async () => ({ tag_name: tag, body: `# Notes\n- **Fixed** ${tag}` }) }
      }
      return { ok: true, json: async () => url.includes('api.github.com')
        ? { tag_name: 'v1.0.0' }
        : { version: url.includes('formula/gemini-cli') ? '1.8.0' : '2.0.0' } }
    }))
    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.providers['codex-cli']).toMatchObject({ releaseNotesExcerpt: 'Notes Fixed rust-v2.0.0' })
    expect(catalog.providers['gemini-cli'].homebrew).toMatchObject({ releaseNotesExcerpt: 'Notes Fixed v1.8.0' })
    expect(noteUrls.filter((url) => url.includes('/openai/codex/'))).toHaveLength(1)
    expect(noteUrls).toHaveLength(6)
    expect(readCachedUpdateCatalog()?.providers).toEqual(catalog.providers)
    const installed = { installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '' }
    const updates = availableUpdates(catalog, '1.0.0', {
      providers: [{ ...installed, providerId: 'codex-cli', installMethod: 'npm' }],
      remoteProviders: [{ ...installed, providerId: 'gemini-cli', installMethod: 'homebrew-formula', executionHostId: 'lab' }],
    }, [], {})
    expect(updates).toEqual([
      expect.objectContaining({ providerId: 'codex-cli', releaseNotesExcerpt: 'Notes Fixed rust-v2.0.0' }),
      expect.objectContaining({ providerId: 'gemini-cli', executionHostId: 'lab', latestVersion: '1.8.0', releaseNotesExcerpt: 'Notes Fixed v1.8.0' }),
    ])
  })

  it('keeps provider update versions when their notes are missing, malformed or for another tag', async () => {
    for (const mode of ['missing', 'malformed', 'mismatched', 'offline']) {
      window.localStorage.removeItem('uam-update-catalog-v1')
      vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => {
        const url = String(input)
        if (url.includes('/releases/tags/')) {
          if (mode === 'offline') throw new Error('offline')
          return { ok: mode !== 'missing', json: async () => ({
            tag_name: mode === 'mismatched' ? 'v9.0.0' : decodeURIComponent(url.split('/').at(-1)!),
            body: mode === 'malformed' ? { text: 'Invalid body' } : 'Wrong or unavailable notes',
          }) }
        }
        return { ok: true, json: async () => url.includes('api.github.com') ? { tag_name: 'v1.0.0' } : { version: '2.0.0' } }
      }))
      const catalog = await fetchLatestUpdateCatalog()
      expect(catalog.providers['codex-cli'].version).toBe('2.0.0')
      expect(catalog.providers['codex-cli']).not.toHaveProperty('releaseNotesExcerpt')
      expect(catalog.providers['codex-cli'].homebrew).not.toHaveProperty('releaseNotesExcerpt')
    }
  })

  it('sanitizes optional provider cache notes and preserves version-specific entries', () => {
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-10-14T00:00:00Z', uam: { version: '1.0.0', url: '' },
      providers: { 'codex-cli': { version: '2.0.0', url: '', releaseNotesExcerpt: '<script>unsafe()</script>' + 'A'.repeat(1_000),
        homebrew: { version: '1.9.0', url: '', releaseNotesExcerpt: { text: 'Invalid body' } } } },
    }))
    const cached = readCachedUpdateCatalog()
    expect(cached?.providers['codex-cli']).toMatchObject({ version: '2.0.0', releaseNotesExcerpt: 'A'.repeat(500) })
    expect(cached?.providers['codex-cli'].homebrew).toEqual({ version: '1.9.0', url: '' })
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-10-14T00:00:00Z', uam: { version: '1.0.0', url: '' },
      providers: { 'codex-cli': { version: '3.0.0', url: '' } },
    }))
    expect(readCachedUpdateCatalog()?.providers['codex-cli']).toEqual({ version: '3.0.0', url: '' })
  })

  it('bounds provider notes by the shared deadline without losing successful version checks', async () => {
    vi.useFakeTimers()
    const pendingNotes: AbortSignal[] = []
    vi.stubGlobal('fetch', vi.fn((input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input)
      if (url.includes('/releases/tags/')) {
        pendingNotes.push(init!.signal!)
        return new Promise<Response>((_resolve, reject) => init!.signal!.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError'))))
      }
      return new Promise((resolve) => window.setTimeout(() => resolve({ ok: true, json: async () =>
        url.includes('api.github.com') ? { tag_name: 'v1.0.0' } : { version: '2.0.0' } }), 10_000))
    }))
    const result = fetchLatestUpdateCatalog()
    await vi.advanceTimersByTimeAsync(10_000)
    expect(pendingNotes).toHaveLength(5)
    await vi.advanceTimersByTimeAsync(UPDATE_REQUEST_TIMEOUT_MS - 10_000)
    const catalog = await result
    expect(pendingNotes.every((signal) => signal.aborted)).toBe(true)
    expect(catalog.providers['codex-cli'].version).toBe('2.0.0')
    expect(catalog.providers['codex-cli']).not.toHaveProperty('releaseNotesExcerpt')
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

  it.each([undefined, null, 123, { text: 'Unexpected object' }, ['Unexpected array'], '   '])('omits missing or malformed release bodies (%j)', async (body) => {
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-07-13T00:00:00.000Z', uam: { version: '4.1.0', url: '', releaseNotesExcerpt: 'Older release notes.' }, providers: {},
    }))
    vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => ({
      ok: true,
      json: async () => String(input).includes('api.github.com')
        ? { tag_name: 'V4.2.0', html_url: 'https://example.test/release', body }
        : { version: '1.0.0' },
    })))
    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.uam.version).toBe('V4.2.0')
    expect(catalog.uam).not.toHaveProperty('releaseNotesExcerpt')
  })

  it('normalizes release text, caches it, and projects it only onto the UAM update', async () => {
    vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => ({
      ok: true,
      json: async () => String(input).includes('api.github.com')
        ? { tag_name: 'V4.2.0', html_url: 'https://example.test/release', body: '# Fixes\r\n- **Faster** [startup](https://example.test).\n<p>Stable `sessions`.</p><script>unsafe()</script>\u0000' }
        : { version: '2.0.0' },
    })))
    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.uam.releaseNotesExcerpt).toBe('Fixes Faster startup. Stable sessions.')
    expect(readCachedUpdateCatalog()?.uam).toEqual(catalog.uam)
    const updates = availableUpdates(catalog, '4.1.0', {
      providers: [{ providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }],
    }, [], {})
    expect(updates[0].releaseNotesExcerpt).toBe(catalog.uam.releaseNotesExcerpt)
    expect(updates[1]).not.toHaveProperty('releaseNotesExcerpt')
    expect(fetch).toHaveBeenCalledTimes(16)
  })

  it('bounds oversized release bodies before caching and projection', async () => {
    vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => ({
      ok: true,
      json: async () => String(input).includes('api.github.com')
        ? { tag_name: 'V4.2.0', body: 'A'.repeat(20_000) }
        : { version: '1.0.0' },
    })))
    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.uam.releaseNotesExcerpt).toBe('A'.repeat(500))
    expect(readCachedUpdateCatalog()?.uam.releaseNotesExcerpt).toHaveLength(500)
    expect(availableUpdates(catalog, '4.1.0', { providers: [] }, [], {})[0].releaseNotesExcerpt).toHaveLength(500)
  })

  it('accepts legacy cache entries and ignores malformed optional excerpts', () => {
    const legacy = { checkedAt: '2026-07-13T00:00:00.000Z', uam: { version: '4.2.0', url: 'https://example.test/uam' }, providers: {} }
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify(legacy))
    expect(readCachedUpdateCatalog()).toEqual(legacy)
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({ ...legacy, uam: { ...legacy.uam, releaseNotesExcerpt: { text: 'Invalid' } } }))
    expect(readCachedUpdateCatalog()).toEqual(legacy)
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({ ...legacy, uam: { ...legacy.uam, releaseNotesExcerpt: 'B'.repeat(1_000) } }))
    expect(readCachedUpdateCatalog()?.uam.releaseNotesExcerpt).toBe('B'.repeat(500))
  })

  it('retains cached notes with their version when GitHub is unavailable', async () => {
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-07-13T00:00:00.000Z', uam: { version: '4.2.0', url: 'https://example.test/uam', releaseNotesExcerpt: 'Existing fixes.' }, providers: {},
    }))
    vi.stubGlobal('fetch', vi.fn(async (input: RequestInfo | URL) => ({
      ok: !String(input).includes('api.github.com'),
      json: async () => ({ version: '1.0.0' }),
    })))
    expect((await fetchLatestUpdateCatalog()).uam).toMatchObject({ version: '4.2.0', releaseNotesExcerpt: 'Existing fixes.' })
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

  it('keeps local and remote updates for the same provider independently dismissible', () => {
    const state = { providerId: 'codex-cli', installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest', status: 'verified' as const, message: '', running: false, lastCommand: '', lastOutput: '' }
    const manager = { providers: [state], remoteProviders: [
      { ...state, executionHostId: 'alpha', executionHostName: 'Alpha' },
      { ...state, executionHostId: 'beta', executionHostName: 'Beta' },
    ] }
    const catalog = { checkedAt: '2026-09-01T00:00:00Z', uam: { version: '1.0.0', url: '' }, providers: { 'codex-cli': { version: '2.0.0', url: '' } } }
    const updates = availableUpdates(catalog, '1.0.0', manager, [], {})
    expect(updates.map((update) => update.id)).toEqual(['codex-cli', JSON.stringify(['alpha', 'codex-cli']), JSON.stringify(['beta', 'codex-cli'])])
    expect(updates[1]).toMatchObject({ name: 'codex-cli · Alpha', executionHostId: 'alpha', installable: true })
    expect(updates[1].remoteHostId).toBeUndefined()
    expect(availableUpdates(catalog, '1.0.0', manager, [], { [updates[1].id]: '2.0.0' }).map((update) => update.id)).toEqual([updates[0].id, updates[2].id])
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
    expect(fetch).toHaveBeenCalledTimes(17)
  })

  it('keeps independent provider releases when GitHub is unavailable', async () => {
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

    const catalog = await fetchLatestUpdateCatalog()
    expect(catalog.providers['gemini-cli'].version).toBeTruthy()
    expect(catalog.providers['codex-cli'].version).toBe('')
    expect(catalog.providers['codex-cli'].homebrew?.version).toBeTruthy()
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
