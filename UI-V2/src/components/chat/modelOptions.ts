// Model option constants and builder helpers for the chat composer.
// Extracted from ChatView.tsx (MO-3). Pure functions — no React, no side effects.

import type { AcpBinding, AcpModel } from '../../store/useAppStore'
import type { Provider } from '../../types/provider'
import {
  providerCapabilities,
  normalizeCliProviderIdAlias,
  DEFAULT_PROVIDER_ID,
  GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  providerRuntimeKindLabel,
  providerShortName,
  GEMINI_DEFAULT_MODEL_LABELS,
  CODEX_REASONING_OPTIONS,
  CODEX_REASONING_LABELS,
  CODEX_SPEED_LABELS,
} from '../../utils/providerMetadata'

export interface ModelOption extends Pick<AcpModel, 'contextWindowTokens' | 'maxOutputTokens'> {
  id: string
  label: string
  shortLabel: string
  detail: string
  disabled?: boolean
}

type ModelCatalogSource = Pick<AcpBinding, 'availableModels'> & Partial<Pick<AcpBinding, 'protocolKind' | 'configOptions' | 'currentModelId'>>

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
  const capacity = { contextWindowTokens: model.contextWindowTokens, maxOutputTokens: model.maxOutputTokens }
  if (useFriendlyLabels) {
    const friendly = FRIENDLY_MODEL_LABELS[id]
    if (friendly) {
      return { id, ...friendly, ...capacity, detail: model.description || friendly.detail }
    }
  }
  const label = model.name.trim() || titleFromModelId(id)
  return {
    id,
    label,
    shortLabel: label.length <= 16 ? label : titleFromModelId(id),
    detail: model.description.trim() || id,
    ...capacity,
  }
}

export function selectedRuntimeModel(acp: ModelCatalogSource | undefined, modelId: string): AcpModel | undefined {
  const models = acp?.availableModels ?? []
  if (modelId) return models.find((model) => model.id === modelId)
  const current = models.find((model) => model.id === acp?.currentModelId)
  if (current) return current
  return models[0]
}

/** Keep a live chat's selected model out of provider-wide defaults. */
export function modelCatalogForDefaults<T extends ModelCatalogSource>(source: T | undefined): T | undefined {
  return source ? { ...source, currentModelId: undefined } : undefined
}

export function buildModelOptions(
  acp: ModelCatalogSource | undefined,
  selectedModelId: string,
  provider: Provider | undefined,
  providerId: string,
  includeDefault = false,
  visibility?: { hiddenModelIds?: string[]; hiddenProviderIds?: string[] }
): ModelOption[] {
  const providerName = providerShortName(provider, providerId)
  const caps = providerCapabilities(providerId, provider)
  const runtimeOptions = (acp?.availableModels ?? []).flatMap((model) => {
    const option = modelOptionFromRuntime(model, caps.usesFriendlyModelLabels)
    return option ? [option] : []
  })
  const configModelOptions = (acp?.configOptions ?? [])
    .find((option) => option.category === 'model' || option.id === 'model')
    ?.options.flatMap((choice) => {
      const id = choice.value.trim()
      if (!id) return []
      const label = choice.name.trim() || titleFromModelId(id)
      return [{ id, label, shortLabel: label.length <= 16 ? label : titleFromModelId(id), detail: choice.description.trim() || id }]
    }) ?? []
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
  const reportedOptions = [...runtimeOptions, ...configModelOptions]
  const baseOptions = reportedOptions.length > 0
    ? reportedOptions
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

  if (providerId === OPENCODE_CLI_PROVIDER_ID && visibility) {
    return options.flatMap((option) => {
      const hidden = Boolean(option.id) && (visibility.hiddenModelIds?.includes(option.id) || visibility.hiddenProviderIds?.includes(option.id.split('/')[0]))
      if (!hidden) return [option]
      return option.id === selectedModelId ? [{ ...option, label: `${option.label} (hidden)`, disabled: true }] : []
    })
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
  fallbackEfforts?: string[],
  requireExplicitModel = false
): ModelOption[] {
	if (requireExplicitModel && !modelId) return []
	const runtimeModel = selectedRuntimeModel(acp, modelId)
	const runtimeEfforts = runtimeModel?.supportedReasoningEfforts ?? []
	if (runtimeModel && runtimeEfforts.length === 0 && fallbackEfforts === undefined) return []
	const base = runtimeEfforts.length > 0
	  ? runtimeEfforts
	  : fallbackEfforts ?? CODEX_REASONING_OPTIONS.map((option) => option.id)
	if (base.length === 0) return []
	const hasAdvertisedDefault = Boolean(runtimeModel?.defaultReasoningEffort && base.includes(runtimeModel.defaultReasoningEffort))
	const ids = runtimeModel && hasAdvertisedDefault ? [...base] : ['', ...base]
	if (!runtimeModel && selectedReasoningEffort && !ids.includes(selectedReasoningEffort)) ids.push(selectedReasoningEffort)
	return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_REASONING_LABELS))
}

/** Runtime-driven choices plus a real escape to Default for a persisted unavailable Gemini override. */
export function buildProviderReasoningOptions(providerId: string, acp: ModelCatalogSource | undefined, modelId: string, savedEffort = ''): ModelOption[] {
  const id = normalizeCliProviderIdAlias(providerId)
  const caps = providerCapabilities(id)
  if (id === GEMINI_CLI_PROVIDER_ID) {
    const options = buildCodexReasoningOptions(acp, modelId, savedEffort, [], true)
    if (!savedEffort || options.some(option => option.id === savedEffort)) return options
    const saved = labeledOption(savedEffort, CODEX_REASONING_LABELS)
    const unavailable = { ...saved, label: `${saved.label} (unavailable)`, shortLabel: `${saved.shortLabel} (unavailable)`, detail: 'This saved thinking level is unavailable for the selected model or checked CLI version. Select Default to clear it.', disabled: true }
    return [unavailable, ...(options.length ? options : [labeledOption('', CODEX_REASONING_LABELS)])]
  }
  const hasRuntimeEfforts = (selectedRuntimeModel(acp, modelId)?.supportedReasoningEfforts?.length ?? 0) > 0
  if (!caps.hasReasoningEffort && !hasRuntimeEfforts) return []
  return buildCodexReasoningOptions(acp, modelId, savedEffort,
    id === COPILOT_CLI_PROVIDER_ID ? caps.reasoningOptions.map(option => option.id) : id === CODEX_CLI_PROVIDER_ID ? undefined : [])
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
	return supported.includes(defaultEffort) ? defaultEffort : ''
}

export function serviceTierForModel(acp: ModelCatalogSource | undefined, modelId: string, currentTier = '') {
	const model = selectedRuntimeModel(acp, modelId)
	if (!model || !currentTier) return currentTier
	return (model.additionalSpeedTiers ?? []).includes(currentTier) ? currentTier : ''
}

export function buildCodexSpeedOptions(acp: ModelCatalogSource | undefined, modelId: string, selectedServiceTier = '', allowOfflineTiers = true): ModelOption[] {
	const runtimeModel = selectedRuntimeModel(acp, modelId)
  if (!runtimeModel && !allowOfflineTiers) return []
  const ids = [CODEX_SPEED_INHERIT_ID, '', ...new Set(runtimeModel ? runtimeModel.additionalSpeedTiers ?? [] : ['fast', 'flex'])]
  if (!runtimeModel && allowOfflineTiers && selectedServiceTier && !ids.includes(selectedServiceTier)) ids.push(selectedServiceTier)
  return Array.from(new Set(ids)).map((id) => {
    const option = labeledOption(id, CODEX_SPEED_LABELS)
    return id === 'fast' && !allowOfflineTiers
      ? { ...option, detail: 'Lower latency at higher usage rates; extra charges may apply' }
      : option
  })
}

/** Match all search terms against the model name, identifier and description. */
export function searchModelOptions(options: ModelOption[], query: string): ModelOption[] {
  const terms = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean)
  if (terms.length === 0) return options
  return options.filter((option) => {
    const text = `${option.id} ${option.label} ${option.detail}`.toLocaleLowerCase()
    return terms.every((term) => text.includes(term))
  })
}
