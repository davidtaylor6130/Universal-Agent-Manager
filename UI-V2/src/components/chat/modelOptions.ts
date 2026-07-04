// Model option constants and builder helpers for the chat composer.
// Extracted from ChatView.tsx (MO-3). Pure functions — no React, no side effects.

import type { AcpBinding, AcpModel } from '../../store/useAppStore'
import type { Provider } from '../../types/provider'
import {
  isClaudeProvider,
  isCodexProvider,
  isCopilotProvider,
  isOpenCodeProvider,
  providerRuntimeKindLabel,
  providerShortName,
} from '../../utils/providerMetadata'

export interface ModelOption {
  id: string
  label: string
  shortLabel: string
  detail: string
}

export const GEMINI_FALLBACK_ACP_MODEL_OPTIONS: ModelOption[] = [
  { id: '', label: 'CLI default', shortLabel: 'CLI default', detail: 'Use Gemini CLI settings' },
  { id: 'auto-gemini-3', label: 'Auto 3', shortLabel: 'Auto 3', detail: 'Gemini 3 routing' },
  { id: 'auto-gemini-2.5', label: 'Auto 2.5', shortLabel: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  { id: 'pro', label: 'Pro', shortLabel: 'Pro', detail: 'Prioritize capability' },
  { id: 'flash', label: 'Flash', shortLabel: 'Flash', detail: 'Prioritize speed' },
  { id: 'flash-lite', label: 'Flash Lite', shortLabel: 'Flash Lite', detail: 'Fastest option' },
]

export const FRIENDLY_MODEL_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'CLI default', detail: 'Use Gemini CLI settings' },
  'auto-gemini-3': { label: 'Auto 3', shortLabel: 'Auto 3', detail: 'Gemini 3 routing' },
  'auto-gemini-2.5': { label: 'Auto 2.5', shortLabel: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  pro: { label: 'Pro', shortLabel: 'Pro', detail: 'Prioritize capability' },
  flash: { label: 'Flash', shortLabel: 'Flash', detail: 'Prioritize speed' },
  'flash-lite': { label: 'Flash Lite', shortLabel: 'Flash Lite', detail: 'Fastest option' },
}

export const CODEX_REASONING_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'Default', detail: 'Use Codex default reasoning' },
  none: { label: 'None', shortLabel: 'None', detail: 'No extra reasoning' },
  minimal: { label: 'Minimal', shortLabel: 'Minimal', detail: 'Fastest reasoning' },
  low: { label: 'Low', shortLabel: 'Low', detail: 'Faster responses' },
  medium: { label: 'Medium', shortLabel: 'Medium', detail: 'Balanced reasoning' },
  high: { label: 'High', shortLabel: 'High', detail: 'Deeper reasoning' },
  xhigh: { label: 'XHigh', shortLabel: 'XHigh', detail: 'Maximum reasoning' },
}

export const CODEX_SPEED_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'Default', detail: 'Use Codex default speed' },
  fast: { label: 'Fast', shortLabel: 'Fast', detail: 'Prioritize latency' },
  flex: { label: 'Flex', shortLabel: 'Flex', detail: 'Use flexible service tier' },
}

export function providerRuntimeLabel(provider?: Provider, acp?: AcpBinding) {
  return providerRuntimeKindLabel(provider, acp?.protocolKind)
}

function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
}

function providerDefaultModelOption(providerName: string): ModelOption {
  return {
    id: '',
    label: 'CLI default',
    shortLabel: 'CLI default',
    detail: `Use ${providerName} CLI settings`,
  }
}

function modelOptionFromRuntime(model: AcpModel, useFriendlyLabels: boolean): ModelOption | null {
  const id = model.id.trim()
  if (!id) return null
  if (useFriendlyLabels) {
    const friendly = FRIENDLY_MODEL_LABELS[id]
    if (friendly) {
      return { id, ...friendly, detail: model.description || friendly.detail }
    }
  }
  const label = model.name.trim() || titleFromModelId(id)
  return {
    id,
    label,
    shortLabel: label.length <= 16 ? label : titleFromModelId(id),
    detail: model.description.trim() || id,
  }
}

function codexSelectedRuntimeModel(acp: AcpBinding | undefined, modelId: string): AcpModel | undefined {
  const models = acp?.availableModels ?? []
  return models.find((model) => model.id === modelId) ?? models.find((model) => Boolean(model.defaultReasoningEffort)) ?? models[0]
}

export function buildModelOptions(
  acp: AcpBinding | undefined,
  selectedModelId: string,
  provider: Provider | undefined,
  providerId: string
): ModelOption[] {
  const providerName = providerShortName(provider, providerId)
  const codexProvider = isCodexProvider(provider, providerId)
  const claudeProvider = isClaudeProvider(provider, providerId)
  const copilotProvider = isCopilotProvider(provider, providerId)
  const openCodeProvider = isOpenCodeProvider(provider, providerId)
  const runtimeOptions = (acp?.availableModels ?? []).flatMap((model) => {
    const option = modelOptionFromRuntime(model, !codexProvider && !claudeProvider && !copilotProvider && !openCodeProvider)
    return option ? [option] : []
  })
  const defaultOption = providerDefaultModelOption(providerName)
  const fallbackOptions = codexProvider
    ? [defaultOption]
    : claudeProvider
      ? [defaultOption, { id: 'sonnet', label: 'Sonnet', shortLabel: 'Sonnet', detail: 'Latest Sonnet alias' }, { id: 'opus', label: 'Opus', shortLabel: 'Opus', detail: 'Latest Opus alias' }]
      : copilotProvider
        ? [defaultOption]
        : openCodeProvider
          ? [defaultOption]
    : [defaultOption, ...GEMINI_FALLBACK_ACP_MODEL_OPTIONS.slice(1)]
  const baseOptions = runtimeOptions.length > 0
    ? [defaultOption, ...runtimeOptions]
    : fallbackOptions
  const options: ModelOption[] = []
  const seen = new Set<string>()

  for (const option of baseOptions) {
    if (seen.has(option.id)) continue
    seen.add(option.id)
    options.push(option)
  }

  if (selectedModelId && !seen.has(selectedModelId)) {
    const friendly = codexProvider || openCodeProvider ? undefined : FRIENDLY_MODEL_LABELS[selectedModelId]
    options.push(
      friendly
        ? { id: selectedModelId, ...friendly }
        : {
            id: selectedModelId,
            label: titleFromModelId(selectedModelId),
            shortLabel: titleFromModelId(selectedModelId),
            detail: selectedModelId,
          }
    )
  }

  return options
}

export function modelOptionFor(options: ModelOption[], modelId?: string) {
  return options.find((option) => option.id === (modelId ?? '')) ?? options[0] ?? providerDefaultModelOption('provider')
}

export function labeledOption(id: string, labels: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>>): ModelOption {
  const fallback = labels[id] ?? {
    label: titleFromModelId(id),
    shortLabel: titleFromModelId(id),
    detail: id,
  }
  return { id, ...fallback }
}

export function buildCodexReasoningOptions(acp: AcpBinding | undefined, modelId: string, selectedReasoningEffort = ''): ModelOption[] {
  const runtimeModel = codexSelectedRuntimeModel(acp, modelId)
  const runtimeEfforts = runtimeModel?.supportedReasoningEfforts ?? []
  const base = runtimeEfforts.length > 0 ? runtimeEfforts : ['none', 'minimal', 'low', 'medium', 'high', 'xhigh']
  const ids = ['', ...base]
  if (selectedReasoningEffort && !ids.includes(selectedReasoningEffort)) ids.push(selectedReasoningEffort)
  return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_REASONING_LABELS))
}

export function buildCodexSpeedOptions(acp: AcpBinding | undefined, modelId: string, selectedServiceTier = ''): ModelOption[] {
  const runtimeModel = codexSelectedRuntimeModel(acp, modelId)
  const runtimeTiers = runtimeModel?.additionalSpeedTiers ?? []
  const ids = ['', ...new Set([...runtimeTiers, 'fast', 'flex'])]
  if (selectedServiceTier && !ids.includes(selectedServiceTier)) ids.push(selectedServiceTier)
  return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_SPEED_LABELS))
}
