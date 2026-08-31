import type { Provider } from '../types/provider'

export const DEFAULT_PROVIDER_ID = 'gemini-cli'
export const CODEX_CLI_PROVIDER_ID = 'codex-cli'
export const CLAUDE_CLI_PROVIDER_ID = 'claude-cli'
export const OPENCODE_CLI_PROVIDER_ID = 'opencode-cli'
export const COPILOT_CLI_PROVIDER_ID = 'copilot-cli'

export interface ProviderCapabilities {
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
  claudePlanPrompt: boolean
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

export const GEMINI_DEFAULT_MODEL_LABELS: Record<string, { label: string; shortLabel: string; detail: string }> = {
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
  { id: '', label: 'Default', detail: 'Use Codex default reasoning' },
  { id: 'minimal', label: 'Minimal', detail: 'Fastest reasoning' },
  { id: 'low', label: 'Low', detail: 'Faster responses' },
  { id: 'medium', label: 'Medium', detail: 'Balanced reasoning' },
  { id: 'high', label: 'High', detail: 'Deeper reasoning' },
  { id: 'xhigh', label: 'Extra High', detail: 'Very deep reasoning' },
  { id: 'max', label: 'Max', detail: 'Maximum available reasoning' },
  { id: 'ultra', label: 'Ultra', detail: 'Maximum reasoning with automatic delegation' },
]

export const COPILOT_REASONING_OPTIONS = [
  { id: '', label: 'Default', detail: 'Use Copilot default reasoning' },
  { id: 'none', label: 'None', detail: 'No extra reasoning' },
  { id: 'minimal', label: 'Minimal', detail: 'Fastest reasoning' },
  { id: 'low', label: 'Low', detail: 'Faster responses' },
  { id: 'medium', label: 'Medium', detail: 'Balanced reasoning' },
  { id: 'high', label: 'High', detail: 'Deeper reasoning' },
  { id: 'xhigh', label: 'XHigh', detail: 'Very deep reasoning' },
  { id: 'max', label: 'Max', detail: 'Maximum available reasoning' },
]

export const CODEX_SPEED_OPTIONS = [
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
  [DEFAULT_PROVIDER_ID]: {
    id: DEFAULT_PROVIDER_ID,
    name: 'Gemini CLI',
    shortName: 'Gemini',
    structuredProtocol: 'gemini-acp',
    runtimeDescription: 'Gemini ACP + CLI',
    npmPackage: '@google/gemini-cli',
    capabilities: {
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
      claudePlanPrompt: false,
      reasoningOptions: [],
      speedOptions: [],
    },
  },
  [CODEX_CLI_PROVIDER_ID]: {
    id: CODEX_CLI_PROVIDER_ID,
    name: 'Codex CLI',
    shortName: 'Codex',
    structuredProtocol: 'codex-app-server',
    runtimeDescription: 'Codex app-server + CLI',
    npmPackage: '@openai/codex',
    capabilities: {
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
      claudePlanPrompt: false,
      reasoningOptions: CODEX_REASONING_OPTIONS,
      speedOptions: CODEX_SPEED_OPTIONS,
    },
  },
  [CLAUDE_CLI_PROVIDER_ID]: {
    id: CLAUDE_CLI_PROVIDER_ID,
    name: 'Claude Code',
    shortName: 'Claude',
    structuredProtocol: 'claude-code-stream-json',
    runtimeDescription: 'Claude stream + CLI',
    npmPackage: '@anthropic-ai/claude-code',
    capabilities: {
      hasReasoningEffort: false,
      hasServiceTier: false,
      hasAcceptEditsMode: false,
      structuredPermissionControl: 'provider',
      terminalPermissionControl: 'provider',
      usesFriendlyModelLabels: false,
      showPlanActionButtons: false,
      autoLabel: 'Auto',
      acceptEditsLabel: 'Accept Edits',
      defaultModelLabels: GEMINI_DEFAULT_MODEL_LABELS,
      memoryModelIds: ['', 'sonnet', 'opus'],
      memoryModelLabels: CLAUDE_MEMORY_MODEL_LABELS,
      memoryModelDefaultId: '',
      claudePlanPrompt: true,
      reasoningOptions: [],
      speedOptions: [],
    },
  },
  [OPENCODE_CLI_PROVIDER_ID]: {
    id: OPENCODE_CLI_PROVIDER_ID,
    name: 'OpenCode',
    shortName: 'OpenCode',
    structuredProtocol: 'opencode-acp',
    runtimeDescription: 'OpenCode ACP + CLI',
    npmPackage: 'opencode-ai',
    capabilities: {
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
      claudePlanPrompt: false,
      reasoningOptions: [],
      speedOptions: [],
    },
  },
  [COPILOT_CLI_PROVIDER_ID]: {
    id: COPILOT_CLI_PROVIDER_ID,
    name: 'GitHub Copilot CLI',
    shortName: 'Copilot',
    structuredProtocol: 'copilot-acp',
    runtimeDescription: 'Copilot ACP + CLI',
    npmPackage: '@github/copilot',
    capabilities: {
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
      claudePlanPrompt: false,
      reasoningOptions: COPILOT_REASONING_OPTIONS,
      speedOptions: [],
    },
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
    capabilities: { ...GEMINI_METADATA.capabilities },
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

export function isCodexProvider(provider?: Provider, providerId = ''): boolean {
  return providerUsesProtocol(provider, providerId, CODEX_CLI_PROVIDER_ID)
}

export function isClaudeProvider(provider?: Provider, providerId = ''): boolean {
  return providerUsesProtocol(provider, providerId, CLAUDE_CLI_PROVIDER_ID)
}

export function isCopilotProvider(provider?: Provider, providerId = ''): boolean {
  return providerUsesProtocol(provider, providerId, COPILOT_CLI_PROVIDER_ID)
}

export function isOpenCodeProvider(provider?: Provider, providerId = ''): boolean {
  return providerUsesProtocol(provider, providerId, OPENCODE_CLI_PROVIDER_ID)
}

export function providerNpmPackageName(providerId: string, provider?: Provider): string {
  if (provider?.npmPackageName?.trim()) return provider.npmPackageName.trim()
  return providerMetadataForId(providerId).npmPackage
}

export function buildProviderCliInstallCommand(providerId: string, version: string, provider?: Provider): string {
  return `npm install -g ${providerNpmPackageName(providerId, provider)}@${version}`
}
