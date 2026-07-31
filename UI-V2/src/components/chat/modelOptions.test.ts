import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { buildCodexReasoningOptions, buildModelOptions, reasoningEffortForModel } from './modelOptions'

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

  it('offers ultra reasoning when live model metadata is unavailable', () => {
    expect(buildCodexReasoningOptions(undefined, 'gpt-5.6').at(-1)).toMatchObject({
      id: 'ultra',
      label: 'Ultra',
    })
    expect(buildCodexReasoningOptions(undefined, 'gpt-5.4').some((option) => option.id === 'ultra')).toBe(false)
    expect(reasoningEffortForModel(undefined, 'gpt-5.4', 'ultra')).toBe('')
  })

  it('does not invent Copilot reasoning choices when ACP omits them', () => {
    const acp = {
      availableModels: [{ id: 'gpt-5.1', name: 'GPT-5.1', supportedReasoningEfforts: [] }],
    } as AcpBinding

    expect(buildCodexReasoningOptions(acp, 'gpt-5.1', 'max', [])).toEqual([])
    expect(reasoningEffortForModel(acp, 'gpt-5.1', 'max')).toBe('')
  })
})
