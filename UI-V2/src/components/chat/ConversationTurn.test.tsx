import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { ConversationTurn } from './ConversationTurn'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('ConversationTurn actions', () => {
  it.each(['user', 'assistant'] as const)('omits the %s message-copy row while preserving branch actions', (role) => {
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<ConversationTurn role={role} assistantLabel="Claude">Answer</ConversationTurn>))
    expect(host.querySelector('button')).toBeNull()
    expect(host.querySelector('footer')).toBeNull()
    expect(host.textContent).toBe('Answer')
    act(() => root.render(<ConversationTurn role={role} assistantLabel="Claude" onEdit={() => {}} branchLabel="Edited branch">Answer</ConversationTurn>))
    expect(host.querySelector('button[aria-label="Edit message in new branch"]')).not.toBeNull()
    expect(host.querySelector('button[aria-label="Copy message"]')).toBeNull()
    expect(host.querySelector('footer')?.textContent).toContain('Edited branch')
    act(() => root.unmount())
  })
})
