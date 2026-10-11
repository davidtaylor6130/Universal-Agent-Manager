import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import { readChatComposerDraft, removeComposerDrafts } from '../../utils/composerDraftStorage'
import { expectWithinBudget, INPUT_BUDGET_MS, HISTORY_PAGE_BUDGET_MS } from '../../test/interactionBudgets'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
const session: Session = { id: 'performance-chat', name: 'Performance fixture', viewMode: 'chat', folderId: null,
  providerId: 'gemini-cli', createdAt: new Date(0), updatedAt: new Date(0) }
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>

function messages(count: number): Message[] {
  return Array.from({ length: count }, (_, index) => ({ id: `message-${index}`, sessionId: session.id,
    role: index % 2 ? 'assistant' : 'user', content: `Fixture message ${index}: ${'ordinary transcript text '.repeat(20)}`, createdAt: new Date(index) }))
}

beforeEach(() => {
  vi.useRealTimers()
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  removeComposerDrafts([session.id])
  useAppStore.setState(initial, true)
  useAppStore.setState({ sessions: [session], activeSessionId: session.id, folders: [],
    messages: { [session.id]: [] }, acpBindingBySessionId: {}, cliBindingBySessionId: {},
    historyStartIndexBySessionId: {}, chatHistoryErrorBySessionId: {},
    refreshUamAgents: vi.fn(async () => true), loadSessionMessages: vi.fn(async () => true) })
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
  removeComposerDrafts([session.id])
  useAppStore.setState(initial, true)
  vi.unstubAllGlobals()
})

describe('chat interaction code-path performance', () => {
  it('makes an empty new-chat composer editable within 100ms', () => {
    expectWithinBudget('empty chat to usable composer', INPUT_BUDGET_MS, () => {
      act(() => root.render(<ChatView session={session} />))
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
      expect(input).not.toBeNull()
      expect(input!.disabled).toBe(false)
    })
  })

  it('publishes local send feedback within 100ms while provider admission is pending', async () => {
    let finish!: (result: boolean) => void
    const send = vi.fn(() => new Promise<boolean>((resolve) => { finish = resolve }))
    useAppStore.setState({ sendAcpPrompt: send })
    act(() => root.render(<ChatView session={session} />))
    const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, 'Send fixture')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const button = host.querySelector<HTMLButtonElement>('[aria-label="Send prompt"]')!
    expect(button.disabled).toBe(false)
    expectWithinBudget('send to local pending feedback', INPUT_BUDGET_MS, () => {
      act(() => button.click())
      expect(send).toHaveBeenCalledWith(session.id, 'Send fixture', [])
      expect(button.disabled).toBe(true)
      expect(input.disabled).toBe(false)
    })
    act(() => host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    expect(send).toHaveBeenCalledTimes(1)
    await act(async () => finish(true))
    expect(input.value).toBe('')
  })

  it.each([20, 50, 200, 10000])('commits the initial bounded history view for %i total messages within 1s', (count) => {
    const pageSize = Math.min(count, 50)
    useAppStore.setState({ sessions: [{ ...session, messageCount: count }], messages: { [session.id]: messages(pageSize) },
      historyStartIndexBySessionId: { [session.id]: count - pageSize } })
    expectWithinBudget('history response to committed initial page', HISTORY_PAGE_BUDGET_MS, () => {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).toContain(`Fixture message ${pageSize - 1}`)
      expect(host.querySelector('textarea')).not.toBeNull()
    })
  })

  it.each([false, true])('handles each input event within 100ms with streaming updates=%s', (streaming) => {
    useAppStore.setState({ messages: { [session.id]: messages(50) } })
    act(() => root.render(<ChatView session={session} />))
    const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
    expect(input).not.toBeNull()
    for (let index = 0; index < 20; index += 1) {
      if (streaming) {
        act(() => useAppStore.setState((state) => ({ messages: { ...state.messages, [session.id]:
          state.messages[session.id].map((message, messageIndex) => messageIndex === 49
            ? { ...message, content: `${message.content} streaming delta ${index}` } : message) } })))
      }
      const text = `Draft ${'typed '.repeat(index + 1)}`
      expectWithinBudget(`input ${index} to committed draft`, INPUT_BUDGET_MS, () => {
        act(() => {
          Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, text)
          input.dispatchEvent(new Event('input', { bubbles: true }))
        })
        expect(input.value).toBe(text)
      })
      // Verify React retained the input separately; a second render is not part of the event response.
      act(() => root.render(<ChatView session={session} />))
      expect(input.value).toBe(text)
    }
    // Persistence is intentionally debounced; pagehide flush verifies the React-owned draft.
    act(() => window.dispatchEvent(new Event('pagehide')))
    expect(readChatComposerDraft(session.id).text).toBe(input.value)
  })
})
