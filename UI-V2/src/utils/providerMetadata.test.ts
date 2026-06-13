import { describe, expect, it } from 'vitest'
import { buildProviderCliInstallCommand, CODEX_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, fallbackProviderForId, normalizeCliProviderIdAlias, OPENCODE_CLI_PROVIDER_ID, providerMetadataForId, providerNpmPackageName, providerRuntimeDescription, providerRuntimeKindLabel, providerShortName, providerUsesProtocol } from './providerMetadata'

describe('providerMetadata', () => {
  it('returns known provider labels and structured protocols', () => {
    expect(providerMetadataForId('claude-cli')).toEqual({
      id: 'claude-cli',
      name: 'Claude Code',
      shortName: 'Claude',
      structuredProtocol: 'claude-code-stream-json',
      runtimeDescription: 'Claude stream + CLI',
      npmPackage: '@anthropic-ai/claude-code',
    })
    expect(fallbackProviderForId('opencode-cli')).toMatchObject({
      id: 'opencode-cli',
      name: 'OpenCode',
      shortName: 'OpenCode',
      outputMode: 'cli',
      supportsStructured: true,
      structuredProtocol: 'opencode-acp',
    })
  })

  it('normalizes known provider aliases before metadata lookup', () => {
    expect(normalizeCliProviderIdAlias(' OpenCode ')).toBe('opencode-cli')
    expect(normalizeCliProviderIdAlias(' open-code ')).toBe('opencode-cli')
    expect(normalizeCliProviderIdAlias(' Claude-Code ')).toBe('claude-cli')
    expect(providerMetadataForId('codex')).toMatchObject({ id: 'codex-cli', shortName: 'Codex' })
    expect(providerUsesProtocol(undefined, ' CoDeX ', 'codex-cli')).toBe(true)
  })

  it('prefers provider display names and falls back to Gemini metadata for unknown ids', () => {
    expect(providerShortName({ id: 'custom', name: 'Custom Provider', shortName: '', color: '', description: '' }, 'custom')).toBe('Custom Provider')
    expect(providerMetadataForId('unknown-provider')).toEqual({
      id: 'unknown-provider',
      name: 'Gemini CLI',
      shortName: 'Gemini',
      structuredProtocol: 'gemini-acp',
      runtimeDescription: 'Gemini ACP + CLI',
      npmPackage: '@google/gemini-cli',
    })
  })

  it('matches provider protocols and runtime descriptions from ids or protocol fields', () => {
    expect(providerUsesProtocol(undefined, 'codex-cli', 'codex-cli')).toBe(true)
    expect(providerUsesProtocol({ id: 'custom', name: '', shortName: '', color: '', description: '', structuredProtocol: 'copilot-acp' }, 'custom', 'copilot-cli')).toBe(true)
    expect(providerRuntimeDescription({ id: 'custom', name: '', shortName: '', color: '', description: '', structuredProtocol: 'claude-code-stream-json' }, 'custom')).toBe('Claude stream + CLI')
    expect(providerRuntimeKindLabel({ id: 'codex-cli', name: '', shortName: '', color: '', description: '', structuredProtocol: 'codex-app-server' })).toBe('App Server')
    expect(providerRuntimeKindLabel(undefined, 'none')).toBe('CLI')
  })

  it('builds provider CLI install commands from provider metadata', () => {
    expect(providerNpmPackageName('opencode-cli')).toBe('opencode-ai')
    expect(buildProviderCliInstallCommand('copilot-cli', 'latest')).toBe('npm install -g @github/copilot@latest')
    expect(buildProviderCliInstallCommand('unknown-provider', '0.38.1')).toBe('npm install -g @google/gemini-cli@0.38.1')
  })

  it('fallback table provider ids match the canonical five CLI providers (FE-1 parity)', () => {
    const expectedIds = [DEFAULT_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID]
    for (const id of expectedIds) {
      expect(providerMetadataForId(id)).toBeDefined()
      expect(providerMetadataForId(id).id).toBe(id)
      expect(providerMetadataForId(id).npmPackage).toBeTruthy()
    }
  })

  it('normalization aliases resolve to the same canonical ids in both directions', () => {
    const aliases: [string, string][] = [
      ['gemini', DEFAULT_PROVIDER_ID],
      ['codex', CODEX_CLI_PROVIDER_ID],
      ['claude', CLAUDE_CLI_PROVIDER_ID],
      ['claude-code', CLAUDE_CLI_PROVIDER_ID],
      ['opencode', OPENCODE_CLI_PROVIDER_ID],
      ['open-code', OPENCODE_CLI_PROVIDER_ID],
      ['copilot', COPILOT_CLI_PROVIDER_ID],
      ['github-copilot', COPILOT_CLI_PROVIDER_ID],
    ]
    for (const [alias, canonical] of aliases) {
      expect(normalizeCliProviderIdAlias(alias)).toBe(canonical)
      expect(providerMetadataForId(alias).id).toBe(canonical)
    }
  })
})
