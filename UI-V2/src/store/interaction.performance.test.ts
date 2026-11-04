import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { expectWithinBudget, INPUT_BUDGET_MS, INTERACTION_BUDGET_MS } from '../test/interactionBudgets'

const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../ipc/cefBridge')>(),
  isCefContext: () => bridge.cef,
  isCompanionContext: () => false,
  sendToCEF: bridge.send,
}))
import { useAppStore } from './useAppStore'

const initial = useAppStore.getState()
const storage = new Map<string, string>()
const originalStorage = Object.getOwnPropertyDescriptor(window, 'localStorage')

beforeEach(() => {
  vi.useRealTimers()
  storage.clear()
  Object.defineProperty(window, 'localStorage', { configurable: true, value: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => storage.set(key, value),
    removeItem: (key: string) => storage.delete(key),
  } })
  // A stalled external operation must not delay the local response under test.
  bridge.cef = true
  bridge.send.mockReset().mockImplementation(() => new Promise(() => {}))
  useAppStore.setState(initial, true)
  useAppStore.setState({ isSettingsOpen: false, isNewChatModalOpen: false, commitPanelOpen: false, sidebarCollapsed: false })
})

afterEach(() => {
  bridge.cef = false
  useAppStore.setState(initial, true)
  if (originalStorage) Object.defineProperty(window, 'localStorage', originalStorage)
})

/** Dispatch a mock click to the real action and observe its actual state publication. */
function clickAndObserve(action: () => void, responded: () => boolean, budget = INTERACTION_BUDGET_MS): void {
  const target = document.createElement('button')
  target.addEventListener('click', action)
  let observed = false
  const unsubscribe = useAppStore.subscribe(() => { if (responded()) observed = true })
  try {
    expectWithinBudget('click to local state response', budget, () => {
      target.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      expect(observed).toBe(true)
      expect(responded()).toBe(true)
    })
  } finally { unsubscribe() }
}

describe('interaction code-path performance budgets', () => {
  it.each([true, false])('publishes settings visibility %s within 250ms', (open) => {
    useAppStore.setState({ isSettingsOpen: !open })
    clickAndObserve(() => useAppStore.getState().setSettingsOpen(open), () => useAppStore.getState().isSettingsOpen === open)
  })

  it.each([true, false])('publishes new-chat visibility %s within the 100ms input budget', (open) => {
    useAppStore.setState({ isNewChatModalOpen: !open })
    clickAndObserve(() => useAppStore.getState().setNewChatModalOpen(open, 'workspace'),
      () => useAppStore.getState().isNewChatModalOpen === open && useAppStore.getState().newChatFolderId === (open ? 'workspace' : null), INPUT_BUDGET_MS)
  })

  it.each([true, false])('publishes commit-panel visibility %s including persisted layout within 250ms', (open) => {
    useAppStore.setState({ commitPanelOpen: !open })
    clickAndObserve(() => useAppStore.getState().setCommitPanelOpen(open), () => useAppStore.getState().commitPanelOpen === open)
    expect(storage.size).toBeGreaterThan(0)
  })

  it.each([true, false])('publishes sidebar visibility %s including persisted layout within 250ms', (collapsed) => {
    useAppStore.setState({ sidebarCollapsed: !collapsed })
    clickAndObserve(() => useAppStore.getState().setSidebarCollapsed(collapsed), () => useAppStore.getState().sidebarCollapsed === collapsed)
    expect(storage.size).toBeGreaterThan(0)
  })

  it('selects a chat in a large catalogue before the backend responds within 250ms', () => {
    const template = initial.sessions[0]
    expect(template).toBeDefined()
    useAppStore.setState({ sessions: Array.from({ length: 1000 }, (_, index) => ({ ...template, id: `perf-chat-${index}` })), activeSessionId: null })
    clickAndObserve(() => useAppStore.getState().setActiveSession('perf-chat-999'), () => useAppStore.getState().activeSessionId === 'perf-chat-999')
    expect(bridge.send).toHaveBeenCalledWith(expect.objectContaining({ action: 'selectSession' }))
  })

  it('publishes VCS preference feedback before a stalled save responds within 250ms', () => {
    useAppStore.setState({ gitEnabled: true, svnEnabled: true })
    clickAndObserve(() => { void useAppStore.getState().setVcsSettings({ gitEnabled: false, svnEnabled: false }) },
      () => !useAppStore.getState().gitEnabled && !useAppStore.getState().svnEnabled)
    expect(bridge.send).toHaveBeenCalledWith(expect.objectContaining({ action: 'setVcsSettings' }), false)
  })

  it('publishes sidebar preferences before a stalled save responds within 250ms', () => {
    useAppStore.setState({ showProviderIconsInSidebar: true, showWorktreePathInSidebar: true })
    clickAndObserve(() => { void useAppStore.getState().setSidebarSettings({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: false }) },
      () => !useAppStore.getState().showProviderIconsInSidebar && !useAppStore.getState().showWorktreePathInSidebar)
    expect(bridge.send).toHaveBeenCalledWith(expect.objectContaining({ action: 'setSidebarSettings' }))
  })
})
