import type { Provider } from '../types/provider'
import { ANTIGRAVITY_ENABLED, CLAUDE_ENABLED, CODEX_ENABLED, COPILOT_ENABLED, GEMINI_ENABLED, OPENCODE_ENABLED } from '../config/buildFeatures'

// ponytail: a provider excluded by its build flag gets an id no chat or setting can carry, so shared
// comparisons stay valid while the bundler drops the real id, metadata and every branch guarded by its flag.
const EXCLUDED_PROVIDER_ID = '\u0000'
export const GEMINI_CLI_PROVIDER_ID = GEMINI_ENABLED ? 'gemini-cli' : EXCLUDED_PROVIDER_ID
export const CODEX_CLI_PROVIDER_ID = CODEX_ENABLED ? 'codex-cli' : EXCLUDED_PROVIDER_ID
export const CLAUDE_CLI_PROVIDER_ID = CLAUDE_ENABLED ? 'claude-cli' : EXCLUDED_PROVIDER_ID
export const OPENCODE_CLI_PROVIDER_ID = OPENCODE_ENABLED ? 'opencode-cli' : EXCLUDED_PROVIDER_ID
export const COPILOT_CLI_PROVIDER_ID = COPILOT_ENABLED ? 'copilot-cli' : EXCLUDED_PROVIDER_ID
export const ANTIGRAVITY_CLI_PROVIDER_ID = ANTIGRAVITY_ENABLED ? 'antigravity-cli' : EXCLUDED_PROVIDER_ID
/** First provider in canonical order that this build includes (CMake requires at least one). */
export const DEFAULT_PROVIDER_ID = GEMINI_ENABLED ? GEMINI_CLI_PROVIDER_ID
  : CODEX_ENABLED ? CODEX_CLI_PROVIDER_ID
  : CLAUDE_ENABLED ? CLAUDE_CLI_PROVIDER_ID
  : OPENCODE_ENABLED ? OPENCODE_CLI_PROVIDER_ID
  : COPILOT_ENABLED ? COPILOT_CLI_PROVIDER_ID
  : ANTIGRAVITY_CLI_PROVIDER_ID

export interface ProviderCapabilities {
  supportsTextWorkers: boolean
  hasReasoningEffort: boolean
  hasServiceTier: boolean
  hasAcceptEditsMode: boolean
  structuredPermissionControl: 'uam' | 'provider'
  terminalPermissionControl: 'provider'
  usesFriendlyModelLabels: boolean
  showPlanActionButtons: boolean
  autoLabel: string
  acceptEditsLabel: string | undefined
  defaultModelLabels: Record<string, { label: string; shortLabel: string; detail: string }>
  memoryModelIds: string[]
  memoryModelLabels: Record<string, { label: string; detail: string }>
  memoryModelDefaultId: string
  reasoningOptions: { id: string; label: string; detail: string }[]
  speedOptions: { id: string; label: string; detail: string }[]
}

export interface ProviderMetadata {
  id: string
  name: string
  shortName: string
  structuredProtocol: string
  runtimeDescription: string
  npmPackage: string
  capabilities: ProviderCapabilities
}

export const GEMINI_DEFAULT_MODEL_LABELS: Record<string, { label: string; shortLabel: string; detail: string }> = !GEMINI_ENABLED ? {} : {
  '': { label: 'Default', shortLabel: 'Default', detail: 'Use Gemini settings' },
  'auto-gemini-3': { label: 'Auto 3', shortLabel: 'Auto 3', detail: 'Gemini 3 routing' },
  'auto-gemini-2.5': { label: 'Auto 2.5', shortLabel: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  pro: { label: 'Pro', shortLabel: 'Pro', detail: 'Prioritize capability' },
  flash: { label: 'Flash', shortLabel: 'Flash', detail: 'Prioritize speed' },
  'flash-lite': { label: 'Flash Lite', shortLabel: 'Flash Lite', detail: 'Fastest option' },
}

export const CODEX_REASONING_LABELS: Record<string, { label: string; shortLabel: string; detail: string }> = {
  '': { label: 'Default', shortLabel: 'Default', detail: 'Use provider default reasoning' },
  none: { label: 'None', shortLabel: 'None', detail: 'No extra reasoning' },
  minimal: { label: 'Minimal', shortLabel: 'Minimal', detail: 'Fastest reasoning' },
  low: { label: 'Low', shortLabel: 'Low', detail: 'Faster responses' },
  medium: { label: 'Medium', shortLabel: 'Medium', detail: 'Balanced reasoning' },
  high: { label: 'High', shortLabel: 'High', detail: 'Deeper reasoning' },
  xhigh: { label: 'Extra High', shortLabel: 'XHigh', detail: 'Very deep reasoning' },
  max: { label: 'Max', shortLabel: 'Max', detail: 'Maximum available reasoning' },
  ultra: { label: 'Ultra', shortLabel: 'Ultra', detail: 'Maximum reasoning with automatic delegation' },
}

export const CODEX_SPEED_LABELS: Record<string, { label: string; shortLabel: string; detail: string }> = {
  __uam_inherit__: { label: 'Inherit', shortLabel: 'Inherit', detail: 'Do not send a speed override' },
  '': { label: 'Standard', shortLabel: 'Standard', detail: 'Explicitly clear any provider speed override' },
  fast: { label: 'Fast', shortLabel: 'Fast', detail: 'Prioritize latency' },
  flex: { label: 'Flex', shortLabel: 'Flex', detail: 'Use flexible service tier' },
}

export const CODEX_REASONING_OPTIONS = [
  { id: '', label: 'Default', detail: CODEX_ENABLED ? 'Use Codex default reasoning' : 'Use provider default reasoning' },
  { id: 'minimal', label: 'Minimal', detail: 'Fastest reasoning' },
  { id: 'low', label: 'Low', detail: 'Faster responses' },
  { id: 'medium', label: 'Medium', detail: 'Balanced reasoning' },
  { id: 'high', label: 'High', detail: 'Deeper reasoning' },
  { id: 'xhigh', label: 'Extra High', detail: 'Very deep reasoning' },
  { id: 'max', label: 'Max', detail: 'Maximum available reasoning' },
  { id: 'ultra', label: 'Ultra', detail: 'Maximum reasoning with automatic delegation' },
]

export const COPILOT_REASONING_OPTIONS = !COPILOT_ENABLED ? [] : [
  { id: '', label: 'Default', detail: 'Use Copilot default reasoning' },
  { id: 'none', label: 'None', detail: 'No extra reasoning' },
  { id: 'minimal', label: 'Minimal', detail: 'Fastest reasoning' },
  { id: 'low', label: 'Low', detail: 'Faster responses' },
  { id: 'medium', label: 'Medium', detail: 'Balanced reasoning' },
  { id: 'high', label: 'High', detail: 'Deeper reasoning' },
  { id: 'xhigh', label: 'XHigh', detail: 'Very deep reasoning' },
  { id: 'max', label: 'Max', detail: 'Maximum available reasoning' },
]

export const CODEX_SPEED_OPTIONS = !CODEX_ENABLED ? [] : [
  { id: '', label: 'Default', detail: 'Use Codex default speed' },
  { id: 'fast', label: 'Fast', detail: 'Prioritize latency' },
  { id: 'flex', label: 'Flex', detail: 'Use flexible service tier' },
]

const GEMINI_MEMORY_MODEL_IDS = ['', 'auto-gemini-3', 'pro', 'flash', 'flash-lite']
const GEMINI_MEMORY_MODEL_LABELS: Record<string, { label: string; detail: string }> = {
  '': { label: 'Default', detail: 'Use Gemini settings' },
  'auto-gemini-3': { label: 'Auto 3', detail: 'Gemini 3 routing' },
  pro: { label: 'Pro', detail: 'Prioritize capability' },
  flash: { label: 'Flash', detail: 'Prioritize speed' },
  'flash-lite': { label: 'Flash Lite', detail: 'Fastest option' },
}
const CODEX_MEMORY_MODEL_IDS = ['', 'gpt-5.6', 'gpt-5.4', 'gpt-5.3', 'gpt-5.2', 'gpt-5.1']
const CODEX_MEMORY_MODEL_LABELS: Record<string, { label: string; detail: string }> = {
  '': { label: 'Default', detail: 'Use Codex settings' },
  'gpt-5.6': { label: 'GPT-5.6', detail: 'Latest frontier coding model' },
  'gpt-5.4': { label: 'GPT-5.4', detail: 'Frontier coding model' },
  'gpt-5.4-mini': { label: 'GPT-5.4 Mini', detail: 'Smaller fast model' },
  'gpt-5.2': { label: 'GPT-5.2', detail: 'Balanced coding model' },
}
const CLAUDE_MEMORY_MODEL_LABELS: Record<string, { label: string; detail: string }> = {
  '': { label: 'Default', detail: 'Use Claude Code settings' },
  sonnet: { label: 'Sonnet', detail: 'Latest Sonnet alias' },
  opus: { label: 'Opus', detail: 'Latest Opus alias' },
}
const GENERIC_MEMORY_MODEL_LABELS: Record<string, { label: string; detail: string }> = {
  '': { label: 'Default', detail: 'Use provider settings' },
}

const PROVIDER_METADATA_BY_ID: Record<string, ProviderMetadata> = {
  ...(GEMINI_ENABLED ? {
    [GEMINI_CLI_PROVIDER_ID]: {
      id: GEMINI_CLI_PROVIDER_ID,
      name: 'Gemini CLI',
      shortName: 'Gemini',
      structuredProtocol: 'gemini-acp',
      runtimeDescription: 'Gemini ACP + CLI',
      npmPackage: '@google/gemini-cli',
      capabilities: {
        supportsTextWorkers: true,
        hasReasoningEffort: false,
        hasServiceTier: false,
        hasAcceptEditsMode: true,
        structuredPermissionControl: 'uam',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: true,
        showPlanActionButtons: false,
        autoLabel: 'Yolo',
        acceptEditsLabel: 'Accept Edits',
        defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
        memoryModelIds: GEMINI_MEMORY_MODEL_IDS,
        memoryModelLabels: GEMINI_MEMORY_MODEL_LABELS,
        memoryModelDefaultId: '',
        reasoningOptions: [],
        speedOptions: [],
      },
    }
  } : {}),
  ...(CODEX_ENABLED ? {
    [CODEX_CLI_PROVIDER_ID]: {
      id: CODEX_CLI_PROVIDER_ID,
      name: 'Codex CLI',
      shortName: 'Codex',
      structuredProtocol: 'codex-app-server',
      runtimeDescription: 'Codex app-server + CLI',
      npmPackage: '@openai/codex',
      capabilities: {
        supportsTextWorkers: true,
        hasReasoningEffort: true,
        hasServiceTier: true,
        hasAcceptEditsMode: true,
        structuredPermissionControl: 'uam',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: false,
        showPlanActionButtons: true,
        autoLabel: 'Yolo',
        acceptEditsLabel: 'Accept Edits',
        defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
        memoryModelIds: CODEX_MEMORY_MODEL_IDS,
        memoryModelLabels: CODEX_MEMORY_MODEL_LABELS,
        memoryModelDefaultId: '',
        reasoningOptions: CODEX_REASONING_OPTIONS,
        speedOptions: CODEX_SPEED_OPTIONS,
      },
    }
  } : {}),
  ...(CLAUDE_ENABLED ? {
    [CLAUDE_CLI_PROVIDER_ID]: {
      id: CLAUDE_CLI_PROVIDER_ID,
      name: 'Claude Code',
      shortName: 'Claude',
      structuredProtocol: 'claude-code-stream-json',
      runtimeDescription: 'Claude stream + CLI',
      npmPackage: '@anthropic-ai/claude-code',
      capabilities: {
        supportsTextWorkers: true,
        hasReasoningEffort: false,
        hasServiceTier: true,
        hasAcceptEditsMode: true,
        structuredPermissionControl: 'uam',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: false,
        showPlanActionButtons: false,
        autoLabel: 'Auto',
        acceptEditsLabel: 'Accept Edits',
        defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
        memoryModelIds: ['', 'sonnet', 'opus'],
        memoryModelLabels: CLAUDE_MEMORY_MODEL_LABELS,
        memoryModelDefaultId: '',
        reasoningOptions: [],
        speedOptions: [],
      },
    }
  } : {}),
  ...(OPENCODE_ENABLED ? {
    [OPENCODE_CLI_PROVIDER_ID]: {
      id: OPENCODE_CLI_PROVIDER_ID,
      name: 'OpenCode',
      shortName: 'OpenCode',
      structuredProtocol: 'opencode-acp',
      runtimeDescription: 'OpenCode ACP + CLI',
      npmPackage: 'opencode-ai',
      capabilities: {
        supportsTextWorkers: true,
        hasReasoningEffort: false,
        hasServiceTier: false,
        hasAcceptEditsMode: true,
        structuredPermissionControl: 'uam',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: false,
        showPlanActionButtons: false,
        autoLabel: 'Yolo',
        acceptEditsLabel: 'Accept Edits',
        defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
        memoryModelIds: [''],
        memoryModelLabels: GENERIC_MEMORY_MODEL_LABELS,
        memoryModelDefaultId: '',
        reasoningOptions: [],
        speedOptions: [],
      },
    }
  } : {}),
  ...(COPILOT_ENABLED ? {
    [COPILOT_CLI_PROVIDER_ID]: {
      id: COPILOT_CLI_PROVIDER_ID,
      name: 'GitHub Copilot CLI',
      shortName: 'Copilot',
      structuredProtocol: 'copilot-sdk',
      runtimeDescription: 'Copilot SDK + CLI',
      npmPackage: '@github/copilot',
      capabilities: {
        supportsTextWorkers: true,
        hasReasoningEffort: true,
        hasServiceTier: false,
        hasAcceptEditsMode: true,
        structuredPermissionControl: 'uam',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: false,
        showPlanActionButtons: false,
        autoLabel: 'Yolo',
        acceptEditsLabel: 'Accept Edits',
        defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
        memoryModelIds: [''],
        memoryModelLabels: GENERIC_MEMORY_MODEL_LABELS,
        memoryModelDefaultId: '',
        reasoningOptions: COPILOT_REASONING_OPTIONS,
        speedOptions: [],
      },
    }
  } : {}),
  ...(ANTIGRAVITY_ENABLED ? {
    [ANTIGRAVITY_CLI_PROVIDER_ID]: {
      id: ANTIGRAVITY_CLI_PROVIDER_ID,
      name: 'Antigravity CLI',
      shortName: 'Antigravity',
      structuredProtocol: 'antigravity-stream-json',
      runtimeDescription: 'Antigravity Stream + CLI',
      npmPackage: '',
      capabilities: {
        supportsTextWorkers: false,
        hasReasoningEffort: true,
        hasServiceTier: false,
        hasAcceptEditsMode: false,
        structuredPermissionControl: 'provider',
        terminalPermissionControl: 'provider',
        usesFriendlyModelLabels: false,
        showPlanActionButtons: false,
        autoLabel: 'Auto',
        acceptEditsLabel: undefined,
        defaultModelLabels: {},
        memoryModelIds: [],
        memoryModelLabels: {},
        memoryModelDefaultId: '',
        reasoningOptions: [],
        speedOptions: [],
      },
    }
  } : {}),
}

export const SUPPORTED_CLI_PROVIDER_IDS = Object.keys(PROVIDER_METADATA_BY_ID)
export const SUPPORTED_TEXT_WORKER_PROVIDER_IDS = SUPPORTED_CLI_PROVIDER_IDS.filter((id) => PROVIDER_METADATA_BY_ID[id].capabilities.supportsTextWorkers)

export function providerSupportsTextWorkers(providerId: string): boolean {
  return providerMetadataForId(providerId).capabilities.supportsTextWorkers
}

/** Supported provider IDs in catalogue order, shared by provider selection controls. */

const DEFAULT_METADATA = PROVIDER_METADATA_BY_ID[DEFAULT_PROVIDER_ID]

const PROVIDER_ID_ALIASES: Record<string, string> = {
  ...(ANTIGRAVITY_ENABLED ? { agy: ANTIGRAVITY_CLI_PROVIDER_ID, antigravity: ANTIGRAVITY_CLI_PROVIDER_ID, [ANTIGRAVITY_CLI_PROVIDER_ID]: ANTIGRAVITY_CLI_PROVIDER_ID } : {}),
  ...(GEMINI_ENABLED ? { gemini: GEMINI_CLI_PROVIDER_ID, [GEMINI_CLI_PROVIDER_ID]: GEMINI_CLI_PROVIDER_ID } : {}),
  ...(CODEX_ENABLED ? { codex: CODEX_CLI_PROVIDER_ID, [CODEX_CLI_PROVIDER_ID]: CODEX_CLI_PROVIDER_ID } : {}),
  ...(CLAUDE_ENABLED ? { claude: CLAUDE_CLI_PROVIDER_ID, 'claude-code': CLAUDE_CLI_PROVIDER_ID, [CLAUDE_CLI_PROVIDER_ID]: CLAUDE_CLI_PROVIDER_ID } : {}),
  ...(OPENCODE_ENABLED ? { opencode: OPENCODE_CLI_PROVIDER_ID, 'open-code': OPENCODE_CLI_PROVIDER_ID, [OPENCODE_CLI_PROVIDER_ID]: OPENCODE_CLI_PROVIDER_ID } : {}),
  ...(COPILOT_ENABLED ? { copilot: COPILOT_CLI_PROVIDER_ID, 'github-copilot': COPILOT_CLI_PROVIDER_ID, [COPILOT_CLI_PROVIDER_ID]: COPILOT_CLI_PROVIDER_ID } : {}),
}

export function normalizeCliProviderIdAlias(providerId: string): string {
  const trimmed = providerId.trim()
  if (!trimmed) return ''
  return PROVIDER_ID_ALIASES[trimmed.toLowerCase()] ?? trimmed
}

export function providerMetadataForId(providerId: string): ProviderMetadata {
  const normalizedProviderId = normalizeCliProviderIdAlias(providerId)
  return PROVIDER_METADATA_BY_ID[normalizedProviderId] ?? {
    ...DEFAULT_METADATA,
    id: normalizedProviderId || DEFAULT_METADATA.id,
    capabilities: { ...DEFAULT_METADATA.capabilities },
  }
}

export function providerCapabilities(providerId: string, provider?: Provider): ProviderCapabilities {
  const metadata = providerMetadataForId(providerId)
  return provider ? {
    ...metadata.capabilities,
    structuredPermissionControl: provider.structuredPermissionControl ?? metadata.capabilities.structuredPermissionControl,
    terminalPermissionControl: 'provider',
  } : metadata.capabilities
}

export function capabilitiesWithDefaults(providerId: string, provider?: Provider, overrides?: Partial<ProviderCapabilities>): ProviderCapabilities {
  const caps = providerCapabilities(providerId, provider)
  return overrides ? { ...caps, ...overrides } : caps
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
    supportsStructured: metadata.structuredProtocol !== 'none',
    structuredProtocol: metadata.structuredProtocol,
  }
}

export function providerShortName(provider?: Provider, fallbackId = ''): string {
  if (provider?.shortName?.trim()) return provider.shortName.trim()
  if (provider?.name?.trim()) return provider.name.trim()
  return providerMetadataForId(fallbackId).shortName
}

export function providerRuntimeDescription(provider?: Provider, fallbackId = ''): string {
  const rawProtocol = provider?.structuredProtocol?.trim()
  const protocol = COPILOT_ENABLED && rawProtocol === 'copilot-acp' ? 'copilot-sdk' : rawProtocol
  const metadata = protocol
    ? Object.values(PROVIDER_METADATA_BY_ID).find((candidate) => candidate.structuredProtocol === protocol)
    : undefined
  return metadata?.runtimeDescription ?? providerMetadataForId(fallbackId).runtimeDescription
}

export function providerRuntimeKindLabel(provider?: Provider, protocolKind = ''): string {
  const protocol = protocolKind || provider?.structuredProtocol || providerMetadataForId(DEFAULT_PROVIDER_ID).structuredProtocol
  if (CODEX_ENABLED && protocol === providerMetadataForId(CODEX_CLI_PROVIDER_ID).structuredProtocol) return 'App Server'
  if (CLAUDE_ENABLED && protocol === providerMetadataForId(CLAUDE_CLI_PROVIDER_ID).structuredProtocol) return 'Claude Stream'
  if (ANTIGRAVITY_ENABLED && protocol === providerMetadataForId(ANTIGRAVITY_CLI_PROVIDER_ID).structuredProtocol) return 'Antigravity Stream'
  if (COPILOT_ENABLED && protocol === 'copilot-sdk') return 'Copilot SDK'
  if (protocol === 'none') return 'CLI'
  return 'ACP'
}

export function providerUsesProtocol(provider: Provider | undefined, providerId: string, fallbackProviderId: string): boolean {
  const metadata = providerMetadataForId(fallbackProviderId)
  const protocol = COPILOT_ENABLED && provider?.structuredProtocol === 'copilot-acp' ? 'copilot-sdk' : provider?.structuredProtocol
  return normalizeCliProviderIdAlias(providerId) === metadata.id || protocol === metadata.structuredProtocol
}

export function isCodexProvider(provider?: Provider, providerId = ''): boolean {
  return CODEX_ENABLED && providerUsesProtocol(provider, providerId, CODEX_CLI_PROVIDER_ID)
}

export function isClaudeProvider(provider?: Provider, providerId = ''): boolean {
  return CLAUDE_ENABLED && providerUsesProtocol(provider, providerId, CLAUDE_CLI_PROVIDER_ID)
}

export function isCopilotProvider(provider?: Provider, providerId = ''): boolean {
  return COPILOT_ENABLED && providerUsesProtocol(provider, providerId, COPILOT_CLI_PROVIDER_ID)
}

export function isOpenCodeProvider(provider?: Provider, providerId = ''): boolean {
  return OPENCODE_ENABLED && providerUsesProtocol(provider, providerId, OPENCODE_CLI_PROVIDER_ID)
}

export function providerNpmPackageName(providerId: string, provider?: Provider): string {
  if (provider?.npmPackageName?.trim()) return provider.npmPackageName.trim()
  return providerMetadataForId(providerId).npmPackage
}

export function buildProviderCliInstallCommand(providerId: string, version: string, provider?: Provider): string {
  const packageName = providerNpmPackageName(providerId, provider)
  return packageName ? `npm install -g ${packageName}@${version}` : ''
}
