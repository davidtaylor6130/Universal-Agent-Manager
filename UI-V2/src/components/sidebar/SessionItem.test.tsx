import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type AcpAttentionKind, type AcpBinding } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { SessionItem } from './SessionItem'
import { setPreferredFamilyChat } from '../../utils/branchPreferenceStorage'
import { chatGridLeaves, defaultChatGridLayout, readChatGridLayout, setChatInLeaf, splitChatLeaf, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const originalActions = { setActiveSession: useAppStore.getState().setActiveSession, setSessionPinned: useAppStore.getState().setSessionPinned, moveResourceToCollection: useAppStore.getState().moveResourceToCollection }

const now = new Date('2026-01-01T12:00:00.000Z')

function makeSession(): Session {
  return {
    id: 'chat-1',
    name: 'Chat 1',
    viewMode: 'chat',
    folderId: 'project',
    createdAt: now,
    updatedAt: now,
    lastOpenedAt: now,
  }
}

function makeAcpBinding(overrides: Partial<AcpBinding> = {}): AcpBinding {
  return {
    sessionId: 'native-1',
    providerId: 'gemini-cli',
    protocolKind: 'gemini-acp',
    threadId: '',
    running: true,
    lifecycleState: 'ready',
    processing: false,
    readySinceLastSelect: false,
    attentionKind: null,
    processingStartedAtMs: null,
    lastError: '',
    recentStderr: '',
    lastExitCode: null,
    diagnostics: [],
    toolCalls: [],
    planSummary: '',
    planEntries: [],
    availableModes: [],
    currentModeId: 'default',
    availableModels: [],
    currentModelId: '',
    turnEvents: [],
    turnUserMessageIndex: -1,
    turnAssistantMessageIndex: -1,
    turnSerial: 0,
    pendingPermission: null,
    pendingUserInput: null,
    agentInfo: null,
    ...overrides,
  }
}

/** Opens the Copy submenu from the open context menu and returns its element. */
function openCopySubmenu(menu: Element) {
  const copy = Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === 'Copy')!
  act(() => copy.click())
  return document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Copy"]')!
}

function renderSessionItem(activityLayout = false) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<SessionItem sessionId="chat-1" activityLayout={activityLayout} />)
  })

  return { host, root }
}

describe('SessionItem status icons', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState({
      ...originalActions,
      folders: [],
      sessions: [makeSession()],
      activeSessionId: 'chat-2',
      messages: {},
      cliBindingBySessionId: {},
      acpBindingBySessionId: {},
      cliTranscriptBySessionId: {},
      isNewChatModalOpen: false,
      newChatFolderId: null,
      resourceCollections: [],
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: true,
    })
  })

  it('refreshes activity branches when workspace scope changes and ignores late replies', async () => {
    const original = useAppStore.getState().getVcsCommitStatus
    type Status = Awaited<ReturnType<typeof original>>
    const finish: Array<(status: Status) => void> = []
    const getVcsCommitStatus = vi.fn(() => new Promise<Status>(resolve => finish.push(resolve)))
    const status = (branchOrRevision: string): Status => ({ available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/tmp', branchOrRevision, changedFiles: [], lineStatsReady: false, warning: '', error: '' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    let session = { ...makeSession(), id: 'branch-scope-test', workspaceDirectory: '/tmp/first', executionHostId: 'local' }
    useAppStore.setState({ sessions: [session], getVcsCommitStatus })
    const render = () => act(() => root.render(<SessionItem sessionId={session.id} session={session} activityLayout />))
    try {
      render()
      await act(async () => finish[0](status('old-branch')))
      expect(host.textContent).toContain('old-branch')
      session = { ...session, workspaceDirectory: '/tmp/second' }
      render()
      expect(getVcsCommitStatus).toHaveBeenCalledTimes(2)
      expect(host.textContent).not.toContain('old-branch')
      session = { ...session, executionHostId: 'remote' }
      render()
      expect(getVcsCommitStatus).toHaveBeenCalledTimes(3)
      await act(async () => finish[2](status('remote-branch')))
      await act(async () => finish[1](status('late-local-branch')))
      expect(host.textContent).toContain('remote-branch')
      expect(host.textContent).not.toContain('late-local-branch')
      act(() => root.unmount())
      const remounted = createRoot(host)
      await act(async () => remounted.render(<SessionItem sessionId={session.id} session={session} activityLayout />))
      expect(host.textContent).toContain('remote-branch')
      act(() => remounted.unmount())
    } finally {
      useAppStore.setState({ getVcsCommitStatus: original })
      host.remove()
    }
  })

  it('requests activity branches only while their row intersects the viewport', async () => {
    const original = useAppStore.getState().getVcsCommitStatus
    type Status = Awaited<ReturnType<typeof original>>
    let intersect!: (entries: Pick<IntersectionObserverEntry, 'isIntersecting'>[]) => void
    const disconnect = vi.fn()
    vi.stubGlobal('IntersectionObserver', class {
      constructor(callback: typeof intersect) { intersect = callback }
      observe() {}
      disconnect = disconnect
    })
    const finish: Array<(status: Status) => void> = []
    const getVcsCommitStatus = vi.fn(() => new Promise<Status>(resolve => finish.push(resolve)))
    let session = { ...makeSession(), id: 'branch-visible-test', workspaceDirectory: '/tmp/visible' }
    useAppStore.setState({ sessions: [session], getVcsCommitStatus })
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    const render = () => act(() => root.render(<SessionItem sessionId={session.id} session={session} activityLayout />))
    try {
      render()
      expect(getVcsCommitStatus).not.toHaveBeenCalled()
      act(() => intersect([{ isIntersecting: true }]))
      expect(getVcsCommitStatus).toHaveBeenCalledTimes(1)
      act(() => intersect([{ isIntersecting: false }]))
      await act(async () => finish[0]({ available: true, activeVcsType: 'git', branchOrRevision: 'late-hidden-branch' } as Status))
      expect(host.textContent).not.toContain('late-hidden-branch')
      session = { ...session, updatedAt: new Date(now.getTime() + 1000) }
      render()
      expect(getVcsCommitStatus).toHaveBeenCalledTimes(1)
      act(() => intersect([{ isIntersecting: true }]))
      expect(getVcsCommitStatus).toHaveBeenCalledTimes(2)
      await act(async () => finish[1]({ available: true, activeVcsType: 'git', branchOrRevision: 'visible-branch' } as Status))
      expect(host.textContent).toContain('visible-branch')
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.unstubAllGlobals()
      useAppStore.setState({ getVcsCommitStatus: original })
    }
    expect(disconnect).toHaveBeenCalled()
  })

  it('does not request an activity branch for imported read-only chats', async () => {
    const original = useAppStore.getState().getVcsCommitStatus
    const getVcsCommitStatus = vi.fn(async () => null)
    useAppStore.setState({ sessions: [{ ...makeSession(), importedReadOnly: true, workspaceBranchName: 'saved-branch' }], getVcsCommitStatus })
    const { host, root } = renderSessionItem(true)
    try {
      await act(async () => {})
      expect(getVcsCommitStatus).not.toHaveBeenCalled()
      expect(host.textContent).not.toContain('saved-branch')
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ getVcsCommitStatus: original })
    }
  })

  it('uses a precomputed branch family without rescanning the full session list', () => {
    const sessions = new Proxy([makeSession()], {
      get(target, property, receiver) {
        if (property === 'filter') throw new Error('session list was rescanned')
        return Reflect.get(target, property, receiver)
      },
    })
    useAppStore.setState({ sessions })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<SessionItem sessionId="chat-1" session={makeSession()} familySessionIds={['chat-1']} />))
    expect(host.textContent).toContain('Chat 1')

    act(() => root.unmount())
    host.remove()
  })

  it.each([false, true])('updates provider icons through a switch and rollback despite an old runtime (activity=%s)', async (activityLayout) => {
    useAppStore.setState({
      providers: [
        { id: 'opencode-cli', name: 'OpenCode CLI', shortName: 'OpenCode', color: '#fff', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#fff', description: '' },
      ],
      sessions: [{ ...makeSession(), providerId: 'opencode-cli' }],
      acpBindingBySessionId: { 'chat-1': makeAcpBinding({ providerId: 'opencode-cli' }) },
    })
    const host = document.createElement('div'), root = createRoot(host)
    function Row() {
      const session = useAppStore(state => state.sessions.find(item => item.id === 'chat-1'))
      return <SessionItem sessionId="chat-1" session={session} familySessionIds={['chat-1']} activityLayout={activityLayout} />
    }
    let reject!: () => void
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action === 'setChatProvider') reject = () => onFailure(500, 'Provider switch failed')
      else onSuccess('{}')
    }
    try {
      await act(async () => root.render(<Row />))
      expect(host.querySelector('[aria-label="Provider: OpenCode"]')).toBeTruthy()
      let switching!: Promise<boolean>
      act(() => { switching = useAppStore.getState().setSessionProvider('chat-1', 'codex-cli') })
      expect(host.querySelector('[aria-label="Provider: Codex"]')).toBeTruthy()
      expect(host.querySelector('[aria-label="Provider: OpenCode"]')).toBeNull()
      act(() => useAppStore.setState({ acpBindingBySessionId: { 'chat-1': makeAcpBinding({ providerId: 'opencode-cli', lastError: 'Old runner exited' }) } }))
      expect(host.querySelector('[aria-label="Provider: Codex"]')).toBeTruthy()
      await act(async () => { reject(); expect(await switching).toBe(false) })
      expect(host.querySelector('[aria-label="Provider: OpenCode"]')).toBeTruthy()
      expect(host.querySelector('[aria-label="Provider: Codex"]')).toBeNull()
      delete window.cefQuery
      await act(async () => { expect(await useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).toBe(true) })
      expect(host.querySelector('[aria-label="Provider: Codex"]')).toBeTruthy()
    } finally { delete window.cefQuery; act(() => root.unmount()) }
  })

  it.each([false, true])('regenerates titles without duplicate requests or an optimistic rename (activity=%s)', async (activityLayout) => {
    let finish: ((response: string) => void) | undefined
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const message = JSON.parse(request)
      if (message.action === 'regenerateChatTitle') { requests.push(message.payload); finish = onSuccess }
      else onSuccess('{}')
    }
    const { host, root } = renderSessionItem(activityLayout)
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
      const menu = document.body.querySelector('[data-viewport-menu]')!
      const button = Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === 'Regenerate title')!
      await act(async () => button.click())
      expect(button.disabled).toBe(true)
      expect(button.textContent).toBe('Generating title…')
      await act(async () => button.click())
      expect(requests).toEqual([{ chatId: 'chat-1' }])
      expect(useAppStore.getState().sessions[0].name).toBe('Chat 1')
      // Simulate the authoritative state push before the request completion.
      await act(async () => {
        useAppStore.setState({ sessions: [{ ...makeSession(), name: 'Generated title' }] })
        finish!(JSON.stringify({ chatId: 'chat-1', title: 'Generated title' }))
      })
      expect(menu.querySelector('[role="status"]')?.textContent).toBe('Chat title updated.')
      expect(useAppStore.getState().sessions[0].name).toBe('Generated title')
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it('cancels a pending title request and keeps the existing name on failure', async () => {
    let fail: ((code: number, message: string) => void) | undefined
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const message = JSON.parse(request)
      requests.push(message.action)
      if (message.action === 'regenerateChatTitle') fail = onFailure
      else onSuccess('{}')
    }
    const { host, root } = renderSessionItem()
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
      const menu = document.body.querySelector('[data-viewport-menu]')!
      const button = (name: string) => Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === name)!
      await act(async () => button('Regenerate title').click())
      await act(async () => button('Cancel title generation').click())
      expect(button('Cancelling…').disabled).toBe(true)
      expect(requests.filter(action => action === 'cancelChatTitle')).toHaveLength(1)
      await act(async () => fail!(409, 'Title generation was cancelled.'))
      expect(menu.querySelector('[role="alert"]')?.textContent).toBe('Title generation was cancelled.')
      expect(button('Regenerate title').disabled).toBe(false)
      expect(useAppStore.getState().sessions[0].name).toBe('Chat 1')
      await act(async () => button('Regenerate title').click())
      await act(async () => fail!(409, 'The chat changed. Its current name was kept.'))
      expect(menu.querySelector('[role="alert"]')?.textContent).toContain('current name was kept')
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it('rejects invalid title responses and explains preview availability', async () => {
    const { host, root } = renderSessionItem()
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
      const menu = document.body.querySelector('[data-viewport-menu]')!
      const button = Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === 'Regenerate title')!
      await act(async () => button.click())
      expect(menu.querySelector('[role="alert"]')?.textContent).toContain('desktop app')
      window.cefQuery = ({ request, onSuccess }) => onSuccess(JSON.parse(request).action === 'regenerateChatTitle' ? JSON.stringify({ chatId: 'another-chat', title: 'Wrong title' }) : '{}')
      await act(async () => button.click())
      expect(menu.querySelector('[role="alert"]')?.textContent).toContain('invalid response')
      expect(useAppStore.getState().sessions[0].name).toBe('Chat 1')
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it.each([false, true])('copies the exact chat name and ID from the context menu (activity=%s)', async (activityLayout) => {
    const copied: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const message = JSON.parse(request)
      if (message.action === 'writeClipboardText') copied.push(message.payload.text)
      onSuccess('{}')
    }
    const { host, root } = renderSessionItem(activityLayout)
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
      const submenu = openCopySubmenu(document.body.querySelector('[data-viewport-menu]')!)
      const button = (name: string) => Array.from(submenu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === name)!
      await act(async () => button('Chat name').click())
      expect(copied).toEqual(['Chat 1'])
      expect(submenu.querySelector('[role="status"]')?.textContent).toBe('Chat name copied.')
      await act(async () => button('Chat ID').click())
      expect(copied).toEqual(['Chat 1', 'chat-1'])
      expect(submenu.querySelector('[role="status"]')?.textContent).toBe('Chat ID copied.')
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it('keeps copy failures visible and allows retry without modifying the chat', async () => {
    const writeText = vi.fn().mockRejectedValueOnce(new Error('Clipboard unavailable')).mockResolvedValueOnce(undefined)
    const previous = Object.getOwnPropertyDescriptor(navigator, 'clipboard')
    Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText } })
    const { host, root } = renderSessionItem()
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
      const submenu = openCopySubmenu(document.body.querySelector('[data-viewport-menu]')!)
      const button = Array.from(submenu.querySelectorAll<HTMLButtonElement>('button')).find(item => item.textContent === 'Chat ID')!
      await act(async () => button.click())
      expect(submenu.querySelector('[role="alert"]')?.textContent).toBe('Could not copy. Try again.')
      expect(button.disabled).toBe(false)
      await act(async () => button.click())
      expect(submenu.querySelector('[role="status"]')?.textContent).toBe('Chat ID copied.')
      expect(useAppStore.getState().sessions[0].name).toBe('Chat 1')
    } finally {
      act(() => root.unmount()); host.remove()
      if (previous) Object.defineProperty(navigator, 'clipboard', previous)
      else Reflect.deleteProperty(navigator, 'clipboard')
    }
  })

  it('shows provider and shortened worktree context when enabled', () => {
    useAppStore.setState({
      sessions: [{
        ...makeSession(),
        providerId: 'codex-cli',
        workspaceWorktreeDirectory: '/Users/david/project/.uam-worktrees/chat-abc123',
        workspaceSourceDirectory: '/Users/david/project',
        workspaceBranchName: 'fix-sidebar',
      }],
    })
    const { host, root } = renderSessionItem()

    expect(host.querySelector('img.uam-provider-logo--codex')?.parentElement?.style.width).toBe('16px')
    expect(host.querySelector('[role="img"][aria-label="Provider: Codex"]')).toBeTruthy()
    expect(host.textContent).toContain('projectfix-sidebar')
    expect(host.textContent).not.toContain('.uam-worktrees')
    expect(host.querySelector('[title="/Users/david/project/.uam-worktrees/chat-abc123"]')).toBeTruthy()

    act(() => useAppStore.setState({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: false }))
    expect(host.querySelector('img.uam-provider-logo--codex')).toBeNull()
    expect(host.textContent).not.toContain('.uam-worktrees')

    act(() => root.unmount())
    host.remove()
  })

  it('uses the original workspace label and logical worktree name in both sidebar layouts', () => {
    const worktree = { ...makeSession(), executionHostId: 'windows', workspaceSourceDirectory: 'C:\\Projects\\UAM\\', workspaceWorktreeDirectory: 'C:\\Temp\\chat-random', workspaceBranchName: 'fix-folders' }
    useAppStore.setState({
      sessions: [worktree],
      folders: [
        { id: 'other', name: 'Wrong machine', directory: 'C:/Projects/UAM', executionHostId: 'local', parentId: null, isExpanded: true, createdAt: now },
        { id: 'project', name: 'Original UAM', directory: 'c:/projects/uam', executionHostId: 'windows', parentId: null, isExpanded: true, createdAt: now },
      ],
    })
    for (const activityLayout of [false, true]) {
      const { host, root } = renderSessionItem(activityLayout)
      expect(host.textContent).toContain('Original UAMfix-folders')
      expect(host.textContent).not.toContain('chat-random')
      const context = Array.from(host.querySelectorAll<HTMLElement>('[title]')).find((element) => element.title === worktree.workspaceWorktreeDirectory)
      expect(context?.querySelector('svg')).toBeTruthy()
      act(() => root.unmount())
      host.remove()
    }
  })

  it('acknowledges the rendered completion only on deliberate mouse or keyboard selection', () => {
    const original = useAppStore.getState().acknowledgeChatAttention
    const acknowledge = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ sessions: [{ ...makeSession(), attentionRevision: 'rendered-completion' }], acknowledgeChatAttention: acknowledge })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<SessionItem sessionId="chat-1" />))
    expect(acknowledge).not.toHaveBeenCalled()
    act(() => host.querySelector('[data-session-id="chat-1"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(acknowledge).toHaveBeenLastCalledWith('chat-1', 'rendered-completion')
    act(() => host.querySelector('[data-session-id="chat-1"]')?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(acknowledge).toHaveBeenCalledTimes(2)
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ acknowledgeChatAttention: original })
  })

  it('does not reserve pin space and keeps row actions keyboard discoverable', () => {
    const { host, root } = renderSessionItem()

    const row = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')
    const actions = host.querySelector<HTMLElement>('[data-testid="session-actions-chat-1"]')
    const pin = actions?.querySelector<HTMLButtonElement>('button[aria-label="Pin chat"]')
    const more = actions?.querySelector<HTMLButtonElement>('button[aria-label="More actions"]')

    expect(row?.className).toContain('gap-1.5')
    expect(row?.className).toContain('px-2.5')
    expect(row?.className).toContain('py-1')
    expect(row?.className).toContain('min-h-[26px]')
    expect(Array.from(row?.querySelectorAll('span') ?? []).find((span) => span.textContent === 'Chat 1')?.className).toContain('text-[13px]')
    expect(actions?.className).toContain('absolute')
    expect(actions?.className).toContain('opacity-0')
    expect(actions?.className).toContain('group-hover:opacity-100')
    expect(actions?.className).toContain('group-focus-within:opacity-100')
    expect(pin).toBeTruthy()
    expect(more).toBeTruthy()
    expect(pin?.tabIndex).toBe(0)
    expect(more?.tabIndex).toBe(0)
    expect(host.querySelector('[role="img"][aria-label="Pinned"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('supports keyboard selection, actions, and confirmed family deletion', async () => {
    const setActiveSession = vi.fn()
    const deleteSessions = vi.fn(async () => true)
    useAppStore.setState({
      setActiveSession,
      deleteSessions,
      sessions: [
        makeSession(),
        { ...makeSession(), id: 'chat-branch', parentChatId: 'chat-1', branchRootChatId: 'chat-1' },
      ],
    })
    const { host, root } = renderSessionItem()
    const row = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')!

    expect(row.getAttribute('role')).toBe('button')
    expect(row.tabIndex).toBe(0)
    act(() => row.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setActiveSession).toHaveBeenCalledWith('chat-branch')

    row.focus()
    act(() => row.dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true })))
    let menu = document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Actions for Chat 1"]')!
    expect(menu).toBeTruthy()
    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.activeElement).toBe(row)

    act(() => row.dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true })))
    menu = document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Actions for Chat 1"]')!
    const deleteAction = Array.from(menu.querySelectorAll<HTMLButtonElement>('[role="menuitem"]'))
      .find((button) => button.textContent?.includes('Delete'))!
    act(() => deleteAction.click())
    expect(deleteSessions).not.toHaveBeenCalled()
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete Chat 1"]')).toBeTruthy()
    const confirm = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Delete chat')!
    await act(async () => confirm.click())
    expect(deleteSessions).toHaveBeenCalledWith(['chat-1', 'chat-branch'])

    act(() => root.unmount())
    host.remove()
  })

  it('selects sibling chats inside the active branch family', () => {
    const setActiveSession = vi.fn()
    const rootSession = { ...makeSession(), id: 'chat-root', branchRootChatId: 'chat-root', messageCount: 3 }
    const branchSession = { ...makeSession(), id: 'chat-branch', parentChatId: 'chat-root', branchRootChatId: 'chat-root', messageCount: 8 }
    useAppStore.setState({
      sessions: [rootSession, branchSession],
      activeSessionId: 'chat-root',
      setActiveSession,
    })
    const familySessionIds = ['chat-root', 'chat-branch']
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<>
      <SessionItem sessionId="chat-root" session={rootSession} familySessionIds={familySessionIds} />
      <SessionItem sessionId="chat-branch" session={branchSession} familySessionIds={familySessionIds} />
    </>))

    const rootRow = host.querySelector<HTMLElement>('[data-testid="session-row-chat-root"]')!
    const branchRow = host.querySelector<HTMLElement>('[data-testid="session-row-chat-branch"]')!
    expect(rootRow.getAttribute('aria-current')).toBe('page')
    expect(branchRow.hasAttribute('aria-current')).toBe(false)
    act(() => branchRow.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(setActiveSession).toHaveBeenCalledWith('chat-branch')

    act(() => useAppStore.setState({ activeSessionId: 'chat-branch' }))
    expect(rootRow.hasAttribute('aria-current')).toBe(false)
    expect(branchRow.getAttribute('aria-current')).toBe('page')
    act(() => rootRow.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setActiveSession).toHaveBeenLastCalledWith('chat-branch')

    act(() => root.unmount())
    host.remove()
  })

  it('restores the last selected family chat after reorder and falls back when it is deleted', () => {
    const rootSession = { ...makeSession(), id: 'chat-root', branchRootChatId: 'chat-root', messageCount: 3 }
    const branchA = { ...makeSession(), id: 'chat-branch-a', parentChatId: 'chat-root', branchRootChatId: 'chat-root', messageCount: 5 }
    const branchB = { ...makeSession(), id: 'chat-branch-b', parentChatId: 'chat-root', branchRootChatId: 'chat-root', messageCount: 20 }
    const familySessionIds = ['chat-root', 'chat-branch-a', 'chat-branch-b']
    useAppStore.setState({ sessions: [rootSession, branchA, branchB], activeSessionId: 'chat-root', setActiveSession: originalActions.setActiveSession })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<>
      <SessionItem sessionId="chat-root" session={rootSession} familySessionIds={familySessionIds} />
      <SessionItem sessionId="chat-branch-a" session={branchA} familySessionIds={familySessionIds} />
      <SessionItem sessionId="chat-branch-b" session={branchB} familySessionIds={familySessionIds} />
    </>))

    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-branch-a"]')!.click())
    expect(useAppStore.getState().activeSessionId).toBe('chat-branch-a')
    act(() => root.unmount())
    useAppStore.setState({ sessions: [rootSession, branchB, branchA] })
    const restored = createRoot(host)
    act(() => restored.render(<SessionItem sessionId="chat-root" session={rootSession} familySessionIds={familySessionIds} />))
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-root"]')!.click())
    expect(useAppStore.getState().activeSessionId).toBe('chat-branch-a')

    act(() => {
      useAppStore.setState({ sessions: [rootSession, branchB] })
      restored.render(<SessionItem sessionId="chat-root" session={rootSession} familySessionIds={['chat-root', 'chat-branch-b']} />)
    })
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-root"]')!.click())
    expect(useAppStore.getState().activeSessionId).toBe('chat-branch-b')

    act(() => restored.unmount())
    host.remove()
  })

  it('selects the longest provided branch without filtering or sorting all chats', () => {
    const setActiveSession = vi.fn()
    const rootSession = { ...makeSession(), id: 'chat-root', branchRootChatId: 'chat-root', messageCount: 3 }
    const branchA = {
      ...makeSession(), id: 'chat-branch-a', parentChatId: 'chat-root', branchRootChatId: 'chat-root',
      messageCount: 8, updatedAt: new Date('2026-09-20T10:00:00Z'),
    }
    const branchB = {
      ...makeSession(), id: 'chat-branch-b', parentChatId: 'chat-root', branchRootChatId: 'chat-root',
      messageCount: 8, updatedAt: new Date('2026-09-20T11:00:00Z'),
    }
    const branchC = {
      ...makeSession(), id: 'chat-branch-c', parentChatId: 'chat-root', branchRootChatId: 'chat-root',
      messageCount: 8, updatedAt: new Date('2026-09-20T11:00:00Z'),
    }
    const unrelated = { ...makeSession(), id: 'unrelated-chat', messageCount: 100 }
    const sessions = new Proxy([rootSession, branchA, branchC, branchB, unrelated], {
      get(target, property, receiver) {
        if (property === 'filter' || property === 'sort') throw new Error(`unexpected ${String(property)} on full chat list`)
        return Reflect.get(target, property, receiver)
      },
    })
    useAppStore.setState({ sessions, activeSessionId: 'unrelated-chat', setActiveSession })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <SessionItem
        sessionId="chat-root"
        session={rootSession}
        familySessionIds={['chat-root', 'chat-branch-a', 'chat-branch-b', 'chat-branch-c']}
      />,
    ))

    const rootRow = host.querySelector<HTMLElement>('[data-testid="session-row-chat-root"]')!
    act(() => rootRow.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(setActiveSession).toHaveBeenCalledWith('chat-branch-b')

    act(() => root.unmount())
    host.remove()
  })

  it('marks only the exact active chat in the companion branch list', () => {
    const previousPath = window.location.pathname
    window.history.pushState({}, '', '/companion')
    const rootSession = { ...makeSession(), id: 'chat-root', branchRootChatId: 'chat-root' }
    const branchSession = { ...makeSession(), id: 'chat-branch', parentChatId: 'chat-root', branchRootChatId: 'chat-root' }
    useAppStore.setState({ sessions: [rootSession, branchSession], activeSessionId: 'chat-root' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<>
      <SessionItem sessionId="chat-root" session={rootSession} familySessionIds={['chat-root', 'chat-branch']} />
      <SessionItem sessionId="chat-branch" session={branchSession} familySessionIds={['chat-root', 'chat-branch']} />
    </>))

    expect(host.querySelector('[data-testid="session-row-chat-root"] [aria-label="Selected chat"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="session-row-chat-branch"] [aria-label="Selected chat"]')).toBeNull()

    act(() => useAppStore.setState({ activeSessionId: 'chat-branch' }))
    expect(host.querySelector('[data-testid="session-row-chat-root"] [aria-label="Selected chat"]')).toBeNull()
    expect(host.querySelector('[data-testid="session-row-chat-branch"] [aria-label="Selected chat"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
    window.history.pushState({}, '', previousPath)
  })

  it('keeps a failed delete confirmation visible with its error', async () => {
    useAppStore.setState({ deleteSessions: vi.fn(async () => false) })
    const { host, root } = renderSessionItem()
    const row = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')!
    act(() => row.dispatchEvent(new KeyboardEvent('keydown', { key: 'F10', shiftKey: true, bubbles: true })))
    const menu = document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Actions for Chat 1"]')!
    act(() => Array.from(menu.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Delete'))!.click())
    const confirm = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Delete chat')!
    await act(async () => confirm.click())

    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete Chat 1"]')?.textContent).toContain('could not be deleted')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the persistent unpin button visible while retaining row actions', () => {
    act(() => useAppStore.setState({ sessions: [{ ...makeSession(), isPinned: true }] }))
    const { host, root } = renderSessionItem()

    expect(host.querySelector('button[aria-label="Unpin Chat 1"]')).toBeTruthy()
    const actions = host.querySelector<HTMLElement>('[data-testid="session-actions-chat-1"]')
    expect(actions?.querySelector('button[aria-label="Unpin chat"]')).toBeTruthy()
    expect(actions?.className).toContain('opacity-0')
    expect(actions?.className).toContain('group-hover:opacity-100')
    expect(actions?.className).toContain('group-focus-within:opacity-100')

    act(() => root.unmount())
    host.remove()
  })


  it.each([false, true])('pins and unpins a running chat from its context menu (activity=%s)', async (activityLayout) => {
    const setSessionPinned = vi.fn(async () => true)
    const setActiveSession = vi.fn()
    useAppStore.setState({ activeSessionId: 'chat-1', sessions: [makeSession()], setSessionPinned, setActiveSession,
      acpBindingBySessionId: { 'chat-1': makeAcpBinding({ processing: true }) } })
    const { host, root } = renderSessionItem(activityLayout)
    try {
      const row = host.querySelector<HTMLElement>('[data-session-id="chat-1"]')!
      act(() => row.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true })))
      const pin = Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.trim() === 'Pin chat')
      expect(pin).toBeTruthy()
      await act(async () => pin!.click())
      expect(setSessionPinned).toHaveBeenCalledWith('chat-1', true)
      expect(setActiveSession).not.toHaveBeenCalled()
      act(() => useAppStore.setState({ sessions: [{ ...makeSession(), isPinned: true }] }))
      act(() => row.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true })))
      const unpin = Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.trim() === 'Unpin chat')
      expect(unpin).toBeTruthy()
      await act(async () => unpin!.click())
      expect(setSessionPinned).toHaveBeenLastCalledWith('chat-1', false)
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('unpins from the persistent button without selecting or dragging its chat', () => {
    const setSessionPinned = vi.fn(async () => true)
    const setActiveSession = vi.fn()
    useAppStore.setState({ sessions: [{ ...makeSession(), isPinned: true }], setSessionPinned, setActiveSession })
    const { host, root } = renderSessionItem()
    const button = host.querySelector<HTMLButtonElement>('button[aria-label="Unpin Chat 1"]')!
    expect(button.type).toBe('button')
    expect(button.draggable).toBe(false)
    const drag = new Event('dragstart', { bubbles: true, cancelable: true })
    act(() => button.dispatchEvent(drag))
    expect(drag.defaultPrevented).toBe(true)
    act(() => button.click())
    expect(setSessionPinned).toHaveBeenCalledWith('chat-1', false)
    expect(setActiveSession).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })

  it.each(['Enter', ' '])('keeps keyboard %s activation on the persistent unpin button', (key) => {
    const setSessionPinned = vi.fn(async () => true)
    const setActiveSession = vi.fn()
    useAppStore.setState({ sessions: [{ ...makeSession(), isPinned: true }], setSessionPinned, setActiveSession })
    const { host, root } = renderSessionItem()
    const button = host.querySelector<HTMLButtonElement>('button[aria-label="Unpin Chat 1"]')!
    button.focus()
    const event = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true })
    act(() => button.dispatchEvent(event))
    expect(event.defaultPrevented).toBe(false)
    // jsdom does not synthesize the click that native buttons receive from Enter/Space.
    act(() => button.click())
    expect(setSessionPinned).toHaveBeenCalledWith('chat-1', false)
    expect(setActiveSession).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })

  it.each([
    ['question' as AcpAttentionKind, 'Needs answer'],
    ['plan' as AcpAttentionKind, 'Plan needs review'],
    ['memory' as AcpAttentionKind, 'Memory input needed'],
    ['permission' as AcpAttentionKind, 'Permission needed'],
    ['command' as AcpAttentionKind, 'Command approval needed'],
    ['file' as AcpAttentionKind, 'File approval needed'],
  ])('renders %s attention before generic processing', (attentionKind, label) => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({
          lifecycleState: 'waitingUserInput',
          processing: true,
          attentionKind,
        }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector(`[aria-label="${label}"]`)).toBeTruthy()
    expect(host.querySelector('[aria-label="Agent running"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not turn an ACP error into an active-chat status icon', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ lifecycleState: 'error', running: false, attentionKind: 'error' }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('.session-status')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the spinner for processing without a user attention kind', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ lifecycleState: 'processing', processing: true }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('[aria-label="Agent running"]')).toBeTruthy()
    expect(host.querySelector('[aria-label="Done"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows terminal-fallback progress when a pending call becomes active', () => {
    const { host, root } = renderSessionItem()
    expect(host.querySelector('[aria-label="Agent running"]')).toBeNull()

    act(() => useAppStore.setState({
      cliBindingBySessionId: {
        'chat-1': {
          terminalId: 'terminal-1',
          boundChatId: 'chat-1',
          running: true,
          lifecycleState: 'idle',
          turnState: 'idle',
          processing: true,
          readySinceLastSelect: false,
          active: true,
          lastError: '',
        },
      },
    }))

    expect(host.querySelector('[aria-label="Agent running"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the dot for done chats without active attention', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ readySinceLastSelect: true }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('[aria-label="Done"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows no status for an idle bound process or a hidden runtime error', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ running: true, lifecycleState: 'error', lastError: 'hidden failure' }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('.session-status')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('shows its pane colour without legacy context-menu pane buttons', () => {
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(setChatInLeaf(layout, 'chat-1', leaves[0].id), 'chat-2', leaves[1].id)
    writeChatGridLayout(layout)
    useAppStore.setState({ activeSessionId: 'chat-1' })
    const { host, root } = renderSessionItem()

    expect(host.querySelector('[role="img"][aria-label="Shown in pane 1"]')).toBeTruthy()
    const sessionRow = host.querySelector('.cursor-pointer') as HTMLElement
    expect((sessionRow.querySelector('[data-testid="pane-indicator"]') as HTMLElement).style.background).toContain('rgb(249, 115, 22)')
    act(() => sessionRow.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const menu = document.body.querySelector('[data-viewport-menu]') as HTMLElement
    expect(menu.textContent).not.toContain('Show in pane')
    expect(menu.querySelector('button[aria-label^="Show Chat 1 in pane"]')).toBeNull()
    expect(menu.textContent).toContain('Rename')

    act(() => root.unmount())
    host.remove()
  })

  it('activates an existing pane when selecting a chat from another pane', () => {
    const setActiveSession = vi.fn()
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(setChatInLeaf(layout, 'chat-1', leaves[0].id), 'chat-2', leaves[1].id)
    writeChatGridLayout({ ...layout, activeLeafId: leaves[1].id })
    useAppStore.setState({ activeSessionId: 'chat-2', setActiveSession })

    const { host, root } = renderSessionItem()
    const sessionRow = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')!
    act(() => sessionRow.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    expect(setActiveSession).toHaveBeenCalledWith('chat-1')

    act(() => root.unmount())
    host.remove()
  })

  it('does not activate a sibling pane when the resolved branch is already active', () => {
    const setActiveSession = vi.fn()
    const rootSession = { ...makeSession(), id: 'chat-root', branchRootChatId: 'chat-root', messageCount: 3 }
    const branchSession = { ...makeSession(), id: 'chat-branch', parentChatId: 'chat-root', branchRootChatId: 'chat-root', messageCount: 8 }
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(setChatInLeaf(layout, 'chat-root', leaves[0].id), 'chat-other', leaves[1].id)
    writeChatGridLayout({ ...layout, activeLeafId: leaves[1].id })
    useAppStore.setState({ sessions: [rootSession, branchSession], activeSessionId: 'chat-branch', setActiveSession })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<SessionItem sessionId="chat-root" session={rootSession} familySessionIds={['chat-root', 'chat-branch']} />))
    const sessionRow = host.querySelector<HTMLElement>('[data-testid="session-row-chat-root"]')!
    act(() => sessionRow.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    expect(readChatGridLayout().activeLeafId).toBe(leaves[1].id)
    expect(setActiveSession).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

  it.each(['fallback', 'active parent', 'preferred sibling', 'missing parent', 'cycle', 'parent cycle', 'foreign parent', 'preferred over active'] as const)('selects the deepest descendant with safe %s handling', (scenario) => {
    const setActiveSession = vi.fn()
    const rootSession = { ...makeSession(), id: 'depth-root', branchRootChatId: 'depth-root', messageCount: 100 }
    const parent = { ...makeSession(), id: 'depth-parent', parentChatId: rootSession.id, branchRootChatId: rootSession.id, messageCount: 80 }
    const sibling = { ...parent, id: 'depth-sibling', messageCount: 90 }
    const deepest = { ...parent, id: 'depth-child', parentChatId: parent.id, messageCount: 2 }
    const malformed = { ...deepest, id: 'depth-malformed', messageCount: 1000, parentChatId: scenario === 'cycle' ? 'depth-malformed' : scenario === 'parent cycle' ? 'cycle-parent' : scenario === 'foreign parent' ? 'foreign-parent' : 'absent-parent' }
    const foreign = { ...parent, id: 'foreign-parent', parentChatId: deepest.id, branchRootChatId: 'another-root' }
    const cycleParent = { ...malformed, id: 'cycle-parent', parentChatId: malformed.id }
    if (scenario === 'preferred sibling' || scenario === 'preferred over active') setPreferredFamilyChat(rootSession.id, sibling.id)
    useAppStore.setState({ sessions: [rootSession, parent, sibling, deepest, malformed, foreign, cycleParent], activeSessionId: scenario === 'active parent' || scenario === 'preferred over active' ? parent.id : 'unrelated', setActiveSession })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<SessionItem sessionId={rootSession.id} session={rootSession} familySessionIds={[rootSession.id, parent.id, sibling.id, deepest.id, malformed.id, cycleParent.id]} />))
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-depth-root"]')!.click())
    if (scenario === 'active parent') expect(setActiveSession).not.toHaveBeenCalled()
    else expect(setActiveSession).toHaveBeenCalledWith(scenario === 'preferred sibling' || scenario === 'preferred over active' ? sibling.id : deepest.id)
    act(() => root.unmount())
    host.remove()
  })

  it('hides pane assignment noise in single-chat view', () => {
    writeChatGridLayout(defaultChatGridLayout)
    const { host, root } = renderSessionItem()

    act(() => (host.querySelector('.cursor-pointer') as HTMLElement).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const menu = document.body.querySelector('[data-viewport-menu]') as HTMLElement
    expect(menu.textContent).not.toContain('Show in pane')
    expect(menu.querySelector('button[aria-label^="Show Chat 1 in pane"]')).toBeNull()
    expect(host.querySelector('[aria-label^="Shown in pane"]')).toBeNull()
    expect(menu.textContent).toContain('Rename')

    act(() => root.unmount())
    host.remove()
  })

  it('shows the chat custom icon in place of the provider glyph and before the activity title', () => {
    const icon = { type: 'text' as const, value: '🧪' }
    useAppStore.setState({ sessions: [{ ...makeSession(), providerId: 'gemini-cli', customIcon: icon }], showProviderIconsInSidebar: true })
    const compact = renderSessionItem()
    try {
      expect(compact.host.querySelector('[aria-label="Chat icon"]')?.textContent).toContain('🧪')
      expect(compact.host.querySelector('[aria-label^="Provider:"]')).toBeNull()
    } finally { act(() => compact.root.unmount()); compact.host.remove() }

    const activity = renderSessionItem(true)
    try {
      const text = activity.host.textContent ?? ''
      expect(text.indexOf('🧪')).toBeGreaterThanOrEqual(0)
      expect(text.indexOf('🧪')).toBeLessThan(text.indexOf('Chat 1'))
    } finally { act(() => activity.root.unmount()); activity.host.remove() }
  })

  it('groups the chat menu into sections with dividers and submenus for copy, snooze and collections', () => {
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }] })
    const { host, root } = renderSessionItem()
    try {
      act(() => host.querySelector('.cursor-pointer')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 30 })))
      const menu = document.body.querySelector('[data-viewport-menu]')!
      const items = Array.from(menu.children).map((child) => child.getAttribute('role') === 'separator' ? '---' : child.textContent?.trim())
      expect(items).toEqual([
        'New chat in this worktree',
        '---',
        'RenameF2',
        'Regenerate title',
        'Pin chat',
        'Icon…',
        '---',
        'Copy',
        'Snooze',
        'Move to collection',
        '---',
        'Delete…Del',
      ])
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('moves a chat to a collection from its right-click menu', async () => {
    const moveResourceToCollection = vi.fn().mockResolvedValue(true)
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
      moveResourceToCollection,
      removeResourceReference: vi.fn().mockResolvedValue(true),
    })
    const { host, root } = renderSessionItem()
    act(() => (host.querySelector('.cursor-pointer') as HTMLElement).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.includes('Move to collection'))
    act(() => move?.click())
    const work = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.trim() === 'Work')
    await act(async () => { work?.click(); await Promise.resolve() })
    expect(moveResourceToCollection).toHaveBeenCalledWith('work', 'chat', 'chat-1', 'Chat 1')
    act(() => root.unmount())
    host.remove()
  })
  it('opens new chat from the right-clicked worktree even while another chat is active or processing', () => {
    const source = { ...makeSession(), workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/tmp/selected-worktree', executionHostId: 'local' }
    useAppStore.setState({ sessions: [source], activeSessionId: 'other-chat', acpBindingBySessionId: { 'chat-1': makeAcpBinding({ processing: true }) }, newChatWorktreeContext: null })
    const { host, root } = renderSessionItem()
    try {
      act(() => host.querySelector('[data-testid="session-row-chat-1"]')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true })))
      const menu = document.body.querySelector<HTMLElement>('[role="menu"][aria-label="Actions for Chat 1"]')!
      const create = Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('New chat in this worktree'))!
      expect(create.disabled).toBe(false)
      act(() => create.click())
      expect(useAppStore.getState()).toMatchObject({ isNewChatModalOpen: true, newChatFolderId: 'project', newChatWorktreeContext: { sourceChatId: 'chat-1', expectedWorkspaceDirectory: '/tmp/selected-worktree', expectedExecutionHostId: 'local' } })
      act(() => useAppStore.getState().setNewChatModalOpen(false))
      expect(useAppStore.getState().newChatWorktreeContext).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

})

describe('SessionItem snooze actions', () => {
  const originalSnooze = useAppStore.getState().setChatSnoozedUntil
  afterEach(() => { useAppStore.setState({ setChatSnoozedUntil: originalSnooze }) })

  it('opens the snooze options from the Active row hover action beside Done', () => {
    useAppStore.setState({ acpBindingBySessionId: { 'chat-1': makeAcpBinding({ readySinceLastSelect: true }) } })
    const { host, root } = renderSessionItem(true)
    try {
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Snooze Chat 1"]')!.click())
      const menu = document.body.querySelector('[role="menu"][aria-label="Snooze Chat 1"]')
      expect(menu?.querySelector('[role="menuitem"]')?.textContent?.startsWith('5 minutes')).toBe(true)
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('offers Done and Snooze on a compact active row and only Done clears it', () => {
    const setChatSettled = vi.fn(async () => true)
    useAppStore.setState({ setChatSettled, acpBindingBySessionId: { 'chat-1': makeAcpBinding({ readySinceLastSelect: true }) } } as never)
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<SessionItem sessionId="chat-1" active />))
    try {
      expect(host.querySelector('button[aria-label="Snooze Chat 1"]')).toBeTruthy()
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Mark Chat 1 done"]')!.click())
      expect(setChatSettled).toHaveBeenCalledWith('chat-1', true)
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('shows the wake time on a snoozed row and wakes it from the Unsnooze action', () => {
    const setChatSnoozedUntil = vi.fn(async () => ({ ok: true }))
    useAppStore.setState({ setChatSnoozedUntil } as never)
    const session = { ...makeSession(), snoozedUntilMs: Date.now() + 3_600_000 }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<SessionItem sessionId="chat-1" session={session} />))
    try {
      expect(host.textContent).toContain('until ')
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Unsnooze Chat 1"]')!.click())
      expect(setChatSnoozedUntil).toHaveBeenCalledWith('chat-1', 0)
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
