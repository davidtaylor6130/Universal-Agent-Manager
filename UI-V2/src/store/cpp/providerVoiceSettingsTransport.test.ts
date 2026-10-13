import { describe, expect, it } from 'vitest'
import { sanitizeCppSettings, sanitizeCppStatePatch, sanitizeProviderAvailability, sanitizeVoiceInputSettings } from './sanitizers'

const voice = {
  voiceInputMode: 'server', voiceInputServerBaseUrl: 'https://voice.example.test',
  voiceInputServerEndpoint: '/v1/audio/transcriptions', voiceInputServerModel: 'whisper-1', voiceInputApiKeyEnv: 'VOICE_API_KEY',
  voiceInputCapabilities: { system: { supported: true, reason: '' }, local: { supported: false, reason: 'Not supported' }, server: { supported: false, reason: 'Capture unavailable on this platform' } },
}

describe('provider and voice settings transport', () => {
  it('retains native scoped switches, server fields and explicit unsupported capabilities through round trips', () => {
    const settings = sanitizeCppSettings({ ...voice, providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', enabled: false }] })
    expect(settings).toMatchObject(voice)
    expect(settings.providerAvailability).toEqual([{ providerId: 'codex-cli', executionHostId: 'ssh-lab', enabled: false }])
    expect(sanitizeCppSettings(settings)).toMatchObject(voice)
    expect(sanitizeVoiceInputSettings({ voiceInputMode: 'local' })).toEqual({ voiceInputMode: 'local' })
    expect(sanitizeVoiceInputSettings({ voiceInputApiKey: 'actual-secret' })).toEqual({})
  })

  it('validates provider aliases and rejects malformed sets without turning a disabled override on', () => {
    expect(sanitizeProviderAvailability([{ providerId: 'codex', executionHostId: '', enabled: false }])).toEqual([{ providerId: 'codex-cli', executionHostId: 'local', enabled: false }])
    expect(sanitizeProviderAvailability([{ providerId: 'codex', executionHostId: 'lab', enabled: true }, { providerId: 'codex-cli', executionHostId: 'lab', enabled: false }])).toEqual([{ providerId: 'codex-cli', executionHostId: 'lab', enabled: false }])
    for (const invalid of [{ providerId: 'unknown', executionHostId: 'lab', enabled: false }, { providerId: 'codex', executionHostId: '../lab', enabled: false }, { providerId: 'codex', executionHostId: 'lab', enabled: 'false' }]) {
      expect(sanitizeProviderAvailability([{ providerId: 'codex', executionHostId: 'lab', enabled: false }, invalid])).toBeUndefined()
    }
    expect(sanitizeProviderAvailability(Array.from({ length: 4097 }, () => ({ providerId: 'codex', executionHostId: 'lab', enabled: false })))).toBeUndefined()
    expect(sanitizeProviderAvailability([])).toEqual([])
  })

  it('rejects unsafe voice fields and byte-limit violations without inferring platform support', () => {
    expect(sanitizeVoiceInputSettings({ ...voice, voiceInputServerBaseUrl: 'http://voice.example.test', voiceInputServerEndpoint: '/v1/audio\r\nInjected', voiceInputServerModel: 'é'.repeat(129), voiceInputApiKeyEnv: 'actual-secret-token' })).toEqual({ voiceInputMode: 'server', voiceInputCapabilities: voice.voiceInputCapabilities })
    const invalid = sanitizeVoiceInputSettings({ voiceInputMode: 'unknown', voiceInputServerEndpoint: '//other-host', voiceInputServerModel: 'x\0bad', voiceInputApiKeyEnv: 'TOKEN\r\nInjected', voiceInputCapabilities: { server: { supported: 'true', reason: '' } } })
    expect(invalid).toEqual({})
    expect(sanitizeVoiceInputSettings({ voiceInputServerBaseUrl: 'http://localhost:8000', voiceInputApiKeyEnv: '', voiceInputServerModel: 'é'.repeat(128) })).toEqual({ voiceInputServerBaseUrl: 'http://localhost:8000', voiceInputApiKeyEnv: '', voiceInputServerModel: 'é'.repeat(128) })
    expect(sanitizeVoiceInputSettings({})).not.toHaveProperty('voiceInputCapabilities')
  })

  it('projects only supplied partial fields and preserves unrelated settings and rejected overrides', () => {
    const previous = sanitizeCppSettings({ ...voice, theme: 'midnight', memoryLevelDefault: 'readOnly', memoryEnabledDefault: true, gitEnabled: false, providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', enabled: false }] })
    const patch = sanitizeCppStatePatch({ stateRevision: 2, settings: { voiceInputServerModel: 'new-model' } })!
    expect(patch.settings).toEqual({ voiceInputServerModel: 'new-model' })
    expect({ ...previous, ...patch.settings }).toMatchObject({ theme: 'midnight', memoryLevelDefault: 'readOnly', gitEnabled: false, voiceInputServerModel: 'new-model', providerAvailability: previous.providerAvailability })
    const rejected = sanitizeCppStatePatch({ settings: { providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', enabled: 'false' }], voiceInputServerBaseUrl: 'http://remote.example.test', gitEnabled: 'true' } })!
    expect(rejected.settings).toEqual({})
    expect(sanitizeCppStatePatch({ settings: { providerAvailability: [] } })?.settings).toEqual({ providerAvailability: [] })
  })
})
