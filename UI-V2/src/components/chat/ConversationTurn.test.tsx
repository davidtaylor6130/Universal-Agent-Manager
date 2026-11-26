import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { ConversationTurn, type ConversationDelivery } from './ConversationTurn'
import { ChatAnnouncer } from './StatusHelpers'
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
    const status = host.querySelector<HTMLElement>('.conversation-turn__delivery')!
    expect(status.firstChild?.textContent).toBe('Not sent')
    expect(status.querySelector('svg')).toBeNull()
    expect(status.textContent).toBe('Not sent. Never reached the provider.')
    expect(status.querySelector('.sr-only')?.textContent).toBe('. Never reached the provider.')
    expect(host.querySelector('.conversation-turn__actions button[aria-label="Edit unsent message"]')).not.toBeNull()
    act(() => root.unmount())
  })

  it('keeps the delivery marker out of the Tab order and out of live regions', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    for (const state of ['sending', 'unsent', 'unconfirmed', 'waiting'] as const) {
      act(() => root.render(<ConversationTurn role="user" assistantLabel="Claude" delivery={{ state, label: 'Explanation.' }}>Hi</ConversationTurn>))
      const status = host.querySelector<HTMLElement>('.conversation-turn__delivery')!
      expect(status.tabIndex).toBe(-1)
      expect(status.hasAttribute('tabindex')).toBe(false)
      expect(status.getAttribute('role')).toBeNull()
      expect(host.querySelector('[role="status"], [aria-live]')).toBeNull()
    }
    act(() => root.unmount())
  })

  it('announces only an in-flight message becoming definitely unsent, once', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const lines: string[] = []
    const render = (delivery?: ConversationDelivery) => act(() => root.render(<ChatAnnouncer>
      <ConversationTurn role="user" assistantLabel="Claude" delivery={delivery}>Hi</ConversationTurn>
    </ChatAnnouncer>))
    const region = () => host.querySelector('[data-testid="chat-announcement"]')!
    const read = () => { if (region().textContent) lines.push(region().textContent!) }
    // A row that is already unsent when the chat opens is not announced.
    render({ state: 'unsent', label: 'Never reached the provider.' })
    read()
    expect(lines).toEqual([])
    act(() => root.unmount())
    const second = createRoot(host)
    const renderSecond = (delivery?: ConversationDelivery) => act(() => second.render(<ChatAnnouncer>
      <ConversationTurn role="user" assistantLabel="Claude" delivery={delivery}>Hi</ConversationTurn>
    </ChatAnnouncer>))
    renderSecond({ state: 'sending', label: 'Waiting.' })
    read()
    renderSecond({ state: 'unconfirmed', label: 'Not confirmed.' })
    read() // Uncertain delivery is never presented as a failure.
    expect(lines).toEqual([])
    renderSecond({ state: 'unsent', label: 'Never reached the provider.' })
    read()
    expect(lines).toEqual(['Message not sent.'])
    const announced = region().firstChild
    renderSecond({ state: 'unsent', label: 'Edit or remove it.' })
    expect(region().firstChild).toBe(announced) // A changed explanation is not a second announcement.
    act(() => second.unmount())
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
