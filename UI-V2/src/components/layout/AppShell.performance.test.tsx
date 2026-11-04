import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { AppShell } from './AppShell'
import { useAppStore } from '../../store/useAppStore'
import { expectWithinBudget, INTERACTION_BUDGET_MS } from '../../test/interactionBudgets'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
const session: Session = { id: 'shell-performance', name: 'Navigation fixture', viewMode: 'chat', folderId: null,
  providerId: 'gemini-cli', createdAt: new Date(0), updatedAt: new Date(0) }
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>

beforeEach(async () => {
  vi.useRealTimers()
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  // Warm module loading is separate from the measured event-to-committed-view path.
  await import('../settings/SettingsModal')
  useAppStore.setState(initial, true)
  useAppStore.setState({ sessions: [session], activeSessionId: session.id, folders: [], messages: {},
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, historyStartIndexBySessionId: {},
    isSettingsOpen: false, isNewChatModalOpen: false, memoryLibraryScope: null,
    refreshUamAgents: vi.fn(async () => true), refreshCustomThemes: vi.fn(async () => true),
    loadSessionMessages: vi.fn(async () => true), unloadSessionMessages: vi.fn() })
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
  useAppStore.setState(initial, true)
  vi.unstubAllGlobals()
})

describe('settings navigation code-path performance', () => {
  it.each([20, 200, 10000])('opens and closes settings within 250ms with %i total chat messages', async count => {
    const pageSize = Math.min(count, 50)
    const messages: Message[] = Array.from({ length: pageSize }, (_, index) => ({ id: `nav-${index}`,
      sessionId: session.id, role: index % 2 ? 'assistant' : 'user',
      content: `Navigation fixture ${index}: ${'ordinary transcript text '.repeat(20)}`, createdAt: new Date(index) }))
    useAppStore.setState({ sessions: [{ ...session, messageCount: count }], messages: { [session.id]: messages },
      historyStartIndexBySessionId: { [session.id]: count - pageSize } })
    await act(async () => root.render(<AppShell />))
    const draft = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
    expect(draft).not.toBeNull()
    const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
    // Resolve React.lazy once; repeated navigation is measured without transform/import time.
    await act(async () => settings.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    const openedAt = performance.now()
    expectWithinBudget('settings click to committed page', INTERACTION_BUDGET_MS, () => {
      act(() => settings.click())
      expect(host.querySelector('[aria-label="Settings pages"]')).not.toBeNull()
      expect(host.querySelector<HTMLElement>('[data-testid="chat-region"]')!.hidden).toBe(true)
    })
    const openMs = performance.now() - openedAt
    const closedAt = performance.now()
    expectWithinBudget('settings close to retained chat', INTERACTION_BUDGET_MS, () => {
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
      expect(host.querySelector<HTMLElement>('[data-testid="chat-region"]')!.hidden).toBe(false)
      expect(host.querySelector('.uam-composer-textarea')).toBe(draft)
      expect(host.querySelector('[aria-label="Settings workspace"]')).toBeNull()
    })
    console.info(`[performance] ${count} total messages: settings open ${openMs.toFixed(2)}ms, close ${(performance.now() - closedAt).toFixed(2)}ms`)

  })
})
