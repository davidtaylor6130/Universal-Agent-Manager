// Model option constants and builder helpers for the chat composer.
// Extracted from ChatView.tsx (MO-3). Pure functions — no React, no side effects.

import type { AcpBinding, AcpModel } from '../../store/useAppStore'
import type { Provider } from '../../types/provider'
import {
  providerCapabilities,
  providerRuntimeKindLabel,
  providerShortName,
  GEMINI_DEFAULT_MODEL_LABELS,
  CODEX_REASONING_OPTIONS,
  CODEX_REASONING_LABELS,
  CODEX_SPEED_LABELS,
} from '../../utils/providerMetadata'

export interface ModelOption {
  id: string
  label: string
  shortLabel: string
  detail: string
}

type ModelCatalogSource = Pick<AcpBinding, 'availableModels'> & Partial<Pick<AcpBinding, 'protocolKind'>>

export const FRIENDLY_MODEL_LABELS = GEMINI_DEFAULT_MODEL_LABELS
export const CODEX_SPEED_INHERIT_ID = '__uam_inherit__'

export function providerRuntimeLabel(provider?: Provider, acp?: ModelCatalogSource) {
  return providerRuntimeKindLabel(provider, acp?.protocolKind)
}

export function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
}

function unresolvedModelOption(providerName: string): ModelOption {
  return {
    id: '',
    label: 'Detecting model…',
    shortLabel: 'Detecting model…',
    detail: `Waiting for ${providerName} to report its active model`,
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

export function selectedRuntimeModel(acp: ModelCatalogSource | undefined, modelId: string): AcpModel | undefined {
  const models = acp?.availableModels ?? []
  if (modelId) return models.find((model) => model.id === modelId)
  return models.find((model) => Boolean(model.defaultReasoningEffort)) ?? models[0]
}

export function buildModelOptions(
  acp: ModelCatalogSource | undefined,
  selectedModelId: string,
  provider: Provider | undefined,
  providerId: string,
  includeDefault = false
): ModelOption[] {
  const providerName = providerShortName(provider, providerId)
  const caps = providerCapabilities(providerId, provider)
  const runtimeOptions = (acp?.availableModels ?? []).flatMap((model) => {
    const option = modelOptionFromRuntime(model, caps.usesFriendlyModelLabels)
    return option ? [option] : []
  })
  const fallbackOptions = caps.memoryModelIds.length > 0
    ? caps.memoryModelIds.filter(Boolean).map((id) => {
      const label = caps.memoryModelLabels[id]?.label ?? titleFromModelId(id)
      return {
        id,
        label,
        shortLabel: label,
        detail: caps.memoryModelLabels[id]?.detail ?? id,
      }
    })
    : []
  const baseOptions = runtimeOptions.length > 0
    ? runtimeOptions
    : fallbackOptions
  const options: ModelOption[] = includeDefault
    ? [{
        id: '',
        label: 'Default',
        shortLabel: 'Default',
        detail: `Use ${providerName}'s default model`,
      }]
    : []
  const seen = new Set(options.map((option) => option.id))

  for (const option of baseOptions) {
    if (seen.has(option.id)) continue
    seen.add(option.id)
    options.push(option)
  }

  if (selectedModelId && !seen.has(selectedModelId)) {
    const friendly = caps.usesFriendlyModelLabels ? FRIENDLY_MODEL_LABELS[selectedModelId] : undefined
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
  return options.find((option) => option.id === (modelId ?? '')) ?? options[0] ?? unresolvedModelOption('Provider')
}

export function labeledOption(id: string, labels: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>>): ModelOption {
  const fallback = labels[id] ?? {
    label: titleFromModelId(id),
    shortLabel: titleFromModelId(id),
    detail: id,
  }
  return { id, ...fallback }
}

export function buildCodexReasoningOptions(
  acp: ModelCatalogSource | undefined,
  modelId: string,
  selectedReasoningEffort = '',
  fallbackEfforts?: string[]
): ModelOption[] {
	const runtimeModel = selectedRuntimeModel(acp, modelId)
	const runtimeEfforts = runtimeModel?.supportedReasoningEfforts ?? []
	if (runtimeModel && runtimeEfforts.length === 0 && fallbackEfforts === undefined) return []
	const base = runtimeEfforts.length > 0
	  ? runtimeEfforts
	  : fallbackEfforts ?? CODEX_REASONING_OPTIONS.map((option) => option.id)
	const ids = runtimeModel ? [...base] : ['', ...base]
	if (!runtimeModel && selectedReasoningEffort && !ids.includes(selectedReasoningEffort)) ids.push(selectedReasoningEffort)
	return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_REASONING_LABELS))
}

export function reasoningEffortForModel(
  acp: ModelCatalogSource | undefined,
  modelId: string,
  currentEffort = '',
  preserveWhenRuntimeOmitsEfforts = false
) {
	const model = selectedRuntimeModel(acp, modelId)
	if (!model) return currentEffort === 'ultra' && !/^gpt-5\.6(?:$|-)/i.test(modelId.trim()) ? '' : currentEffort
	const supported = model.supportedReasoningEfforts ?? []
	if (supported.length === 0) return preserveWhenRuntimeOmitsEfforts ? currentEffort : ''
	if (supported.includes(currentEffort)) return currentEffort
	const defaultEffort = model.defaultReasoningEffort ?? ''
	return supported.includes(defaultEffort) ? defaultEffort : supported[0]
}

export function serviceTierForModel(acp: ModelCatalogSource | undefined, modelId: string, currentTier = '') {
	const model = selectedRuntimeModel(acp, modelId)
	if (!model || !currentTier) return currentTier
	return (model.additionalSpeedTiers ?? []).includes(currentTier) ? currentTier : ''
}

export function buildCodexSpeedOptions(acp: ModelCatalogSource | undefined, modelId: string, selectedServiceTier = ''): ModelOption[] {
	const runtimeModel = selectedRuntimeModel(acp, modelId)
  const ids = [CODEX_SPEED_INHERIT_ID, '', ...new Set(runtimeModel ? runtimeModel.additionalSpeedTiers ?? [] : ['fast', 'flex'])]
  if (!runtimeModel && selectedServiceTier && !ids.includes(selectedServiceTier)) ids.push(selectedServiceTier)
  return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_SPEED_LABELS))
}
