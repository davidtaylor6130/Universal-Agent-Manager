import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, expect, it, vi } from 'vitest'
import { CompanionShell } from './CompanionShell'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'

vi.mock('../../ipc/cefBridge', () => ({ sendToCEF: vi.fn() }))
vi.mock('../../store/useAppStore', async () => {
  const { create } = await import('zustand')
  return { useAppStore: create(() => ({
    sessions: [], folders: [], resourceCollections: [], activeSessionId: null, acpBindingBySessionId: {}, cliBindingBySessionId: {},
    loadFromCef: vi.fn(), loadSessionMessages: vi.fn(),
  })) }
})
vi.mock('../views/ChatView', () => ({ ChatView: () => <div>Conversation</div> }))
vi.mock('../sidebar/FolderTree', () => ({ FolderTree: () => <div>Workspace chats</div> }))
vi.mock('../sidebar/SessionItem', () => ({
  SessionItem: ({ sessionId }: { sessionId: string }) => <div data-session-id={sessionId}>Chat row</div>,
  sidebarStatusIcon: () => <span aria-hidden="true" />,
}))
vi.mock('../sidebar/NewChatModal', () => ({ NewChatModal: ({ onCreated }: { onCreated?: () => void }) => <div role="dialog">New chat modal<button onClick={onCreated}>Create</button></div> }))
vi.mock('../ui', () => ({
  Button: ({ children, variant: _variant, size: _size, leadingIcon: _leadingIcon, ...props }: React.ButtonHTMLAttributes<HTMLButtonElement> & { variant?: string; size?: string; leadingIcon?: React.ReactNode }) => <button {...props}>{children}</button>,
  IconButton: ({ label, onClick }: { label: string; onClick: () => void }) => <button aria-label={label} onClick={onClick}>{label}</button>,
}))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const createMemoryStorage = (): Storage => {
  const entries = new Map<string, string>()
  return {
    get length() { return entries.size },
    clear: () => entries.clear(),
    getItem: (key) => entries.get(key) ?? null,
    key: (index) => Array.from(entries.keys())[index] ?? null,
    removeItem: (key) => { entries.delete(key) },
    setItem: (key, value) => { entries.set(key, String(value)) },
  }
}

beforeEach(() => {
  Object.defineProperty(window, 'localStorage', { configurable: true, value: createMemoryStorage() })
  Object.defineProperty(window, 'sessionStorage', { configurable: true, value: createMemoryStorage() })
  window.localStorage.clear()
  sessionStorage.clear()
  vi.clearAllMocks()
  useAppStore.setState({
    sessions: [], folders: [], resourceCollections: [], activeSessionId: null,
    messages: {},
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, statusLine: '',
    isNewChatModalOpen: false, setNewChatModalOpen: vi.fn(), setSessionPinned: vi.fn().mockResolvedValue(true),
  })
})

it.each([1, 100])('accepts a restarted desktop revision %s, preserves selection, and disconnects', async (revision) => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  await act(async () => root.render(<CompanionShell />))
  expect(document.querySelector('link[rel=manifest]')?.getAttribute('href')).toBe('/companion.webmanifest')
  expect(document.querySelector('link[rel=apple-touch-icon]')?.getAttribute('href')).toBe('/app_icon-180.png')
  expect(sendToCEF).not.toHaveBeenCalled()
  await act(async () => root.unmount())

  window.localStorage.setItem('uam-companion-token', 'test-token')
  useAppStore.setState({ activeSessionId: 'phone-chat', lastAppliedStateRevision: 100 })
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { selectedChatId: 'desktop-chat', folders: [], stateRevision: revision } })
  const connectedRoot = createRoot(host)
  await act(async () => connectedRoot.render(<CompanionShell />))
  expect(useAppStore.getState().loadFromCef).toHaveBeenCalledWith({ selectedChatId: 'phone-chat', selectedChatIndex: -1, folders: [], resourceCollections: undefined, stateRevision: revision })
  expect(useAppStore.getState().lastAppliedStateRevision).toBe(-1)
  expect(host.querySelector('[aria-label="Activity"]')).not.toBeNull()
  vi.mocked(sendToCEF).mockClear()
  await act(async () => {
    host.querySelector('[aria-label="Settings"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  expect(host.textContent).toContain('Connected')
  await act(async () => {
    Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Refresh chats and activity')!.click()
  })
  expect(sendToCEF).toHaveBeenCalledWith({ action: 'getInitialState' })
  await act(async () => {
    host.querySelector('[aria-label="Chats"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  expect(host.textContent).toContain('Workspace chats')
  await act(async () => { host.querySelector('[aria-label="Settings"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
  await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Logout')!.click() })
  expect(window.localStorage.getItem('uam-companion-token')).toBeNull()
  expect(host.querySelector('input[type="password"]')).not.toBeNull()
  await act(async () => connectedRoot.unmount())
  host.remove()
})

it('skips unchanged poll snapshots and reloads changed or restarted boots', async () => {
  vi.useFakeTimers()
  window.localStorage.setItem('uam-companion-token', 'test-token')
  useAppStore.setState({
    activeSessionId: 'phone-chat',
    sessions: [{ id: 'phone-chat', name: 'Phone chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number]],
    loadSessionMessages: vi.fn(async (id: string) => { useAppStore.setState({ messages: { [id]: [] } }) }),
  })
  vi.mocked(sendToCEF)
    .mockResolvedValueOnce({ ok: true, data: { folders: [], stateRevision: 1, bootId: 'boot-a' } })
    .mockResolvedValueOnce({ ok: true, data: { unchanged: true, stateRevision: 1, bootId: 'boot-a' } })
    .mockResolvedValueOnce({ ok: true, data: { folders: [], stateRevision: 2, bootId: 'boot-a' } })
    .mockResolvedValueOnce({ ok: true, data: { folders: [], stateRevision: 0, bootId: 'boot-b' } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    expect(useAppStore.getState().loadFromCef).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().loadSessionMessages).toHaveBeenCalledTimes(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(useAppStore.getState().loadFromCef).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().loadSessionMessages).toHaveBeenCalledTimes(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(useAppStore.getState().loadFromCef).toHaveBeenCalledTimes(2)
    expect(useAppStore.getState().loadSessionMessages).toHaveBeenCalledTimes(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(useAppStore.getState().loadFromCef).toHaveBeenCalledTimes(3)
    expect(useAppStore.getState().loadSessionMessages).toHaveBeenCalledTimes(1)
  } finally {
    await act(async () => root.unmount())
    host.remove()
    vi.useRealTimers()
  }
})

it('refreshes an unchanged selected transcript while processing or streaming', async () => {
  vi.useFakeTimers()
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const loadSessionMessages = vi.fn(async (id: string) => { useAppStore.setState({ messages: { [id]: [] } }) })
  useAppStore.setState({
    activeSessionId: 'phone-chat',
    sessions: [{ id: 'phone-chat', name: 'Phone chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number]],
    loadSessionMessages,
    acpBindingBySessionId: { 'phone-chat': { processing: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] },
  })
  vi.mocked(sendToCEF)
    .mockResolvedValueOnce({ ok: true, data: { folders: [], stateRevision: 1, bootId: 'boot-a' } })
    .mockResolvedValue({ ok: true, data: { unchanged: true, stateRevision: 1, bootId: 'boot-a' } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(loadSessionMessages).toHaveBeenCalledTimes(2)
  } finally {
    await act(async () => root.unmount())
    host.remove()
    vi.useRealTimers()
  }
})

it('applies a forced full snapshot when its revision is unchanged', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1, bootId: 'boot-a' } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    useAppStore.setState({ lastAppliedStateRevision: 1 })
    await act(async () => {
      host.querySelector('[aria-label="Settings"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    const refresh = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Refresh chats and activity')
    await act(async () => refresh?.click())
    expect(useAppStore.getState().loadFromCef).toHaveBeenCalledTimes(2)
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(-1)
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('shows transcript failures and clears them after a successful refresh', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  useAppStore.setState({
    activeSessionId: 'phone-chat',
    sessions: [{ id: 'phone-chat', name: 'Phone chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number]],
    statusLine: 'Remote transcript could not be read.',
  })
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  vi.mocked(useAppStore.getState().loadSessionMessages).mockResolvedValueOnce(false).mockResolvedValue(undefined)
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    expect(host.querySelector('[role="alert"]')?.textContent).toBe('Remote transcript could not be read.')
    await act(async () => { document.dispatchEvent(new Event('visibilitychange')) })
    expect(host.querySelector('[role="alert"]')).toBeNull()
    await act(async () => { host.querySelector('[aria-label="Settings"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(host.textContent).toContain('Connected')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it.each([false, true])('chooses the initial tab from activity=%s and preserves later tab choice', async (hasActivity) => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  if (hasActivity) {
    useAppStore.setState({
      sessions: [{ id: 'active-chat', name: 'Active chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number]],
      acpBindingBySessionId: { 'active-chat': { processing: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] },
    })
  }
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    const activity = () => host.querySelector('button[aria-current="page"]')?.getAttribute('aria-label')
    expect(activity()).toBe(hasActivity ? 'Activity' : 'Chats')
    const manualChoice = hasActivity ? 'Chats' : 'Activity'
    await act(async () => { host.querySelector(`[aria-label="${manualChoice}"]`)?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(activity()).toBe(manualChoice)
    await act(async () => { host.querySelector('[aria-label="Settings"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Refresh chats and activity')?.click() })
    await act(async () => { host.querySelector(`[aria-label="${manualChoice}"]`)?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(activity()).toBe(manualChoice)
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('keeps Settings open when the initial activity refresh finishes', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  let resolveState!: (response: Awaited<ReturnType<typeof sendToCEF>>) => void
  vi.mocked(sendToCEF).mockReturnValue(new Promise((resolve) => { resolveState = resolve }))
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { host.querySelector('[aria-label="Settings"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    await act(async () => resolveState({ ok: true, data: { folders: [], stateRevision: 1 } }))
    expect(host.textContent).toContain('Connected')
    expect(host.querySelector('h1')?.textContent).toBe('Settings')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('shows live conversation status and calls the pin API from the header', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const setSessionPinned = vi.fn().mockResolvedValue(true)
  useAppStore.setState({
    activeSessionId: 'phone-chat',
    sessions: [{ id: 'phone-chat', name: 'Phone chat', isPinned: false } as ReturnType<typeof useAppStore.getState>['sessions'][number]],
    acpBindingBySessionId: { 'phone-chat': { processing: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] },
    setSessionPinned,
  })
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { host.querySelector('[data-session-id="phone-chat"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(host.querySelector('[aria-label="Agent running"]')).not.toBeNull()
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Pin chat')?.click() })
    expect(setSessionPinned).toHaveBeenCalledWith('phone-chat', true)
    await act(async () => { useAppStore.setState({ acpBindingBySessionId: {} }) })
    expect(host.querySelector('[aria-label="Idle"]')).not.toBeNull()
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('opens the shared new chat flow from the phone Chats tab', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const setNewChatModalOpen = vi.fn()
  useAppStore.setState({ isNewChatModalOpen: false, setNewChatModalOpen })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { host.querySelector('[aria-label="Chats"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    await act(async () => { host.querySelector('[aria-label="New chat"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(setNewChatModalOpen).toHaveBeenCalledWith(true)
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('runs a queued refresh after creating a chat during an in-flight poll', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  let resolvePoll!: (response: Awaited<ReturnType<typeof sendToCEF>>) => void
  const initialState = { ok: true as const, data: { folders: [], stateRevision: 1, bootId: 'boot-a' } }
  vi.mocked(sendToCEF)
    .mockResolvedValueOnce(initialState)
    .mockReturnValueOnce(new Promise((resolve) => { resolvePoll = resolve }))
    .mockResolvedValueOnce(initialState)
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { host.querySelector('[aria-label="Activity"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    await act(async () => { host.querySelector('[aria-label="Refresh activity"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    await act(async () => { useAppStore.setState({ isNewChatModalOpen: true }) })
    await act(async () => { host.querySelector('[role="dialog"] button')?.click() })
    expect(sendToCEF).toHaveBeenCalledTimes(2)
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Refresh activity"]')?.disabled).toBe(true)
    await act(async () => resolvePoll(initialState))
    expect(sendToCEF).toHaveBeenCalledTimes(3)
    expect(sendToCEF.mock.calls[2][0]).toEqual({ action: 'getInitialState' })
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Refresh activity"]')?.disabled).toBe(false)
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('filters activity by project and keeps review status above the completion time', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  const completedAt = new Date('2026-09-23T14:05:00')
  useAppStore.setState({
    folders: [
      { id: 'project-a', name: 'Project A', parentId: null, directory: '/tmp/a', isExpanded: true, createdAt: new Date() },
      { id: 'project-a-copy', name: 'Project A', parentId: null, directory: '/tmp/a-copy', isExpanded: true, createdAt: new Date() },
    ],
    sessions: [
      { id: 'running', name: 'Running task', folderId: 'project-a', providerId: 'codex-cli', workspaceDirectory: '/tmp/a/main', updatedAt: new Date(), viewMode: 'chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number],
      { id: 'review', name: 'Finished task', folderId: 'project-a', providerId: 'claude-cli', workspaceDirectory: '/tmp/a/review', updatedAt: completedAt, viewMode: 'chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number],
      { id: 'other', name: 'Other task', folderId: 'other', providerId: 'opencode-cli', workspaceDirectory: '/tmp/other', updatedAt: new Date(), viewMode: 'chat' } as ReturnType<typeof useAppStore.getState>['sessions'][number],
    ],
    acpBindingBySessionId: { running: { processing: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string], review: { readySinceLastSelect: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string], other: { readySinceLastSelect: true } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] },
  })
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    expect(host.textContent).toContain('Running task')
    expect(host.textContent).toContain('Finished task')
    expect(host.textContent).toContain('Other task')
    expect(host.querySelector('.uam-companion-refresh-stack [role="status"]')?.textContent).toMatch(/^Updated /)
    expect(Array.from(host.querySelectorAll('#companion-project-filter option')).filter((option) => option.textContent === 'Project A').map((option) => option.getAttribute('value'))).toEqual(['project-a', 'project-a-copy'])
    const reviewRow = host.querySelector('[data-session-id="review"]')!
    const expectedReviewTime = completedAt.toLocaleDateString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })
    expect(reviewRow.textContent?.indexOf('Ready to review')).toBeLessThan(reviewRow.textContent?.indexOf(expectedReviewTime) ?? -1)
    await act(async () => {
      const filter = host.querySelector<HTMLSelectElement>('#companion-project-filter')!
      filter.value = 'unsorted'
      filter.dispatchEvent(new Event('change', { bubbles: true }))
    })
    expect(host.textContent).toContain('Other task')
    expect(host.textContent).not.toContain('Running task')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('keeps one central new-chat control and slides the icon navigation selection', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    expect(host.querySelectorAll('[aria-label="New chat"]').length).toBe(1)
    expect(host.querySelector('[aria-label="Activity"] svg')).not.toBeNull()
    await act(async () => host.querySelector('[aria-label="Chats"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(host.querySelector('[aria-label="Activity"] svg')).not.toBeNull()
    expect(host.querySelector('[aria-label="Chats"] svg')).not.toBeNull()
    expect(host.querySelector('.uam-companion-nav-indicator--chats')).not.toBeNull()
    expect(host.querySelector('.uam-companion-list')?.textContent).not.toContain('New chat')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})

it('does not open an unrelated chat when the new-chat modal is cancelled', async () => {
  window.localStorage.setItem('uam-companion-token', 'test-token')
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    await act(async () => { useAppStore.setState({ isNewChatModalOpen: true, sessions: [] }) })
    await act(async () => root.render(<CompanionShell />))
    await act(async () => { useAppStore.setState({ isNewChatModalOpen: false, sessions: [{ id: 'unrelated', name: 'Unrelated' } as ReturnType<typeof useAppStore.getState>['sessions'][number]], activeSessionId: 'unrelated' }) })
    expect(host.textContent).not.toContain('Conversation')
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})


it('migrates the old login and reconnects after a fresh app launch', async () => {
  sessionStorage.setItem('uam-companion-token', 'existing-token')
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { folders: [], stateRevision: 1 } })
  const host = document.createElement('div')
  document.body.append(host)
  let root = createRoot(host)
  try {
    await act(async () => root.render(<CompanionShell />))
    expect(window.localStorage.getItem('uam-companion-token')).toBe('existing-token')
    expect(sessionStorage.getItem('uam-companion-token')).toBeNull()
    await act(async () => root.unmount())
    sessionStorage.clear()
    vi.mocked(sendToCEF).mockClear()
    root = createRoot(host)
    await act(async () => root.render(<CompanionShell />))
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'getInitialState' })
    expect(host.querySelector('input[type="password"]')).toBeNull()
  } finally {
    await act(async () => root.unmount())
    host.remove()
  }
})
