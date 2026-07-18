import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { reasoningEffortForModel } from './modelOptions'

describe('reasoningEffortForModel', () => {
  it('defaults invalid or empty effort to the highest supported value', () => {
    const acp = {
      availableModels: [{
        id: 'gpt-5.4',
        name: 'GPT-5.4',
        defaultReasoningEffort: 'medium',
        supportedReasoningEfforts: ['low', 'medium', 'high', 'xhigh'],
      }],
    } as AcpBinding

    expect(reasoningEffortForModel(acp, 'gpt-5.4')).toBe('xhigh')
    expect(reasoningEffortForModel(acp, 'gpt-5.4', 'unknown')).toBe('xhigh')
    expect(reasoningEffortForModel(acp, 'gpt-5.4', 'low')).toBe('low')
  })
})
