import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { buildModelOptions, reasoningEffortForModel } from './modelOptions'

describe('reasoningEffortForModel', () => {
  it('defaults invalid or empty effort to the runtime model default', () => {
    const acp = {
      availableModels: [{
        id: 'gpt-5.4',
        name: 'GPT-5.4',
        defaultReasoningEffort: 'medium',
        supportedReasoningEfforts: ['low', 'medium', 'high', 'xhigh'],
      }],
    } as AcpBinding

    expect(reasoningEffortForModel(acp, 'gpt-5.4')).toBe('medium')
    expect(reasoningEffortForModel(acp, 'gpt-5.4', 'unknown')).toBe('medium')
    expect(reasoningEffortForModel(acp, 'gpt-5.4', 'low')).toBe('low')
  })

  it('keeps provider-default model selection explicit', () => {
    const options = buildModelOptions(undefined, '', undefined, 'gemini-cli', true)

    expect(options[0]).toMatchObject({ id: '', label: 'Default' })
  })
})
