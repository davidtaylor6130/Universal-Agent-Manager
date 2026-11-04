import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { ConversationTurn } from './ConversationTurn'
import { OutgoingRecoveryActions } from './OutgoingMessageRow'

// Vitest empties CSS imports, so read the source tokens (runner cwd is UI-V2).
const indexCss = readFileSync(resolve(process.cwd(), 'src/index.css'), 'utf8')
const conversationCss = readFileSync(resolve(process.cwd(), 'src/components/chat/conversation.css'), 'utf8')

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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

  it('shows Not sent as one quiet line with no icon, beside recovery actions that keep their slot', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    act(() => root.render(<ConversationTurn role="user" assistantLabel="Claude" delivery={{ state: 'unsent', label: 'Never reached the provider.' }}
      deliveryActions={<OutgoingRecoveryActions onEdit={() => {}} onRemove={async () => true} />}>Hi</ConversationTurn>))
    const status = host.querySelector('[role="status"]')!
    expect(status.textContent).toBe('Not sent')
    expect(status.querySelector('svg')).toBeNull()
    expect(status.getAttribute('aria-label')).toBe('Not sent. Never reached the provider.')
    expect(host.querySelector('.conversation-turn__actions button[aria-label="Edit unsent message"]')).not.toBeNull()
    act(() => root.unmount())
  })

  it('fades hover actions without collapsing them, so the delivery label never shifts', () => {
    const hidden = indexCss.match(/\.conversation-turn__actions \{([^}]*)\}/)![1]
    expect(hidden).toContain('opacity: 0')
    expect(hidden).not.toMatch(/display:\s*none|visibility:\s*hidden|width:\s*0/)
    expect(indexCss).toMatch(/\.conversation-turn:focus-within \.conversation-turn__actions/)
    expect(conversationCss).toMatch(/\.conversation-turn__actions, \.conversation-turn__branches \{[^}]*flex-shrink: 0/)
    expect(conversationCss).toMatch(/\.conversation-turn__delivery \{[^}]*white-space: nowrap;[^}]*flex-shrink: 0/)
  })
})
