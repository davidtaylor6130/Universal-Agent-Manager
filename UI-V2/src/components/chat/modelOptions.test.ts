import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { buildProviderReasoningOptions, buildCodexReasoningOptions, buildCodexSpeedOptions, buildModelOptions, CODEX_SPEED_INHERIT_ID, FRIENDLY_MODEL_LABELS, modelCatalogForDefaults, reasoningEffortForModel, searchModelOptions, selectedRuntimeModel } from './modelOptions'

describe('reported model capacity options', () => {
  it('retains reported token limits for every provider, including friendly labels and explicit zero', () => {
    const modelId = Object.keys(FRIENDLY_MODEL_LABELS).find(Boolean)!
    const source = { availableModels: [{ id: modelId, name: 'Reported model', description: '', contextWindowTokens: 200000, maxOutputTokens: 0 }] }
    for (const providerId of ['gemini-cli', 'codex-cli', 'claude-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli']) {
      expect(buildModelOptions(source, '', undefined, providerId)[0]).toMatchObject({ id: modelId, contextWindowTokens: 200000, maxOutputTokens: 0 })
    }
    expect(buildModelOptions(source, '', undefined, 'gemini-cli')[0].label).toBe(FRIENDLY_MODEL_LABELS[modelId].label)
  })

  it('does not infer capacity for defaults, configured choices, retained selections or fallback names', () => {
    const source = {
      currentModelId: 'reported',
      availableModels: [{ id: 'reported', name: 'Reported', description: '', contextWindowTokens: 1000000, maxOutputTokens: 64000 }],
      configOptions: [{ id: 'model', category: 'model', name: 'Model', description: '', currentValue: 'configured-70b-200k', options: [{ value: 'configured-70b-200k', name: 'Configured 70B 200K', description: '' }] }],
    }
    const options = buildModelOptions(source, 'retained-128k', undefined, 'codex-cli', true)
    expect(options.find(option => option.id === 'reported')?.contextWindowTokens).toBe(1000000)
    for (const id of ['', 'configured-70b-200k', 'retained-128k']) {
      const option = options.find(option => option.id === id)!
      expect(option).toBeDefined()
      expect(option.contextWindowTokens).toBeUndefined()
      expect(option.maxOutputTokens).toBeUndefined()
    }
    for (const option of buildModelOptions(undefined, '', undefined, 'gemini-cli')) {
      expect(option.contextWindowTokens).toBeUndefined()
      expect(option.maxOutputTokens).toBeUndefined()
    }
  })
})

describe('reasoningEffortForModel', () => {
  it('defaults invalid or empty effort to the runtime model default', () => {
    const acp = {
      availableModels: [{
        id: 'gpt-5.6',
        name: 'GPT-5.6',
        defaultReasoningEffort: 'medium',
        supportedReasoningEfforts: ['low', 'medium', 'high', 'xhigh', 'ultra'],
      }],
    } as AcpBinding

    expect(reasoningEffortForModel(acp, 'gpt-5.6')).toBe('medium')
    expect(reasoningEffortForModel(acp, 'gpt-5.6', 'unknown')).toBe('medium')
    expect(reasoningEffortForModel(acp, 'gpt-5.6', 'low')).toBe('low')
    expect(reasoningEffortForModel(acp, 'gpt-5.6', 'ultra')).toBe('ultra')
  })

  it('keeps provider-default model selection explicit', () => {
    const options = buildModelOptions(undefined, '', undefined, 'gemini-cli', true)

    expect(options[0]).toMatchObject({ id: '', label: 'Default' })
  })

  it('uses ACP model config choices when the provider omits availableModels', () => {
    const acp = {
      availableModels: [],
      configOptions: [{
        id: 'model',
        name: 'Model',
        description: '',
        category: 'model',
        currentValue: 'opencode/big-pickle',
        options: [{ value: 'opencode/big-pickle', name: 'Big Pickle', description: 'Remote model' }],
      }],
    } as AcpBinding

    expect(buildModelOptions(acp, '', undefined, 'opencode-cli').map((option) => option.id))
      .toEqual(['opencode/big-pickle'])
  })

  it('keeps Copilot launch-time reasoning when ACP omits per-model efforts', () => {
    const acp = {
      availableModels: [{ id: 'gpt-5.1', name: 'GPT-5.1', supportedReasoningEfforts: [] }],
    } as AcpBinding
    const fallbackEfforts = ['', 'low', 'medium', 'high', 'xhigh', 'max']

    expect(buildCodexReasoningOptions(acp, 'gpt-5.1', 'max', fallbackEfforts).map((option) => option.id))
      .toEqual(fallbackEfforts)
    expect(reasoningEffortForModel(acp, 'gpt-5.1', 'max', true)).toBe('max')
  })

  it('uses the selected model speed catalog instead of global Fast and Flex choices', () => {
    const acp = {
      availableModels: [
        { id: 'gpt-5.6-sol', name: 'GPT-5.6 Sol', additionalSpeedTiers: ['fast'] },
        { id: 'gpt-5.4-mini', name: 'GPT-5.4 Mini', additionalSpeedTiers: [] },
      ],
    } as AcpBinding

    expect(buildCodexSpeedOptions(acp, 'gpt-5.6-sol').map((option) => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, '', 'fast'])
    expect(buildCodexSpeedOptions(acp, 'gpt-5.4-mini', 'flex').map((option) => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, ''])
    expect(buildCodexSpeedOptions(undefined, '').map((option) => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, '', 'fast', 'flex'])
    expect(buildCodexSpeedOptions(acp, 'gpt-5.4-mini').map((option) => option.label).slice(0, 2)).toEqual(['Inherit', 'Standard'])
  })

  it('keeps current Codex efforts in the offline fallback catalog', () => {
    expect(buildCodexReasoningOptions(undefined, '', '').map((option) => option.id))
      .toEqual(['', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'ultra'])
  })

  it('shows Claude speed tiers only from its native model catalogue', () => {
    const acp = { availableModels: [
      { id: 'opus', name: 'Opus', additionalSpeedTiers: ['fast'] },
      { id: 'sonnet', name: 'Sonnet', additionalSpeedTiers: [] },
    ] } as AcpBinding
    expect(buildCodexSpeedOptions(undefined, 'opus', 'fast', false)).toEqual([])
    expect(buildCodexSpeedOptions(acp, 'opus', '', false).map(option => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, '', 'fast'])
    expect(buildCodexSpeedOptions(acp, 'sonnet', 'fast', false).map(option => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, ''])
  })
})

it('filters OpenCode providers and models while retaining a disabled current model', () => {
  const acp = { availableModels: [
    { id: 'openai/visible', name: 'Visible', description: '' },
    { id: 'openai/hidden', name: 'Hidden', description: '' },
    { id: 'anthropic/sonnet', name: 'Sonnet', description: '' },
  ] }
  const visibility = { hiddenModelIds: ['openai/hidden'], hiddenProviderIds: ['anthropic'] }
  const options = buildModelOptions(acp, 'openai/hidden', undefined, 'opencode-cli', true, visibility)
  expect(options.filter((option) => !option.disabled).map((option) => option.id)).toEqual(['', 'openai/visible'])
  expect(options.find((option) => option.id === 'openai/hidden')).toMatchObject({ label: 'Hidden (hidden)', disabled: true })
  expect(acp.availableModels).toHaveLength(3)
  expect(buildModelOptions(acp, '', undefined, 'opencode-cli', true, visibility).some((option) => option.id === 'openai/hidden')).toBe(false)
})

describe('searchModelOptions', () => {
  it('matches names, IDs and descriptions without changing model identity or availability', () => {
    const options = buildModelOptions(undefined, '', undefined, 'gemini-cli', true)
    expect(searchModelOptions(options, '   ')).toBe(options)
    const candidate = options.find((option) => option.id)!
    expect(searchModelOptions(options, candidate.id.toUpperCase())).toContain(candidate)
    expect(searchModelOptions(options, 'no-such-model')).toEqual([])
    const hidden = { ...candidate, disabled: true, detail: 'Hidden fast model' }
    expect(searchModelOptions([hidden], 'FAST hidden')).toEqual([hidden])
    expect(searchModelOptions([hidden], 'fast unavailable')).toEqual([])
  })
})


describe('advertised provider default model', () => {
  it('uses the actual Antigravity default when a different model advertises effort', () => {
    const models = [
      { id: 'fixed-default', name: 'Fixed', description: '', supportedReasoningEfforts: [] },
      { id: 'adjustable', name: 'Adjustable', description: '', defaultReasoningEffort: 'high', supportedReasoningEfforts: ['low', 'high'] },
    ]
    const source = { protocolKind: 'antigravity-stream-json', availableModels: models }
    expect(selectedRuntimeModel(source, '')?.id).toBe('fixed-default')
    expect(buildCodexReasoningOptions(source, '', '', [])).toEqual([])
    expect(selectedRuntimeModel({ ...source, currentModelId: 'adjustable' }, '')?.id).toBe('adjustable')
    expect(buildCodexReasoningOptions({ ...source, currentModelId: 'adjustable' }, '', '', []).map((option) => option.id)).toEqual(['low', 'high'])
  })
})


describe('provider capability review regressions', () => {
  it('keeps unadvertised effort choices hidden while Antigravity discovery is unavailable', () => {
    for (const source of [undefined, { availableModels: [] }, { availableModels: [{ id: 'fixed', name: 'Fixed', supportedReasoningEfforts: [] }] }]) {
      expect(buildCodexReasoningOptions(source, 'missing', 'high', [])).toEqual([])
    }
    const catalogue = { availableModels: [
      { id: 'fixed', name: 'Fixed', supportedReasoningEfforts: [] },
      { id: 'adjustable', name: 'Adjustable', defaultReasoningEffort: 'high', supportedReasoningEfforts: ['low', 'high'] },
    ] }
    expect(selectedRuntimeModel(catalogue, '')?.id).toBe('fixed')
    expect(buildCodexReasoningOptions(catalogue, '', '', [])).toEqual([])
  })

  it('shows provider Default when Claude advertises efforts without a default', () => {
    const source = { availableModels: [{ id: 'sonnet', name: 'Sonnet', supportedReasoningEfforts: ['low', 'medium', 'high', 'max'] }] }
    expect(buildCodexReasoningOptions(source, 'sonnet', '', []).map(option => option.id)).toEqual(['', 'low', 'medium', 'high', 'max'])
    expect(reasoningEffortForModel(source, 'sonnet', '')).toBe('')
    expect(reasoningEffortForModel(source, 'sonnet', 'low')).toBe('low')
    expect(reasoningEffortForModel(source, 'sonnet', 'invalid')).toBe('')
  })

  it('isolates default option metadata from an unrelated chat model override', () => {
    const source = { currentModelId: 'chat-override', availableModels: [
      { id: 'provider-default', name: 'Default', supportedReasoningEfforts: [], additionalSpeedTiers: [] },
      { id: 'chat-override', name: 'Override', supportedReasoningEfforts: ['low', 'high'], additionalSpeedTiers: ['fast'] },
    ] }
    const defaults = modelCatalogForDefaults(source)
    expect(selectedRuntimeModel(source, '')?.id).toBe('chat-override')
    expect(selectedRuntimeModel(defaults, '')?.id).toBe('provider-default')
    expect(buildCodexReasoningOptions(defaults, '', '', [])).toEqual([])
    expect(buildCodexSpeedOptions(defaults, '', '', false).map(option => option.id)).toEqual([CODEX_SPEED_INHERIT_ID, ''])
    expect(selectedRuntimeModel(defaults, 'chat-override')?.id).toBe('chat-override')
    expect(source.currentModelId).toBe('chat-override')
  })
})

describe('explicit launch-configured model reasoning', () => {
  it('keeps Default absent on the wire and exposes no inferred model or alias choices', () => {
    const acp = { currentModelId: 'gemini-3.1-pro-preview', availableModels: [
      { id: 'gemini-3.1-pro-preview', name: 'Gemini', defaultReasoningEffort: '', supportedReasoningEfforts: ['low', 'medium', 'high'] },
      { id: 'auto-gemini-3', name: 'Auto', supportedReasoningEfforts: [] },
    ] } as AcpBinding
    expect(buildCodexReasoningOptions(acp, '', '', [], true)).toEqual([])
    expect(buildCodexReasoningOptions(acp, 'auto-gemini-3', '', [], true)).toEqual([])
    expect(buildCodexReasoningOptions(acp, 'gemini-3.1-pro-preview', '', [], true).map(option => option.id)).toEqual(['', 'low', 'medium', 'high'])
  })
})


describe('provider reasoning recovery', () => {
  it('provides a selectable Default for unavailable saved Gemini effort and normalized aliases', () => {
    for (const provider of ['gemini-cli', 'gemini', ' GEMINI ']) {
      const options = buildProviderReasoningOptions(provider, undefined, 'auto-gemini-3', 'high')
      expect(options).toHaveLength(2)
      expect(options[0]).toMatchObject({ id: 'high', disabled: true })
      expect(options[0].label).toContain('unavailable')
      expect(options[1]).toMatchObject({ id: '', label: 'Default' })
      expect(buildProviderReasoningOptions(provider, undefined, '', '')).toEqual([])
    }
  })

  it('preserves non-Gemini native capabilities and advertised defaults', () => {
    expect(buildProviderReasoningOptions('codex', undefined, '', '').map(option => option.id)).toContain('xhigh')
    expect(buildProviderReasoningOptions('github-copilot', undefined, '', '').map(option => option.id)).toContain('max')
    expect(buildProviderReasoningOptions('claude-cli', undefined, '', 'high')).toEqual([])
    expect(buildProviderReasoningOptions('opencode-cli', undefined, '', '')).toEqual([])
    const acp = { availableModels: [{ id: 'native', name: 'Native', supportedReasoningEfforts: ['low', 'high'], defaultReasoningEffort: 'high' }] } as AcpBinding
    for (const provider of ['claude', 'opencode-cli', 'antigravity-cli']) {
      expect(buildProviderReasoningOptions(provider, acp, 'native', 'high').map(option => option.id)).toEqual(['low', 'high'])
    }
    const noDefault = { availableModels: [{ id: 'native', name: 'Native', supportedReasoningEfforts: ['low', 'high'], defaultReasoningEffort: '' }] } as AcpBinding
    expect(buildProviderReasoningOptions('antigravity-cli', noDefault, 'native', '').map(option => option.id)).toEqual(['', 'low', 'high'])
  })
})
