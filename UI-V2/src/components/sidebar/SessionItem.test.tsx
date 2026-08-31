import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type AcpAttentionKind, type AcpBinding } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { formatSidebarWorktreePath, SessionItem } from './SessionItem'
import { chatGridLeaves, defaultChatGridLayout, readChatGridLayout, setChatInLeaf, splitChatLeaf, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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

function renderSessionItem() {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<SessionItem sessionId="chat-1" />)
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

  it('shows provider and shortened worktree context when enabled', () => {
    useAppStore.setState({
      sessions: [{
        ...makeSession(),
        providerId: 'codex-cli',
        workspaceWorktreeDirectory: '/Users/david/project/.uam-worktrees/chat-abc123',
      }],
    })
    const { host, root } = renderSessionItem()

    expect(formatSidebarWorktreePath('C:\\Users\\david\\project\\chat-abc123')).toBe('.../project/chat-abc123')
    expect(host.querySelector('img.uam-provider-logo--codex')?.parentElement?.style.width).toBe('16px')
    expect(host.querySelector('[role="img"][aria-label="Provider: Codex"]')).toBeTruthy()
    expect(host.textContent).toContain('.../.uam-worktrees/chat-abc123')
    expect(host.querySelector('[title="/Users/david/project/.uam-worktrees/chat-abc123"]')).toBeTruthy()

    act(() => useAppStore.setState({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: false }))
    expect(host.querySelector('img.uam-provider-logo--codex')).toBeNull()
    expect(host.textContent).not.toContain('.uam-worktrees')

    act(() => root.unmount())
    host.remove()
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
    expect(setActiveSession).toHaveBeenCalledWith('chat-1')

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

  it('keeps pinned state visible while its unpin action stays with row actions', () => {
    act(() => useAppStore.setState({ sessions: [{ ...makeSession(), isPinned: true }] }))
    const { host, root } = renderSessionItem()

    expect(host.querySelector('[role="img"][aria-label="Pinned"]')).toBeTruthy()
    const actions = host.querySelector<HTMLElement>('[data-testid="session-actions-chat-1"]')
    expect(actions?.querySelector('button[aria-label="Unpin chat"]')).toBeTruthy()
    expect(actions?.className).toContain('opacity-0')
    expect(actions?.className).toContain('group-hover:opacity-100')
    expect(actions?.className).toContain('group-focus-within:opacity-100')

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

  it('moves a chat to a collection from its right-click menu', async () => {
    const addResourceReference = vi.fn().mockResolvedValue(true)
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
      addResourceReference,
      removeResourceReference: vi.fn().mockResolvedValue(true),
    })
    const { host, root } = renderSessionItem()
    act(() => (host.querySelector('.cursor-pointer') as HTMLElement).dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.includes('Move to collection'))
    act(() => move?.click())
    const work = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.trim() === 'Work')
    await act(async () => { work?.click(); await Promise.resolve() })
    expect(addResourceReference).toHaveBeenCalledWith('work', 'chat', 'chat-1', 'Chat 1')
    act(() => root.unmount())
    host.remove()
  })
})
