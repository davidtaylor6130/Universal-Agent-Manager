import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { useAppStore } from '../useAppStore'
import { sanitizeCppAppState, sanitizeVcsStorageSettings } from './sanitizers'

describe('native settings data actions', () => {
  const initial = useAppStore.getState()
  beforeEach(() => useAppStore.setState(initial, true))
  afterEach(() => { delete window.cefQuery; useAppStore.setState(initial, true) })

  it('preserves voice, scoped provider and VCS values on omitted full bridge fields', () => {
    useAppStore.setState({ voiceInputMode: 'server', voiceInputServerModel: 'fixture-model', providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'lab', enabled: false }], gitAutoWorktree: false, worktreeRetentionDays: 30, backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 })
    useAppStore.getState().loadFromCef(sanitizeCppAppState({ folders: [], chats: [], providers: [], settings: { theme: 'focus' } })!)
    expect(useAppStore.getState()).toMatchObject({ voiceInputMode: 'server', voiceInputServerModel: 'fixture-model', providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'lab', enabled: false }], gitAutoWorktree: false, worktreeRetentionDays: 30, backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 })
  })

  it('validates native VCS and storage integer boundaries without coercion', () => {
    expect(sanitizeVcsStorageSettings({ worktreeRetentionDays: 1, backupLogRetentionDays: 0, httpCacheClearIntervalDays: 3650, gitPortMethod: 'squash', gitDefaultBaseBranch: '' })).toEqual({ worktreeRetentionDays: 1, backupLogRetentionDays: 0, httpCacheClearIntervalDays: 3650, gitPortMethod: 'squash', gitDefaultBaseBranch: '' })
    expect(sanitizeVcsStorageSettings({ worktreeRetentionDays: 0, backupLogRetentionDays: 3651, httpCacheClearIntervalDays: '7', gitPortMethod: 'rebase', gitAutoWorktree: 'false', gitDefaultBaseBranch: 'main\nInjected' })).toEqual({})
  })

  it('sends optional VCS fields and rolls them back after native rejection', async () => {
    const previous = { gitEnabled: true, svnEnabled: false, gitAutoWorktree: true, gitPortMethod: 'patch' as const, worktreeRetentionDays: 14 }
    useAppStore.setState(previous)
    let request: unknown
    window.cefQuery = ({ request: raw, onFailure }) => { request = JSON.parse(raw); onFailure(400, 'Rejected') }
    await expect(useAppStore.getState().setVcsSettings({ gitEnabled: false, svnEnabled: true, gitAutoWorktree: false, gitPortMethod: 'merge', worktreeRetentionDays: 30 })).resolves.toBe(false)
    expect(request).toMatchObject({ action: 'setVcsSettings', payload: { gitEnabled: false, svnEnabled: true, gitAutoWorktree: false, gitPortMethod: 'merge', worktreeRetentionDays: 30 } })
    expect(useAppStore.getState()).toMatchObject(previous)
  })

  it('adopts the required storage pair only after durable native acceptance', async () => {
    useAppStore.setState({ backupLogRetentionDays: 14, httpCacheClearIntervalDays: 0 })
    let accept: (() => void) | undefined
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({ action: 'setStorageSettings', payload: { backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 } })
      accept = () => onSuccess('{}')
    }
    const pending = useAppStore.getState().setStorageSettings({ backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 })
    expect(useAppStore.getState()).toMatchObject({ backupLogRetentionDays: 14, httpCacheClearIntervalDays: 0 })
    accept!()
    await expect(pending).resolves.toBe(true)
    expect(useAppStore.getState()).toMatchObject({ backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 })
  })

  it('does not replace a newer native storage policy when an older request completes', async () => {
    useAppStore.setState({ backupLogRetentionDays: 14, httpCacheClearIntervalDays: 0 })
    let accept: (() => void) | undefined
    window.cefQuery = ({ onSuccess }) => { accept = () => onSuccess('{}') }
    const pending = useAppStore.getState().setStorageSettings({ backupLogRetentionDays: 0, httpCacheClearIntervalDays: 7 })
    useAppStore.setState({ backupLogRetentionDays: 30, httpCacheClearIntervalDays: 2 })
    accept!()
    await expect(pending).resolves.toBe(true)
    expect(useAppStore.getState()).toMatchObject({ backupLogRetentionDays: 30, httpCacheClearIntervalDays: 2 })
  })
  it('saves a canonical provider switch only for a known existing host after native acceptance', async () => {
    useAppStore.setState({ providerAvailability: [] })
    let accept: (() => void) | undefined
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({ action: 'setProviderAvailability', payload: { providerId: 'codex-cli', executionHostId: 'local', enabled: false } })
      accept = () => onSuccess('{}')
    }
    const pending = useAppStore.getState().setProviderAvailability({ providerId: 'codex', executionHostId: '', enabled: false })
    expect(useAppStore.getState().providerAvailability).toEqual([])
    accept!()
    await expect(pending).resolves.toBe(true)
    expect(useAppStore.getState().providerAvailability).toEqual([{ providerId: 'codex-cli', executionHostId: 'local', enabled: false }])
  })

  it('rejects unknown providers, missing hosts and nonboolean switches without sending requests', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    for (const payload of [{ providerId: 'unknown', executionHostId: 'local', enabled: false }, { providerId: 'codex', executionHostId: 'missing', enabled: false }, { providerId: 'codex', executionHostId: 'local', enabled: 'false' }]) {
      await expect(useAppStore.getState().setProviderAvailability(payload as never)).resolves.toBe(false)
    }
    expect(requests).toBe(0)
  })

  it('preserves pushed provider switches when an older save response arrives', async () => {
    let accept: (() => void) | undefined
    window.cefQuery = ({ onSuccess }) => { accept = () => onSuccess('{}') }
    const pending = useAppStore.getState().setProviderAvailability({ providerId: 'codex', executionHostId: 'local', enabled: false })
    const pushed = [{ providerId: 'claude-cli', executionHostId: 'local', enabled: false }]
    useAppStore.getState().loadFromCef(sanitizeCppAppState({ stateRevision: useAppStore.getState().lastAppliedStateRevision + 1, folders: [], chats: [], providers: [], settings: { providerAvailability: pushed } })!)
    accept!()
    await expect(pending).resolves.toBe(true)
    expect(useAppStore.getState().providerAvailability).toEqual(pushed)
  })

  it('maps voice saves to native keys without sending capabilities or secret values', async () => {
    const capabilities = { server: { supported: false, reason: 'Capture unavailable' } }
    useAppStore.setState({ voiceInputCapabilities: capabilities })
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({ action: 'setVoiceInputSettings', payload: { mode: 'server', serverBaseUrl: 'https://voice.example.test', serverEndpoint: '/v1/audio/transcriptions', serverModel: 'whisper-1', apiKeyEnv: 'VOICE_API_KEY' } })
      expect(Object.keys(JSON.parse(request).payload).sort()).toEqual(['apiKeyEnv', 'mode', 'serverBaseUrl', 'serverEndpoint', 'serverModel'])
      onSuccess('{}')
    }
    await expect(useAppStore.getState().setVoiceInputSettings({ mode: 'server', serverBaseUrl: 'https://voice.example.test', serverEndpoint: '/v1/audio/transcriptions', serverModel: 'whisper-1', apiKeyEnv: 'VOICE_API_KEY' })).resolves.toBe(true)
    expect(useAppStore.getState()).toMatchObject({ voiceInputMode: 'server', voiceInputApiKeyEnv: 'VOICE_API_KEY', voiceInputCapabilities: capabilities })
  })

  it('rejects malformed voice requests and incomplete server settings before native submission', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    for (const payload of [{ mode: 'unknown' }, { mode: 'system', serverModel: 42 }, { mode: 'server' }, { mode: 'system', apiKey: 'secret' }, { mode: 'system', serverBaseUrl: 'http://remote.example.test' }]) {
      await expect(useAppStore.getState().setVoiceInputSettings(payload as never)).resolves.toBe(false)
    }
    expect(requests).toBe(0)
  })

  it('preserves newer voice settings after delayed acceptance and leaves rejected saves unchanged', async () => {
    let accept: (() => void) | undefined
    window.cefQuery = ({ onSuccess }) => { accept = () => onSuccess('{}') }
    const pending = useAppStore.getState().setVoiceInputSettings({ mode: 'local' })
    useAppStore.setState({ voiceInputMode: 'server' })
    accept!()
    await expect(pending).resolves.toBe(true)
    expect(useAppStore.getState().voiceInputMode).toBe('server')
    window.cefQuery = ({ onFailure }) => onFailure(500, 'Save failed')
    await expect(useAppStore.getState().setVoiceInputSettings({ mode: 'system' })).resolves.toBe(false)
    expect(useAppStore.getState().voiceInputMode).toBe('server')
  })

  it('merges independent provider saves into current overrides when acknowledgements arrive out of order', async () => {
    useAppStore.setState({ providerAvailability: [] })
    const accepts: Array<() => void> = []
    window.cefQuery = ({ onSuccess }) => { accepts.push(() => onSuccess('{}')) }
    const codex = useAppStore.getState().setProviderAvailability({ providerId: 'codex', executionHostId: 'local', enabled: false })
    const gemini = useAppStore.getState().setProviderAvailability({ providerId: 'gemini', executionHostId: 'local', enabled: false })
    accepts[1]()
    await expect(gemini).resolves.toBe(true)
    accepts[0]()
    await expect(codex).resolves.toBe(true)
    expect(useAppStore.getState().providerAvailability).toEqual([
      { providerId: 'gemini-cli', executionHostId: 'local', enabled: false },
      { providerId: 'codex-cli', executionHostId: 'local', enabled: false },
    ])
  })

  it('keeps the newer same-key save when the older disable acknowledgement arrives last', async () => {
    useAppStore.setState({ providerAvailability: [] })
    const accepts: Array<() => void> = []
    window.cefQuery = ({ onSuccess }) => { accepts.push(() => onSuccess('{}')) }
    const older = useAppStore.getState().setProviderAvailability({ providerId: 'codex', executionHostId: 'local', enabled: false })
    const newer = useAppStore.getState().setProviderAvailability({ providerId: 'codex', executionHostId: 'local', enabled: true })
    accepts[1]()
    await expect(newer).resolves.toBe(true)
    accepts[0]()
    await expect(older).resolves.toBe(true)
    expect(useAppStore.getState().providerAvailability).toEqual([])
  })

})
