import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'
import { FolderTree } from './FolderTree'

// Every multi-selection action reaches each selected row through the real store and a fake native bridge.
(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initialState = useAppStore.getState()
const now = new Date('2026-01-01T12:00:00.000Z')
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
type Request = { action: string; payload: Record<string, unknown> }
let requests: Request[]
let held: Query[]
let failures: AppFailure[]
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>
const collectFailure = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
const sent = (action: string) => requests.filter((request) => request.action === action).map((request) => request.payload)
const row = (id: string) => host.querySelector<HTMLElement>(`[data-testid="session-row-${id}"]`)!
const selectedIds = () => Array.from(host.querySelectorAll('[data-testid^="session-row-"][data-selected="true"]')).map((element) => element.getAttribute('data-session-id'))
const menu = (label: string) => document.body.querySelector<HTMLElement>(`[role="menu"][aria-label="${label}"]`)
const item = (text: string) => Array.from(document.body.querySelectorAll<HTMLElement>('[role="menuitem"]')).find((element) => element.textContent?.trim().startsWith(text))!
const flush = async () => { for (let index = 0; index < 8; index++) await Promise.resolve() }

function chat(id: string, folderId = 'project', extra: Record<string, unknown> = {}) {
  return { id, name: `Chat ${id}`, viewMode: 'chat' as const, folderId, createdAt: now, updatedAt: now, ...extra }
}

function selectChats(first: string, last: string) {
  act(() => row(first).click())
  act(() => row(last).dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
}

function openChatMenu(id: string) {
  act(() => row(id).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
}

beforeEach(() => {
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', {
    get length() { return values.size },
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value) },
    removeItem: (key: string) => { values.delete(key) },
    clear: () => values.clear(),
    key: (index: number) => [...values.keys()][index] ?? null,
  })
  vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
  requests = []
  held = []
  failures = []
  window.addEventListener(APP_FAILURE_EVENT, collectFailure)
  window.cefQuery = (query) => {
    const request = JSON.parse(query.request) as Request
    requests.push(request)
    if (request.action === 'deleteSessions' || request.action === 'deleteFolders') held.push(query)
    else if (request.action === 'regenerateChatTitle') query.onSuccess(JSON.stringify({ chatId: request.payload.chatId, title: 'New title' }))
    else query.onSuccess('{}')
  }
  vi.spyOn(console, 'error').mockImplementation(() => {})
  useAppStore.setState({
    ...initialState,
    sessions: [chat('a'), chat('a-branch', 'project', { parentChatId: 'a', branchRootChatId: 'a' }), chat('b'), chat('c'), chat('keep')],
    folders: [
      { id: 'project', name: 'Project', parentId: null, directory: '/fixture/project', isExpanded: true, createdAt: now },
      { id: 'w1', name: 'Workspace one', parentId: null, directory: '/fixture/one', isExpanded: true, createdAt: now },
      { id: 'w2', name: 'Workspace two', parentId: null, directory: '/fixture/two', isExpanded: true, createdAt: now },
    ],
    activeSessionId: 'keep',
    cliBindingBySessionId: {}, acpBindingBySessionId: {}, cliTranscriptBySessionId: {},
    resourceCollections: [{ id: 'col', name: 'Favourites', isExpanded: true, references: [] }, { id: 'col2', name: 'Archive', isExpanded: true, references: [] }],
    executionHosts: [{ id: 'local', label: 'Fixture computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' }],
    isNewChatModalOpen: false, newChatFolderId: null,
  } as never, true)
  host = document.createElement('div')
  document.body.append(host)
  root = createRoot(host)
  act(() => root.render(<FolderTree searchQuery="" />))
})

afterEach(async () => {
  await act(async () => { held.forEach((query) => query.onFailure(499, 'Fixture cleanup.')); await flush() })
  act(() => root.unmount())
  host.remove()
  window.removeEventListener(APP_FAILURE_EVENT, collectFailure)
  delete window.cefQuery
  useAppStore.setState(initialState, true)
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

describe('chat multi-selection actions', () => {
  it('deletes every selected chat and its branches from the row menu with one confirmation, leaving the rest', async () => {
    selectChats('a', 'c')
    expect(selectedIds()).toEqual(['a', 'b', 'c'])
    openChatMenu('b')
    expect(menu('Actions for 3 selected chats')).not.toBeNull()
    act(() => item('Delete 3 chats').click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete 3 selected chats"]')!
    expect(dialog.textContent).toContain('3 chats and 1 related branches will be permanently deleted')
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Delete chats')!.click(); await flush() })
    expect(sent('deleteSessions')).toEqual([{ chatIds: ['a', 'a-branch', 'b', 'c'] }])
    await act(async () => { held[0].onSuccess(JSON.stringify({ deletedChatIds: ['a', 'a-branch', 'b', 'c'], rejected: [], selectedChatId: 'keep' })); await flush() })
    expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['keep'])
    expect(failures).toEqual([])
    expect(selectedIds()).toEqual([])
  })

  it('routes the Delete key on a selected row to the whole selection', () => {
    selectChats('b', 'c')
    act(() => { row('c').focus(); row('c').dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, key: 'Delete' })) })
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete 2 selected chats"]')).not.toBeNull()
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete Chat c"]')).toBeNull()
  })

  it('deletes the chats native accepted, reports each refused chat by name and keeps it selected', async () => {
    selectChats('b', 'c')
    openChatMenu('c')
    act(() => item('Delete 2 chats').click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete 2 selected chats"]')!
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Delete chats')!.click(); await flush() })
    await act(async () => {
      held[0].onSuccess(JSON.stringify({ deletedChatIds: ['b'], rejected: [{ id: 'c', error: 'Cannot delete chats while a selected runtime is still running.' }], selectedChatId: 'keep' }))
      await flush()
    })
    expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['a', 'a-branch', 'c', 'keep'])
    expect(failures).toHaveLength(1)
    expect(failures[0].message).toBe('Could not delete 1 of 2 selected chats.')
    expect(failures[0].detail).toBe('“Chat c”: Cannot delete chats while a selected runtime is still running.')
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete 2 selected chats"]')).toBeNull()
    expect(selectedIds()).toEqual(['c'])
  })

  it('pins every selected chat, then unpins them all', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    await act(async () => { item('Pin 2 chats').click(); await flush() })
    expect(sent('setChatPinned')).toEqual([{ chatId: 'b', pinned: true }, { chatId: 'c', pinned: true }])
    expect(useAppStore.getState().sessions.filter((session) => session.isPinned).map((session) => session.id)).toEqual(['b', 'c'])
    openChatMenu('b')
    await act(async () => { item('Unpin 2 chats').click(); await flush() })
    expect(sent('setChatPinned').slice(2)).toEqual([{ chatId: 'b', pinned: false }, { chatId: 'c', pinned: false }])
  })

  it('copies every selected chat ID', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    act(() => item('Copy').click())
    await act(async () => { item('Chat IDs').click(); await flush() })
    expect(sent('writeClipboardText')).toEqual([{ text: 'b\nc' }])
  })

  it('snoozes every selected chat', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    act(() => item('Snooze').click())
    await act(async () => { item('1 hour').click(); await flush() })
    expect(sent('setChatSnoozedUntil').map((payload) => payload.chatId)).toEqual(['b', 'c'])
  })

  it('moves every selected chat to a collection', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    act(() => item('Move to collection').click())
    await act(async () => { item('Favourites').click(); await flush() })
    expect(sent('applyOrganizationAction').map((payload) => payload.target)).toEqual(['b', 'c'])
  })

  it('regenerates the title of every selected chat', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    await act(async () => { item('Regenerate 2 titles').click(); await flush() })
    expect(sent('regenerateChatTitle')).toEqual([{ chatId: 'b' }, { chatId: 'c' }])
  })

  it('sets one icon on every selected chat', async () => {
    selectChats('b', 'c')
    openChatMenu('b')
    act(() => item('Icon for 2 chats').click())
    const dialog = document.body.querySelector<HTMLElement>('[role="dialog"][aria-label="Icon for 2 chats"]')!
    act(() => Array.from(dialog.querySelectorAll<HTMLButtonElement>('[role="tab"]')).find((tab) => tab.textContent === 'Emoji')!.click())
    const input = dialog.querySelector<HTMLInputElement>('input[aria-label="Icon character"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '⭐')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Save')!.click(); await flush() })
    expect(sent('setCustomIcon').map((payload) => payload.targetId)).toEqual(['b', 'c'])
  })

  it('keeps single-chat actions visible but disabled with an explanation, and never applies them to one row', () => {
    selectChats('b', 'c')
    openChatMenu('b')
    for (const label of ['New chat in this worktree', 'Rename']) {
      const element = item(label)
      expect(element.getAttribute('aria-disabled')).toBe('true')
      expect(document.getElementById(element.getAttribute('aria-describedby')!)?.textContent).toBe('Not available for multiple selection — select a single chat')
    }
    act(() => item('Rename').click())
    expect(host.querySelector('input')).toBeNull()
    act(() => { row('b').focus(); row('b').dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, key: 'F2' })) })
    expect(host.querySelector('input')).toBeNull()
    expect(useAppStore.getState().isNewChatModalOpen).toBe(false)
  })

  it('applies the row pin and wake buttons to the whole selection', async () => {
    selectChats('b', 'c')
    await act(async () => { host.querySelector<HTMLButtonElement>('[data-testid="session-actions-b"] button[aria-label="Pin selected chats"]')!.click(); await flush() })
    expect(sent('setChatPinned')).toEqual([{ chatId: 'b', pinned: true }, { chatId: 'c', pinned: true }])
    const later = Date.now() + 3_600_000
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ['b', 'c'].includes(session.id) ? { ...session, snoozedUntilMs: later } : session) })))
    const wake = host.querySelector<HTMLButtonElement>('[data-testid="session-actions-c"] button[aria-label="Wake selected chats"]')!
    await act(async () => { wake.click(); await flush() })
    expect(sent('setChatSnoozedUntil')).toEqual([{ chatId: 'b', untilMs: 0 }, { chatId: 'c', untilMs: 0 }])
  })

  it('keeps the ordinary single-chat menu when only one row is selected', () => {
    openChatMenu('b')
    expect(menu('Actions for Chat b')).not.toBeNull()
    expect(item('Rename').getAttribute('aria-disabled')).toBeNull()
  })
})

describe('workspace multi-selection actions', () => {
  function selectWorkspaces() {
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-w1"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-w2"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-w2"]')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
  }

  it('disables single-workspace actions with an explanation and applies the rest to every workspace', async () => {
    selectWorkspaces()
    for (const label of ['New chat', 'Project memory', 'Review native chats', 'Change host icon', 'Rename folder']) {
      const element = item(label)
      expect(element.getAttribute('aria-disabled')).toBe('true')
      expect(document.getElementById(element.getAttribute('aria-describedby')!)?.textContent).toBe('Not available for multiple selection — select a single workspace')
    }
    act(() => item('Move to collection').click())
    await act(async () => { item('Favourites').click(); await flush() })
    expect(sent('applyOrganizationAction').map((payload) => payload.target)).toEqual(['w1', 'w2'])
  })

  it('deletes accepted workspaces and reports each refused workspace by name', async () => {
    selectWorkspaces()
    act(() => item('Delete 2 workspaces').click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete selected workspaces"]')!
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Delete workspaces')!.click(); await flush() })
    expect(sent('deleteFolders')).toEqual([{ folderIds: ['w1', 'w2'] }])
    await act(async () => {
      held[0].onSuccess(JSON.stringify({ deletedChatIds: [], deletedFolderIds: ['w1'], rejected: [{ id: 'w2', error: 'Wait for the chat worktree operation to finish.' }], selectedChatId: 'keep' }))
      await flush()
    })
    expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['project', 'w2'])
    expect(failures.map((failure) => [failure.message, failure.detail])).toEqual([['Could not delete 1 of 2 selected workspaces.', '“Workspace two”: Wait for the chat worktree operation to finish.']])
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete selected workspaces"]')).toBeNull()
  })
})

describe('collection multi-selection actions', () => {
  it('disables rename with an explanation and deletes every selected collection after one confirmation', async () => {
    act(() => host.querySelector<HTMLElement>('[data-collection-id="col"] button[aria-label="Favourites"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLElement>('[data-collection-id="col2"] button[aria-label="Archive"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(host.textContent).toContain('2 collections selected')
    act(() => host.querySelector<HTMLElement>('[data-collection-id="col2"]')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
    const rename = item('Rename')
    expect(rename.getAttribute('aria-disabled')).toBe('true')
    expect(document.getElementById(rename.getAttribute('aria-describedby')!)?.textContent).toBe('Not available for multiple selection — select a single collection')
    act(() => rename.click())
    expect(host.querySelector('input[aria-label^="Rename"]')).toBeNull()
    act(() => item('Delete 2 collections').click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete 2 selected collections"]')!
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Delete collections')!.click(); await flush() })
    expect(sent('deleteResourceCollection')).toEqual([{ collectionId: 'col' }, { collectionId: 'col2' }])
    expect(useAppStore.getState().resourceCollections).toEqual([])
  })

  it('keeps a plain click as expand or collapse without selecting', async () => {
    await act(async () => { host.querySelector<HTMLElement>('[data-collection-id="col"] button[aria-label="Favourites"]')!.click(); await flush() })
    expect(sent('toggleResourceCollection')).toEqual([{ collectionId: 'col' }])
    expect(host.textContent).not.toContain('collections selected')
  })
})
