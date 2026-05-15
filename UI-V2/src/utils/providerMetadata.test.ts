import { describe, expect, it } from 'vitest'
import { buildProviderCliInstallCommand, fallbackProviderForId, normalizeCliProviderIdAlias, providerMetadataForId, providerNpmPackageName, providerRuntimeDescription, providerRuntimeKindLabel, providerShortName, providerUsesProtocol } from './providerMetadata'

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
})
