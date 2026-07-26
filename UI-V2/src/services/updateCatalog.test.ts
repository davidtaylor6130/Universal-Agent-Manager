import { describe, expect, it, vi } from 'vitest'
import { availableUpdates, compareVersions, fetchLatestUpdateCatalog } from './updateCatalog'

describe('update catalog', () => {
  it('compares normalized semantic versions', () => {
    expect(compareVersions('V4.2.0', '4.1.9')).toBeGreaterThan(0)
    expect(compareVersions('1.2', '1.2.0')).toBe(0)
    expect(compareVersions('0.9.9', '1.0.0')).toBeLessThan(0)
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
          { providerId: 'codex-cli', installedVersion: '0.124.0', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'supported', message: '', running: false, lastCommand: '', lastOutput: '' },
          { providerId: 'gemini-cli', installedVersion: '0.38.1', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'supported', message: '', running: false, lastCommand: '', lastOutput: '' },
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

  it('reports native-policy mismatches even when the installed runtime is newer than npm latest', () => {
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
          status: 'unsupported', message: '', running: false, lastCommand: '', lastOutput: '',
        }],
      },
      [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' }],
      {},
    )

    expect(updates).toHaveLength(1)
    expect(updates[0]).toMatchObject({ currentVersion: '0.140.0', latestVersion: '0.124.0', installable: true })
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
          status: 'supported',
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
    vi.unstubAllGlobals()
  })

  it('keeps successful provider results when another update service is unavailable', async () => {
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
    expect(catalog.uam.version).toBe('')
    expect(Object.keys(catalog.providers)).toHaveLength(5)
    vi.unstubAllGlobals()
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
    vi.unstubAllGlobals()
  })
})
