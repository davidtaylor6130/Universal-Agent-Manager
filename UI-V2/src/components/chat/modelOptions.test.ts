import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { buildCodexReasoningOptions, buildCodexSpeedOptions, buildModelOptions, CODEX_SPEED_INHERIT_ID, reasoningEffortForModel } from './modelOptions'

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
})
