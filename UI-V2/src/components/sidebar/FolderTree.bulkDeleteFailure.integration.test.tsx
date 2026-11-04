import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'
import { FolderTree } from './FolderTree'

// Connected regression: synthetic chats and fake native callbacks only, no real deletion.
(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initialState = useAppStore.getState()
const now = new Date('2026-01-01T12:00:00.000Z')
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
let priorQuery: Window['cefQuery']
let pending: Query[]
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>
let failures: AppFailure[]
let originalMessages: typeof initialState.messages
const collectFailure = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
const requestIds = () => pending.map(({ request }) => (JSON.parse(request).payload as { chatIds: string[] }).chatIds)
const selectedIds = () => Array.from(host.querySelectorAll('[data-testid^="session-row-"][data-selected="true"]'))
  .map(row => row.getAttribute('data-session-id'))
const dialog = () => document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete 2 selected chats"]')!
const confirm = () => Array.from(dialog().querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Delete chats')!
const unchanged = () => {
  expect(useAppStore.getState().sessions.map(session => session.id)).toEqual(['chat-1', 'chat-2', 'chat-3'])
  expect(useAppStore.getState().activeSessionId).toBe('chat-1')
  expect(useAppStore.getState().messages).toBe(originalMessages)
  expect(selectedIds()).toEqual(['chat-1', 'chat-2'])
}

async function openConfirmation() {
  act(() => root.render(<FolderTree searchQuery="" />))
  act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')!.click())
  act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-2"]')!
    .dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
  expect(selectedIds()).toEqual(['chat-1', 'chat-2'])
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Delete 2 selected chats"]')!.click())
  expect(dialog().textContent).toContain('This cannot be undone')
  expect(pending).toHaveLength(0)
}

beforeEach(() => {
  // Standalone integration file: never replace deleteSessions or mock cefBridge.
  // This in-memory storage belongs solely to jsdom, including grid/draft cleanup.
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', {
    get length() { return values.size },
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value) },
    removeItem: (key: string) => { values.delete(key) },
    clear: () => values.clear(),
    key: (index: number) => [...values.keys()][index] ?? null,
  })
  priorQuery = window.cefQuery
  pending = []
  failures = []
  window.addEventListener(APP_FAILURE_EVENT, collectFailure)
  window.cefQuery = query => {
    const envelope = JSON.parse(query.request)
    expect(envelope.action).toBe('deleteSessions')
    pending.push(query)
  }
  vi.spyOn(console, 'error').mockImplementation(() => {})
  originalMessages = Object.fromEntries(['chat-1', 'chat-2', 'chat-3'].map(id => [id,
    [{ id: `message-${id}`, sessionId: id, role: 'user' as const, content: `fixture bytes ${id}`, createdAt: now }],
  ]))
  useAppStore.setState({
    ...initialState,
    sessions: [1, 2, 3].map(index => ({
      id: `chat-${index}`, name: `Chat ${index}`, viewMode: 'chat' as const, folderId: 'fixture-project',
      createdAt: new Date(now.getTime() - index * 60000), updatedAt: new Date(now.getTime() - index * 60000),
    })),
    folders: [{ id: 'fixture-project', name: 'Fixture project', parentId: null, directory: '/fixture-only/project', isExpanded: true, createdAt: now }],
    activeSessionId: 'chat-1', messages: originalMessages,
    cliBindingBySessionId: {}, acpBindingBySessionId: {}, cliTranscriptBySessionId: {},
    resourceCollections: [],
    executionHosts: [{ id: 'local', label: 'Fixture computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' }],
    isNewChatModalOpen: false, newChatFolderId: null,
  }, true)
  expect(vi.isMockFunction(useAppStore.getState().deleteSessions)).toBe(false)
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
})

afterEach(async () => {
  // Resolve any unconsumed fixture callbacks even when a safety assertion fails.
  await act(async () => {
    pending.forEach(query => query.onFailure(499, 'Fixture cleanup; no real transport exists.'))
    await Promise.resolve()
  })
  act(() => root.unmount())
  host.remove()
  window.removeEventListener(APP_FAILURE_EVENT, collectFailure)
  if (priorQuery) window.cefQuery = priorQuery
  else delete window.cefQuery
  useAppStore.setState(initialState, true)
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

describe('connected bulk chat deletion failure', () => {
  it('retains exact selection and bytes after native refusal, then deletes only after explicit retry acknowledgement', async () => {
    await openConfirmation()
    await act(async () => { confirm().click(); await Promise.resolve() })
    expect(requestIds()).toEqual([['chat-1', 'chat-2']])
    unchanged()
    await act(async () => pending[0].onFailure(409, 'Fixture deletion refused: a selected chat is running.'))
    expect(dialog().querySelector('[role="alert"]')?.textContent).toContain('could not be deleted')
    unchanged()
    await act(async () => { await Promise.resolve(); await Promise.resolve() })
    expect(pending).toHaveLength(1) // No automatic replay after a definitive refusal.

    await act(async () => { confirm().click(); await Promise.resolve() })
    expect(requestIds()).toEqual([['chat-1', 'chat-2'], ['chat-1', 'chat-2']])
    unchanged() // No optimistic deletion before the second native acknowledgement.
    await act(async () => pending[1].onSuccess(JSON.stringify({ deletedChatIds: ['chat-1', 'chat-2'], selectedChatId: 'chat-3' })))
    expect(useAppStore.getState().sessions.map(session => session.id)).toEqual(['chat-3'])
    expect(useAppStore.getState().messages).toEqual({ 'chat-3': originalMessages['chat-3'] })
    expect(useAppStore.getState().activeSessionId).toBe('chat-3')
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete 2 selected chats"]')).toBeNull()
    expect(selectedIds()).toEqual([])
    expect(pending).toHaveLength(2)
  })

  it('reports the actual refusal through the shared failure route without discarding the retry selection', async () => {
    await openConfirmation()
    await act(async () => { confirm().click(); await Promise.resolve() })
    await act(async () => pending[0].onFailure(409, 'Fixture deletion refused: a selected chat is running.'))
    unchanged()
    expect(failures).toHaveLength(1)
    expect(`${failures[0]?.message} ${failures[0]?.detail}`).toContain('Fixture deletion refused: a selected chat is running.')
    act(() => Array.from(dialog().querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Cancel')!.click())
    expect(selectedIds()).toEqual(['chat-1', 'chat-2'])
    expect(failures).toHaveLength(1) // Dialog dismissal cannot retract an app-wide failure.
    expect(pending).toHaveLength(1)
  })

  it('cannot issue a second destructive request while the first confirmation is unresolved', async () => {
    await openConfirmation()
    const button = confirm()
    await act(async () => { button.click(); await Promise.resolve() })
    unchanged()
    act(() => button.click())
    expect(pending).toHaveLength(1)
    await act(async () => pending[0].onFailure(409, 'Fixture deletion refused: a selected chat is running.'))
    unchanged()
    await act(async () => { confirm().click(); await Promise.resolve() })
    expect(requestIds()).toEqual([['chat-1', 'chat-2'], ['chat-1', 'chat-2']])
  })
  it('coalesces repeated confirmation events before React can render busy state', async () => {
    await openConfirmation()
    const button = confirm()
    await act(async () => { button.click(); button.click(); await Promise.resolve() })
    expect(pending).toHaveLength(1)
    unchanged()
  })

  it('keeps the pending confirmation and retry selection through Cancel and Escape', async () => {
    await openConfirmation()
    await act(async () => { confirm().click(); await Promise.resolve() })
    expect(dialog().getAttribute('aria-busy')).toBe('true')
    const cancel = Array.from(dialog().querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Cancel')!
    expect(cancel.disabled).toBe(true)
    act(() => cancel.click())
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
    expect(dialog()).not.toBeNull()
    unchanged()
    expect(pending).toHaveLength(1)
    await act(async () => pending[0].onFailure(409, 'Fixture deletion refused: a selected chat is running.'))
    expect(dialog().getAttribute('aria-busy')).toBe('false')
    expect(confirm().disabled).toBe(false)
    unchanged()
  })

})
