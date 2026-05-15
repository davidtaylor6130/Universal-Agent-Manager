import type { Provider } from '../types/provider'

export const DEFAULT_PROVIDER_ID = 'gemini-cli'
export const CODEX_CLI_PROVIDER_ID = 'codex-cli'
export const CLAUDE_CLI_PROVIDER_ID = 'claude-cli'
export const OPENCODE_CLI_PROVIDER_ID = 'opencode-cli'
export const COPILOT_CLI_PROVIDER_ID = 'copilot-cli'

export interface ProviderMetadata {
  id: string
  name: string
  shortName: string
  structuredProtocol: string
  runtimeDescription: string
  npmPackage: string
}

const PROVIDER_METADATA_BY_ID: Record<string, ProviderMetadata> = {
  [DEFAULT_PROVIDER_ID]: {
    id: DEFAULT_PROVIDER_ID,
    name: 'Gemini CLI',
    shortName: 'Gemini',
    structuredProtocol: 'gemini-acp',
    runtimeDescription: 'Gemini ACP + CLI',
    npmPackage: '@google/gemini-cli',
  },
  [CODEX_CLI_PROVIDER_ID]: {
    id: CODEX_CLI_PROVIDER_ID,
    name: 'Codex CLI',
    shortName: 'Codex',
    structuredProtocol: 'codex-app-server',
    runtimeDescription: 'Codex app-server + CLI',
    npmPackage: '@openai/codex',
  },
  [CLAUDE_CLI_PROVIDER_ID]: {
    id: CLAUDE_CLI_PROVIDER_ID,
    name: 'Claude Code',
    shortName: 'Claude',
    structuredProtocol: 'claude-code-stream-json',
    runtimeDescription: 'Claude stream + CLI',
    npmPackage: '@anthropic-ai/claude-code',
  },
  [OPENCODE_CLI_PROVIDER_ID]: {
    id: OPENCODE_CLI_PROVIDER_ID,
    name: 'OpenCode',
    shortName: 'OpenCode',
    structuredProtocol: 'opencode-acp',
    runtimeDescription: 'OpenCode ACP + CLI',
    npmPackage: 'opencode-ai',
  },
  [COPILOT_CLI_PROVIDER_ID]: {
    id: COPILOT_CLI_PROVIDER_ID,
    name: 'GitHub Copilot CLI',
    shortName: 'Copilot',
    structuredProtocol: 'copilot-acp',
    runtimeDescription: 'Copilot ACP + CLI',
    npmPackage: '@github/copilot',
  },
}

const GEMINI_METADATA = PROVIDER_METADATA_BY_ID[DEFAULT_PROVIDER_ID]

const PROVIDER_ID_ALIASES: Record<string, string> = {
  gemini: DEFAULT_PROVIDER_ID,
  [DEFAULT_PROVIDER_ID]: DEFAULT_PROVIDER_ID,
  codex: CODEX_CLI_PROVIDER_ID,
  [CODEX_CLI_PROVIDER_ID]: CODEX_CLI_PROVIDER_ID,
  claude: CLAUDE_CLI_PROVIDER_ID,
  'claude-code': CLAUDE_CLI_PROVIDER_ID,
  [CLAUDE_CLI_PROVIDER_ID]: CLAUDE_CLI_PROVIDER_ID,
  opencode: OPENCODE_CLI_PROVIDER_ID,
  'open-code': OPENCODE_CLI_PROVIDER_ID,
  [OPENCODE_CLI_PROVIDER_ID]: OPENCODE_CLI_PROVIDER_ID,
  copilot: COPILOT_CLI_PROVIDER_ID,
  'github-copilot': COPILOT_CLI_PROVIDER_ID,
  [COPILOT_CLI_PROVIDER_ID]: COPILOT_CLI_PROVIDER_ID,
}

export function normalizeCliProviderIdAlias(providerId: string): string {
  const trimmed = providerId.trim()
  if (!trimmed) return ''
  return PROVIDER_ID_ALIASES[trimmed.toLowerCase()] ?? trimmed
}

export function providerMetadataForId(providerId: string): ProviderMetadata {
  const normalizedProviderId = normalizeCliProviderIdAlias(providerId)
  return PROVIDER_METADATA_BY_ID[normalizedProviderId] ?? {
    ...GEMINI_METADATA,
    id: normalizedProviderId || GEMINI_METADATA.id,
  }
}

export function fallbackProviderForId(providerId: string): Provider {
  const metadata = providerMetadataForId(providerId)
  return {
    id: metadata.id,
    name: metadata.name,
    shortName: metadata.shortName,
    color: '#8ab4ff',
    description: '',
    outputMode: 'cli',
    supportsCli: true,
    supportsStructured: true,
    structuredProtocol: metadata.structuredProtocol,
  }
}

export function providerShortName(provider?: Provider, fallbackId = ''): string {
  if (provider?.shortName?.trim()) return provider.shortName.trim()
  if (provider?.name?.trim()) return provider.name.trim()
  return providerMetadataForId(fallbackId).shortName
}

export function providerRuntimeDescription(provider?: Provider, fallbackId = ''): string {
  const protocol = provider?.structuredProtocol?.trim()
  const metadata = protocol
    ? Object.values(PROVIDER_METADATA_BY_ID).find((candidate) => candidate.structuredProtocol === protocol)
    : undefined
  return metadata?.runtimeDescription ?? providerMetadataForId(fallbackId).runtimeDescription
}

export function providerRuntimeKindLabel(provider?: Provider, protocolKind = ''): string {
  const protocol = protocolKind || provider?.structuredProtocol || providerMetadataForId(DEFAULT_PROVIDER_ID).structuredProtocol
  if (protocol === providerMetadataForId(CODEX_CLI_PROVIDER_ID).structuredProtocol) return 'App Server'
  if (protocol === providerMetadataForId(CLAUDE_CLI_PROVIDER_ID).structuredProtocol) return 'Claude Stream'
  if (protocol === 'none') return 'CLI'
  return 'ACP'
}

export function providerUsesProtocol(provider: Provider | undefined, providerId: string, fallbackProviderId: string): boolean {
  const metadata = providerMetadataForId(fallbackProviderId)
  return normalizeCliProviderIdAlias(providerId) === metadata.id || provider?.structuredProtocol === metadata.structuredProtocol
}

export function providerNpmPackageName(providerId: string): string {
  return providerMetadataForId(providerId).npmPackage
}

export function buildProviderCliInstallCommand(providerId: string, version: string): string {
  return `npm install -g ${providerNpmPackageName(providerId)}@${version}`
}
