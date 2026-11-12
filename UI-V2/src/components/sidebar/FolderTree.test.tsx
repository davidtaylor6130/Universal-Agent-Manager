import { addOutgoingAttempt, automationPromptRowKey, outgoingScope, resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { APP_FAILURE_EVENT } from '../../utils/appFailure'
import type { Folder, Session } from '../../types/session'
import { FolderTree } from './FolderTree'

const stubRect = (element: HTMLElement, top: number, height: number) =>
  vi.spyOn(element, 'getBoundingClientRect').mockReturnValue({ top, bottom: top + height, height, left: 0, right: 100, width: 100, x: 0, y: top, toJSON: () => ({}) })
const pointer = (target: EventTarget, type: string, clientX: number, clientY: number) =>
  target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, clientX, clientY, button: 0, buttons: type === 'pointerup' ? 0 : 1 }))
/** A drag swallows the click that follows its release until the next task; let that pass before later clicks. */
const releaseClickGuard = () => act(async () => { await new Promise((resolve) => setTimeout(resolve, 0)) })
import { chatGridLeaves, defaultChatGridLayout, readChatGridLayout, setChatInLeaf, splitChatLeaf, subscribeChatGridLayout, writeChatGridLayout, type ChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function gridLayout(...sessionIds: string[]) {
  let layout = defaultChatGridLayout
  while (chatGridLeaves(layout.root).length < sessionIds.length) layout = splitChatLeaf(layout, layout.activeLeafId, 'horizontal')
  chatGridLeaves(layout.root).forEach((leaf, index) => { layout = setChatInLeaf(layout, sessionIds[index] ?? '', leaf.id) })
  return layout
}

const now = new Date('2026-01-01T12:00:00.000Z')
const originalAddFolder = useAppStore.getState().addFolder
const originalListRemoteDirectories = useAppStore.getState().listRemoteDirectories
const originalSetActiveSession = useAppStore.getState().setActiveSession
const originalDeleteFolder = useAppStore.getState().deleteFolder
const originalDeleteFolders = useAppStore.getState().deleteFolders

function makeFolder(): Folder {
  return {
    id: 'project',
    name: 'Project',
    parentId: null,
    directory: '/tmp/project',
    isExpanded: true,
    createdAt: now,
  }
}

function makeSession(index: number): Session {
  const openedAt = new Date(now)
  openedAt.setMinutes(now.getMinutes() - index)

  return {
    id: `chat-${index}`,
    name: `Chat ${index}`,
    viewMode: 'chat',
    folderId: 'project',
    createdAt: openedAt,
    updatedAt: openedAt,
    lastOpenedAt: openedAt,
  }
}

describe('FolderTree', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      folders: [makeFolder()],
      sessions: Array.from({ length: 7 }, (_, index) => makeSession(index + 1)),
      activeSessionId: 'chat-1',
      messages: {},
      cliBindingBySessionId: {},
      acpBindingBySessionId: {},
      cliTranscriptBySessionId: {},
      resourceCollections: [],
      sidebarPinnedChatOrder: [],
      sidebarActiveChatOrder: [],
	  executionHosts: [
		{ id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
	  ],
      isNewChatModalOpen: false,
      newChatFolderId: null,
      openFolderMemoryLibrary: vi.fn(() => Promise.resolve(true)),
      rescanFolderChats: vi.fn(() => Promise.resolve(true)),
      previewUnsortedWorkspaceFolders: vi.fn(() => Promise.resolve(null)),
      rebuildUnsortedWorkspaceFolders: vi.fn(() => Promise.resolve(true)),
      workspaceFolderRecoveryError: '',
      workspaceDeletionError: '',
      addFolder: originalAddFolder,
      listRemoteDirectories: originalListRemoteDirectories,
    })
  })

  it.each([
    { caseName: 'remote Unsorted', folderId: null, sessionHost: 'ssh-a', expected: 'Remote A' },
    { caseName: 'remote chat with a conflicting folder host', folderId: 'project', sessionHost: 'ssh-a', expected: 'Remote A' },
    { caseName: 'explicit local chat in a remote folder', folderId: 'project', sessionHost: 'local', expected: 'This computer' },
    { caseName: 'legacy chat inheriting its assigned folder host', folderId: 'project', sessionHost: undefined, expected: 'Remote B' },
  ])('uses chat ownership for the Active host label: $caseName', ({ folderId, sessionHost, expected }) => {
    useAppStore.setState((state) => ({
      executionHosts: [...state.executionHosts, ...['a', 'b'].map((suffix) => ({ ...state.executionHosts[0], id: `ssh-${suffix}`, label: `Remote ${suffix.toUpperCase()}`, transport: 'ssh' as const }))],
      folders: [{ ...makeFolder(), executionHostId: 'ssh-b' }],
      sessions: [{ ...makeSession(1), folderId, executionHostId: sessionHost }],
      acpBindingBySessionId: { 'chat-1': { processing: true } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      const active = host.querySelector('[data-testid="active-chats"]')!
      expect(active.querySelector('[role="img"][aria-label^="Runs on"]')?.getAttribute('aria-label')).toBe(`Runs on ${expected}`)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('glides workspace chats on expand and keeps collapsed chats inert', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<FolderTree searchQuery="" />))
    const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!
    const list = host.querySelector<HTMLElement>('[data-testid="folder-sessions-project"]')!
    expect(header.getAttribute('aria-expanded')).toBe('true')
    expect(list.querySelector('.uam-folder-expand-glide')).toBeTruthy()

    act(() => header.click())
    expect(header.getAttribute('aria-expanded')).toBe('false')
    expect(list.getAttribute('aria-hidden')).toBe('true')
    expect(list.hasAttribute('inert')).toBe(true)
    expect(list.querySelector('.uam-folder-expand-glide')).toBeNull()

    act(() => header.click())
    expect(header.getAttribute('aria-expanded')).toBe('true')
    expect(list.hasAttribute('inert')).toBe(false)
    expect(list.querySelector('.uam-folder-expand-glide')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('renders a workspace custom icon in the sidebar header', () => {
    useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, customIcon: { type: 'text', value: '📌' } })) }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.querySelector('[data-testid="folder-header-project"]')?.textContent).toContain('📌')
    expect(host.querySelector('[data-testid="folder-icon-project"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('shows five recent chats in a folder until see more is clicked', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    expect(host.textContent).toContain('Chat 1')
    expect(host.textContent).toContain('Chat 5')
    expect(host.textContent).not.toContain('Chat 6')
    expect(host.textContent).not.toContain('⌃')
    expect(host.textContent).toContain('Show 2 more')
    expect(host.textContent).not.toContain('See more')
    expect(Array.from(host.querySelectorAll('button')).filter((button) => button.textContent?.trim() === 'New chat')).toHaveLength(0)
    expect((host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement).style.color).toBe('var(--text-3)')
    const activeTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 1')
    expect(activeTitle?.getAttribute('style')).toContain('var(--text)')
    expect(activeTitle?.getAttribute('style')).not.toContain('#ffffff')

    const seeMoreButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Show 2 more')
    )
    expect(seeMoreButton).toBeTruthy()
    expect(seeMoreButton?.style.borderStyle).toBe('none')

    act(() => {
      seeMoreButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Chat 6')
    expect(host.textContent).toContain('Chat 7')
    expect(host.textContent).toContain('Show less')
    expect(host.querySelector('[data-testid="folder-row-project"]')?.className).toContain('mb-0')
    expect(Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 1')?.closest('.cursor-pointer')?.className).toContain('py-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows all chats in an expanded companion workspace', () => {
    window.history.replaceState(null, '', '/companion')
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    try {
      act(() => {
        root.render(<FolderTree searchQuery="" />)
      })

      expect(host.textContent).toContain('Chat 6')
      expect(host.textContent).toContain('Chat 7')
      expect(host.textContent).not.toContain('Show 2 more')
    } finally {
      act(() => root.unmount())
      host.remove()
      window.history.replaceState(null, '', '/')
    }
  })

  it('keeps workspace headers keyboard reachable while searching, without nesting the actions button', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="Chat" />))
      const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!
      expect((host.querySelector('[data-testid="folder-row-project"]') as HTMLElement).draggable).toBe(false)
      expect(header.tabIndex).toBe(0)
      expect(header.getAttribute('role')).toBe('button')
      expect(header.getAttribute('aria-expanded')).toBe('true')
      expect(header.hasAttribute('aria-pressed')).toBe(false)
      expect(header.querySelector('button, [tabindex="0"]')).toBeNull()
      const actions = host.querySelector<HTMLButtonElement>('button[aria-label="Folder actions"]')!
      expect(header.contains(actions)).toBe(false)
      expect(actions.closest('.group')?.contains(header)).toBe(true)
      expect(actions.parentElement?.closest('[class*="group-has-[:focus-visible]:opacity-100"]')).toBeTruthy()
      expect(header.getAttribute('aria-keyshortcuts')).not.toContain('ArrowUp')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('does not enable desktop folder dragging in the companion chat list', () => {
    window.history.replaceState(null, '', '/companion')
    const setActiveSession = vi.fn()
    useAppStore.setState({ setActiveSession, activeSessionId: 'chat-2' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      expect((host.querySelector('[data-testid="folder-row-project"]') as HTMLElement | null)?.draggable).toBe(false)
      act(() => host.querySelector('[data-testid="session-row-chat-1"]')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
      expect(setActiveSession).toHaveBeenCalledWith('chat-1')
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ setActiveSession: originalSetActiveSession })
      window.history.replaceState(null, '', '/')
    }
  })

  it('hides unsupported folder actions and bulk selection in companion context', () => {
    useAppStore.setState({
      folders: ['a', 'b', 'c'].map((id) => ({ ...makeFolder(), id, name: id })),
      sessions: ['a', 'b', 'c'].map((id, index) => ({ ...makeSession(index), folderId: id })),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-a"]')!.click())
      act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-c"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
      expect(host.querySelector('[aria-label="Delete 3 selected workspaces"]')).toBeTruthy()
      act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
      expect(document.body.querySelector('[role="menu"][aria-label="a actions"]')?.textContent).toContain('Rename folder')

      window.history.replaceState(null, '', '/companion')
      act(() => useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder })) })))
      expect(host.querySelector('[aria-label="Delete 3 selected workspaces"]')).toBeNull()
      expect(Array.from(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]'))).toHaveLength(0)
      const menu = document.body.querySelector('[role="menu"][aria-label="a actions"]')
      expect(menu?.textContent).toContain('New chat')
      expect(menu?.textContent).not.toContain('Rename folder')
      expect(menu?.textContent).not.toContain('Delete folder')
      expect(menu?.textContent).not.toContain('Review chats')

      act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-b"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
      expect(host.querySelector('[aria-label="Delete 2 selected workspaces"]')).toBeNull()
      expect(host.querySelector('[data-testid="folder-header-b"]')?.getAttribute('aria-label')).toBe('b')
    } finally {
      act(() => root.unmount())
      host.remove()
      window.history.replaceState(null, '', '/')
    }
  })

  it('does not rescan the sidebar runtime model when only turn event text changes', () => {
    const sessions = Array.from({ length: 120 }, (_, index) => makeSession(index + 1))
    let historicalStatusReads = 0
    const acpBindingBySessionId = Object.fromEntries(sessions.map((session, index) => {
      const binding = { turnEvents: [] as Array<{ type: 'assistant_text'; text: string }> }
      const countRead = <T,>(value: T) => ({
        configurable: true,
        enumerable: true,
        get: () => {
          if (index >= 49) historicalStatusReads += 1
          return value
        },
      })
      Object.defineProperties(binding, {
        processing: countRead(false),
        lifecycleState: countRead('ready'),
        readySinceLastSelect: countRead(false),
        attentionKind: countRead(null),
      })
      return [session.id, binding]
    }))
    useAppStore.setState({
      sessions,
      acpBindingBySessionId: acpBindingBySessionId as never,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelectorAll('[data-session-id]')).toHaveLength(5)
    expect(historicalStatusReads).toBe(355)
    historicalStatusReads = 0
    const currentBinding = acpBindingBySessionId['chat-1']

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...currentBinding,
          turnEvents: [{ type: 'assistant_text', text: 'A new streaming fragment.' }],
        } as never,
      },
    })))

    expect(historicalStatusReads).toBe(0)
    expect(host.textContent).toContain('Show 115 more')

    act(() => root.unmount())
    host.remove()
  })

  it('shows the actual execution computer on workspace rows only', () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'gaming-ai', label: 'Windows workstation', transport: 'ssh', sshAlias: 'gaming-ai', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'windows', architecture: 'x86_64', lastSeenAt: '' },
      ],
      folders: [{ ...useAppStore.getState().folders[0], executionHostId: 'gaming-ai' }],
      sessions: Array.from({ length: 2 }, (_, index) => ({ ...makeSession(index + 1), executionHostId: 'gaming-ai' })),
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.querySelector('[data-testid="workspace-machine-project"]')?.getAttribute('aria-label')).toBe('Runs on Windows workstation')
    expect(host.querySelectorAll('[data-testid^="workspace-machine-"]')).toHaveLength(1)

    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session, index) => index === 0 ? { ...session, executionHostId: 'local' } : session) })))
    expect(host.querySelector('[data-testid="workspace-machine-project"]')?.getAttribute('aria-label')).toBe('Runs on Windows workstation')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps same-directory workspaces on different computers separate', () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'homelab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
        { id: 'nas', label: 'NAS', transport: 'ssh', sshAlias: 'nas', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
      ],
      folders: [
        { ...useAppStore.getState().folders[0], id: 'homelab-workspace', name: 'Homelab project', directory: '/srv/project', executionHostId: 'homelab' },
        { ...useAppStore.getState().folders[0], id: 'nas-workspace', name: 'NAS project', directory: '/srv/project', executionHostId: 'nas' },
      ],
      sessions: [
        { ...makeSession(1), folderId: 'homelab-workspace', workspaceDirectory: '/srv/project', executionHostId: 'homelab' },
        { ...makeSession(2), folderId: 'nas-workspace', workspaceDirectory: '/srv/project', executionHostId: 'nas' },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.querySelector('[data-testid="workspace-machine-homelab-workspace"]')?.getAttribute('aria-label')).toBe('Runs on Homelab')
    expect(host.querySelector('[data-testid="workspace-machine-nas-workspace"]')?.getAttribute('aria-label')).toBe('Runs on NAS')
    expect(host.querySelectorAll('[data-testid^="workspace-machine-"]')).toHaveLength(2)

    act(() => root.unmount())
    host.remove()
  })

  it('does not reclassify a workspace chat drag as a folder drag', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const data = new Map<string, string>()
    const transfer = {
      effectAllowed: '',
      get types() { return [...data.keys()] },
      getData: (type: string) => data.get(type) ?? '',
      setData: (type: string, value: string) => { data.set(type, value) },
    }
    const dragStart = new Event('dragstart', { bubbles: true, cancelable: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: transfer })

    act(() => host.querySelector('[data-testid="session-row-chat-1"]')?.dispatchEvent(dragStart))

    expect(transfer.effectAllowed).toBe('copy')
    expect(data.get('text/x-uam-chat-id')).toBe('chat-1')
    expect(data.has('text/x-uam-folder-id')).toBe(false)
    expect(data.has('text/x-uam-folder-resource-id')).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('uses collections only to group, collapse, and accept workspace folders', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    useAppStore.setState({
      folders: [makeFolder(), second],
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector('[data-testid="folder-collection-work"]')
    expect(collection?.querySelector('[data-testid="folder-row-project"]')).toBeTruthy()
    const collectionHeader = collection?.firstElementChild as HTMLElement
    const collectionChildren = collection?.querySelector('[data-testid="collection-children-work"]') as HTMLElement
    expect(collectionChildren.className).toContain('ml-3')
    expect(collectionChildren.className).toContain('pl-1')
    expect(collectionChildren.className).toContain('py-0')
    expect(collectionChildren.className).not.toContain('border-l-2')
    expect(collectionHeader.style.border).toBe('1px solid transparent')
    expect(collectionChildren.style.background).toBe('transparent')
    expect(collectionChildren.style.boxShadow).toBe('none')
    expect(collection?.querySelector('[data-testid="folder-row-second"]')).toBeNull()
    expect(host.querySelectorAll('[data-testid="folder-row-project"]')).toHaveLength(1)
    expect(host.textContent).not.toContain('Collections')

    act(() => {
      collection?.querySelector('button[aria-label="Collapse Work"]')
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.querySelector('[data-testid="collection-children-work"]')?.parentElement?.parentElement?.getAttribute('aria-hidden')).toBe('true')
    expect(host.querySelector('[data-testid="folder-row-second"]')).toBeTruthy()

    // Dragging a workspace over a collection highlights the collection; releasing moves it in.
    const collectionHeaderTarget = host.querySelector<HTMLElement>('[data-testid="collection-header-work"]')!
    const elementFromPoint = vi.fn(() => collectionHeaderTarget)
    Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: elementFromPoint })
    try {
      const secondHeader = host.querySelector<HTMLElement>('[data-testid="folder-header-second"]')!
      act(() => pointer(secondHeader, 'pointerdown', 10, 50))
      act(() => pointer(document, 'pointermove', 10, 20))
      expect(collection?.classList.contains('uam-sb-drop-target')).toBe(true)
      await act(async () => { pointer(document, 'pointerup', 10, 20); await Promise.resolve() })
      expect(collection?.classList.contains('uam-sb-drop-target')).toBe(false)
    } finally {
      delete (document as { elementFromPoint?: unknown }).elementFromPoint
    }

    expect(useAppStore.getState().resourceCollections[0].references.map((reference) => reference.target)).toEqual(['project', 'second'])
    expect(collection?.querySelector('[data-testid="folder-row-second"]')).toBeTruthy()

    await releaseClickGuard()
    act(() => root.unmount())
    host.remove()
  })

  it('compacts collection, workspace, and chat rows without hiding their actions', () => {
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector<HTMLElement>('[data-testid="folder-collection-work"]')
    const collectionHeader = host.querySelector<HTMLElement>('[data-testid="collection-header-work"]')
    const folder = host.querySelector<HTMLElement>('[data-testid="folder-row-project"]')
    const folderHeader = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')
    const chat = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')

    expect(collection?.className).toContain('mb-0')
    expect(collectionHeader?.className).toContain('gap-1.5')
    expect(collectionHeader?.className).toContain('px-2.5')
    expect(collectionHeader?.className).toContain('py-0.5')
    expect(folder?.className).toContain('mb-0')
    expect(folder?.style.boxShadow).toBe('')
    expect(folderHeader?.className).toContain('gap-1.5')
    expect(folderHeader?.className).toContain('px-2.5')
    expect(folderHeader?.className).toContain('py-0.5')
    expect(chat?.className).toContain('gap-1.5')
    expect(chat?.className).toContain('px-2.5')
    expect(chat?.className).toContain('py-1')
    expect(chat?.parentElement?.parentElement?.className).toContain('pl-3.5')
    expect(chat?.querySelector('button[aria-label="More actions"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('opens collection actions in a dismissible viewport menu without expanding the collection layout', () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector('[data-testid="folder-collection-work"]') as HTMLElement
    const trigger = collection.querySelector('button[aria-label="Actions for Work"]') as HTMLButtonElement
    const childCount = collection.children.length
    act(() => trigger.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Work actions"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(collection.contains(menu)).toBe(false)
    expect(collection.children).toHaveLength(childCount)
    expect(trigger.getAttribute('aria-expanded')).toBe('true')
    expect(document.activeElement?.textContent).toContain('Rename')

    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[aria-label="Work actions"]')).toBeNull()
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('shows nested collections as a tree with nesting actions and a child-aware delete', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] },
        { id: 'other', name: 'Other', collapsed: false, references: [] },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      const parentChildren = host.querySelector('[data-testid="collection-children-homelab"]') as HTMLElement
      expect(parentChildren.querySelector('[data-testid="folder-collection-services"] [data-testid="folder-row-project"]')).toBeTruthy()
      const toggle = host.querySelector('[data-testid="collection-header-homelab"] button[aria-label="Collapse Homelab"]') as HTMLButtonElement
      expect(toggle.getAttribute('aria-controls')).toBe(parentChildren.id)

      // Nested collection menu: keyboard-reachable actions for nesting and leaving the parent.
      const trigger = host.querySelector('button[aria-label="Actions for Services"]') as HTMLButtonElement
      act(() => trigger.click())
      let menu = document.body.querySelector('[role="menu"][aria-label="Services actions"]') as HTMLElement
      expect(menu.textContent).toContain('New collection inside')
      const top = Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('Move to top level')) as HTMLButtonElement
      await act(async () => { top.click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections.find((collection) => collection.id === 'services')?.parentId).toBeUndefined()

      // Move to collection lists only valid targets with their path.
      act(() => (host.querySelector('button[aria-label="Actions for Homelab"]') as HTMLButtonElement).click())
      menu = document.body.querySelector('[role="menu"][aria-label="Homelab actions"]') as HTMLElement
      expect(menu.textContent).not.toContain('Move to top level')
      act(() => (Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('Move to collection')) as HTMLButtonElement).click())
      const submenu = document.body.querySelector('[role="menu"][aria-label="Move Homelab to collection"]') as HTMLElement
      const labels = Array.from(submenu.querySelectorAll('button')).map((button) => button.textContent)
      expect(labels).toEqual(['Other', 'Services'])
      await act(async () => { (Array.from(submenu.querySelectorAll('button')).find((button) => button.textContent === 'Services') as HTMLButtonElement).click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections.find((collection) => collection.id === 'homelab')?.parentId).toBe('services')

      // Deleting a parent offers to move children up a level.
      act(() => (host.querySelector('button[aria-label="Actions for Services"]') as HTMLButtonElement).click())
      menu = document.body.querySelector('[role="menu"][aria-label="Services actions"]') as HTMLElement
      act(() => (Array.from(menu.querySelectorAll('button')).find((button) => button.textContent === 'Delete') as HTMLButtonElement).click())
      const dialog = document.body.querySelector('[role="alertdialog"]') as HTMLElement
      expect(dialog.textContent).toContain('1 collection inside')
      const keep = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Move contents up') as HTMLButtonElement
      await act(async () => { keep.click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections.map((collection) => [collection.id, collection.parentId])).toEqual([['other', undefined], ['homelab', undefined]])
      expect(useAppStore.getState().folders.map((folder) => folder.id)).toContain('project')
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.unstubAllGlobals()
    }
  })

  describe('nested collection delete prompt', () => {
    const openPrompt = (host: HTMLElement, name: string) => {
      act(() => (host.querySelector(`button[aria-label="Actions for ${name}"]`) as HTMLButtonElement).click())
      const menu = document.body.querySelector(`[role="menu"][aria-label="${name} actions"]`) as HTMLElement
      act(() => (Array.from(menu.querySelectorAll('button')).find((button) => button.textContent === 'Delete') as HTMLButtonElement).click())
      return document.body.querySelector('[role="alertdialog"]') as HTMLElement
    }
    const withTree = async (run: (host: HTMLElement) => Promise<void> | void) => {
      vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      try {
        act(() => root.render(<FolderTree searchQuery="" />))
        await run(host)
      } finally {
        act(() => root.unmount())
        host.remove()
        vi.unstubAllGlobals()
      }
    }
    const buttons = (dialog: HTMLElement) => Array.from(dialog.querySelectorAll('button')).map((button) => button.textContent)
    const workspace = { id: 'project-ref', type: 'workspace-folder' as const, target: 'project', label: 'Project' }
    const app = { id: 'app-ref', type: 'desktop-app' as const, target: '/Applications/App.app', label: 'App' }

    it('keeps the warning and counts every level', () => withTree((host) => {
      act(() => useAppStore.setState({ resourceCollections: [
        { id: 'a', name: 'A', collapsed: false, references: [] },
        { id: 'b', name: 'B', parentId: 'a', collapsed: false, references: [] },
        { id: 'c', name: 'C', parentId: 'b', collapsed: false, references: [] },
      ] }))
      const dialog = openPrompt(host, 'A')
      expect(dialog.getAttribute('aria-label')).toBe('Delete collection A')
      expect(dialog.textContent).toContain('The collection “A” will be permanently deleted. This cannot be undone.')
      expect(dialog.textContent).toContain('It has 2 collections inside, counting nested ones.')
      expect(dialog.textContent).toContain('Move contents up moves its child collections to the top level and leaves its workspaces ungrouped.')
      expect(buttons(dialog)).toEqual(['Cancel', 'Move contents up', 'Delete all'])
    }))

    it('moves a nested collection\'s workspaces, references and children to its parent', () => withTree(async (host) => {
      act(() => useAppStore.setState({ resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: false, references: [workspace, app] },
        { id: 'sub', name: 'Sub', parentId: 'services', collapsed: false, references: [] },
      ] }))
      const dialog = openPrompt(host, 'Services')
      expect(dialog.textContent).toContain('Move contents up moves everything inside to “Homelab”.')
      await act(async () => { (Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Move contents up') as HTMLButtonElement).click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections).toEqual([
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [workspace, app] },
        { id: 'sub', name: 'Sub', parentId: 'homelab', collapsed: false, references: [] },
      ])
    }))

    it('offers the choice for a nested collection holding only references', () => withTree((host) => {
      act(() => useAppStore.setState({ resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: false, references: [workspace] },
      ] }))
      expect(buttons(openPrompt(host, 'Services'))).toEqual(['Cancel', 'Move contents up', 'Delete all'])
    }))

    it('says what a top-level collection loses when there is nowhere to move it', () => withTree((host) => {
      act(() => useAppStore.setState({ resourceCollections: [
        { id: 'solo', name: 'Solo', collapsed: false, references: [workspace, app] },
        { id: 'top', name: 'Top', collapsed: false, references: [app] },
        { id: 'kid', name: 'Kid', parentId: 'top', collapsed: false, references: [] },
      ] }))
      const solo = openPrompt(host, 'Solo')
      expect(buttons(solo)).toEqual(['Cancel', 'Delete collection'])
      expect(solo.textContent).toContain('cannot be restored. Its other links are removed.')
      expect(solo.getAttribute('aria-label')).toBe('Delete collection Solo')
      expect(solo.textContent).toContain('The collection “Solo” will be permanently deleted. This cannot be undone.')
      act(() => (Array.from(solo.querySelectorAll('button')).find((button) => button.textContent === 'Cancel') as HTMLButtonElement).click())
      expect(openPrompt(host, 'Top').textContent).toContain('leaves its workspaces ungrouped; its other links are removed.')
    }))

    it('names the parent limit only when moving references up can hit it', () => withTree(async (host) => {
      const failures: { detail?: string }[] = []
      const collect = (event: Event) => failures.push((event as CustomEvent).detail)
      window.addEventListener(APP_FAILURE_EVENT, collect)
      const original = useAppStore.getState().deleteResourceCollection
      const deleteResourceCollection = vi.fn(async () => false)
      const press = async (dialog: HTMLElement, label: string) => { await act(async () => { (Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === label) as HTMLButtonElement).click(); await Promise.resolve() }) }
      act(() => useAppStore.setState({ deleteResourceCollection, resourceCollections: [
        { id: 'solo', name: 'Solo', collapsed: false, references: [workspace] },
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: false, references: [app] },
      ] }))
      await press(openPrompt(host, 'Solo'), 'Delete collection')
      await press(openPrompt(host, 'Services'), 'Delete all')
      await press(openPrompt(host, 'Services'), 'Move contents up')
      window.removeEventListener(APP_FAILURE_EVENT, collect)
      act(() => useAppStore.setState({ deleteResourceCollection: original }))
      expect(deleteResourceCollection.mock.calls).toEqual([['solo', false], ['services', true], ['services', false]])
      expect(failures.map((failure) => failure.detail?.includes('500'))).toEqual([false, false, true])
      expect(failures[2].detail).toContain('“Homelab” may be full')
    }))

    it('uses the same prompt for a one-item selection as for the menu', () => withTree(async (host) => {
      act(() => useAppStore.setState({ resourceCollections: [
        { id: 'a', name: 'A', collapsed: false, references: [] },
        { id: 'b', name: 'B', parentId: 'a', collapsed: false, references: [] },
        { id: 'c', name: 'C', parentId: 'b', collapsed: false, references: [] },
      ] }))
      const fromMenu = openPrompt(host, 'A')
      const menuText = fromMenu.textContent
      expect(menuText).toContain('its workspaces and theirs become ungrouped')
      act(() => (Array.from(fromMenu.querySelectorAll('button')).find((button) => button.textContent === 'Cancel') as HTMLButtonElement).click())
      act(() => host.querySelector('[data-collection-id="a"] button[aria-label="A"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
      act(() => (host.querySelector('[aria-label="Delete 1 selected collection"]') as HTMLButtonElement).click())
      const fromSelection = document.body.querySelector('[role="alertdialog"]') as HTMLElement
      expect(fromSelection.textContent).toBe(menuText)
      expect(fromSelection.textContent).toContain('It has 2 collections inside, counting nested ones.')
      await act(async () => { (Array.from(fromSelection.querySelectorAll('button')).find((button) => button.textContent === 'Delete all') as HTMLButtonElement).click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections).toEqual([])
      expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
      expect(host.querySelector('[aria-label="Delete 1 selected collection"]')).toBeNull()
    }))
  })

  it('leaves collection drags to the collection or list, never to a workspace row', async () => {
    useAppStore.setState({
      resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] },
        { id: 'other', name: 'Other', collapsed: false, references: [] },
      ],
    })
    const failures: unknown[] = []
    const collect = (event: Event) => failures.push((event as CustomEvent).detail)
    window.addEventListener(APP_FAILURE_EVENT, collect)
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      const dragged = (id: string, kind: string) => {
        const data = new Map<string, string>()
        const transfer = { effectAllowed: '', get types() { return [...data.keys()] }, getData: (type: string) => data.get(type) ?? '', setData: (type: string, value: string) => { data.set(type, value) } }
        const start = new Event('dragstart', { bubbles: true })
        Object.defineProperty(start, 'dataTransfer', { value: transfer })
        act(() => { host.querySelector(`[data-testid="collection-header-${id}"]`)!.dispatchEvent(start) })
        const event = new Event(kind, { bubbles: true, cancelable: true })
        Object.defineProperty(event, 'dataTransfer', { value: transfer })
        Object.defineProperty(event, 'clientY', { value: 1 })
        return event
      }
      const row = () => host.querySelector('[data-testid="folder-row-project"]') as HTMLElement

      // Homelab cannot nest in itself: nothing accepts the drop and the row shows no insert line.
      const self = dragged('homelab', 'dragover')
      act(() => { row().dispatchEvent(self) })
      expect(self.defaultPrevented).toBe(false)
      expect(row().style.boxShadow).toBe('')
      const selfDrop = dragged('homelab', 'drop')
      await act(async () => { row().dispatchEvent(selfDrop); await Promise.resolve() })
      expect(failures).toEqual([])
      expect(useAppStore.getState().resourceCollections.map((collection) => collection.id)).toEqual(['homelab', 'other'])

      // Other can nest in Homelab: the enclosing collection accepts, the row still shows nothing.
      const valid = dragged('other', 'dragover')
      act(() => { row().dispatchEvent(valid) })
      expect(valid.defaultPrevented).toBe(true)
      expect(row().style.boxShadow).toBe('')
      const validDrop = dragged('other', 'drop')
      await act(async () => { row().dispatchEvent(validDrop); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections.find((collection) => collection.id === 'other')?.parentId).toBe('homelab')
      expect(failures).toEqual([])
    } finally {
      window.removeEventListener(APP_FAILURE_EVENT, collect)
      act(() => root.unmount())
      host.remove()
    }
  })

  it('counts nested collections and workspaces directly inside a collection in its header badge', () => {
    useAppStore.setState({
      resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: false, references: [] },
        { id: 'deep', name: 'Deep', parentId: 'services', collapsed: false, references: [] },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      const badge = (id: string) => host.querySelector(`[data-testid="collection-count-${id}"]`)?.textContent
      expect(badge('homelab')).toBe('2')
      expect(badge('services')).toBe('1')
      expect(badge('deep')).toBe('0')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('moves a collection when its submenu target gets a full press and release', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: false, references: [] },
        { id: 'services', name: 'Services', collapsed: false, references: [] },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      act(() => (host.querySelector('button[aria-label="Actions for Services"]') as HTMLButtonElement).click())
      const menu = document.body.querySelector('[role="menu"][aria-label="Services actions"]') as HTMLElement
      act(() => (Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('Move to collection')) as HTMLButtonElement).click())
      const target = document.body.querySelector('[role="menu"][aria-label="Move Services to collection"] button') as HTMLButtonElement
      // A real press reaches the document first; the submenu is portaled outside the parent menu and must survive it.
      act(() => { target.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true })) })
      expect(target.isConnected).toBe(true)
      act(() => { target.dispatchEvent(new MouseEvent('mouseup', { bubbles: true, cancelable: true })) })
      await act(async () => { target.click(); await Promise.resolve() })
      expect(useAppStore.getState().resourceCollections.find((collection) => collection.id === 'services')?.parentId).toBe('homelab')
      expect(document.body.querySelector('[role="menu"][aria-label="Services actions"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.unstubAllGlobals()
    }
  })

  it('search still reaches workspaces referenced by nested collections', () => {
    useAppStore.setState({
      resourceCollections: [
        { id: 'homelab', name: 'Homelab', collapsed: true, references: [] },
        { id: 'services', name: 'Services', parentId: 'homelab', collapsed: true, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="Chat 1" />))
      expect(host.querySelector('[data-testid="folder-row-project"]')).toBeTruthy()
      expect(host.querySelector('[data-testid="folder-collection-homelab"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('opens collection actions from the collection context menu', () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const header = host.querySelector('[data-testid="folder-collection-work"] > div') as HTMLElement
    act(() => header.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 80, clientY: 90 })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Work actions"]')
    expect(menu?.textContent).toContain('Rename')
    expect(menu?.textContent).toContain('Delete')

    const rows = Array.from(menu?.querySelectorAll('.uam-sb-menu-item') ?? [])
    expect(rows.length).toBeGreaterThan(0)
    for (const row of rows) {
      expect(row.querySelector('svg')).not.toBeNull()
    }
    const deleteRow = rows.find((r) => r.textContent?.includes('Delete'))
    expect(deleteRow?.className).toContain('uam-sb-menu-item--danger')
    expect(menu?.querySelector('.uam-sb-menu-divider')).not.toBeNull()

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('removes a workspace from its collection without deleting the workspace or chats', async () => {
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collectedFolder = host.querySelector('[data-testid="folder-collection-work"] [data-testid="folder-row-project"]') as HTMLElement
    act(() => collectedFolder.firstElementChild?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 20 })))

    const menu = document.body.querySelector('[data-viewport-menu]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(collectedFolder.contains(menu)).toBe(false)
    expect(menu.textContent).not.toContain('Remove from collection')
    const move = Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('Move to collection')) as HTMLButtonElement
    act(() => move.click())
    const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
    expect(submenu).toBeTruthy()
    const remove = Array.from(submenu.querySelectorAll('button')).find((button) => button.textContent?.includes('Remove from collection'))
    await act(async () => { remove?.click(); await Promise.resolve() })

    expect(useAppStore.getState().resourceCollections[0].references).toEqual([])
    expect(useAppStore.getState().folders.map((folder) => folder.id)).toContain('project')
    expect(useAppStore.getState().sessions).toHaveLength(7)
    expect(host.querySelector('[data-testid="folder-collection-work"] [data-testid="folder-row-project"]')).toBeNull()
    expect(host.querySelector('[data-testid="folder-row-project"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('closes the collection submenu after the pointer leaves both menu surfaces', () => {
    vi.useFakeTimers()
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-header-project"]')
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) =>
      button.textContent?.includes('Move to collection')
    ) as HTMLButtonElement
    act(() => {
      move.dispatchEvent(new MouseEvent('mouseover', { bubbles: true }))
      vi.advanceTimersByTime(160) // hover intent before the submenu opens
    })
    const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
    expect(submenu).toBeTruthy()

    act(() => {
      move.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: submenu }))
      submenu.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, relatedTarget: move }))
      submenu.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: document.body }))
      vi.advanceTimersByTime(320) // close delay, then exit motion
    })

    expect(document.body.querySelector('[role="menu"][aria-label="Move to collection"]')).toBeNull()
    expect(document.body.querySelector('[data-viewport-menu]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })

  it('opens and closes the collection submenu with nested-menu keyboard controls', () => {
    vi.useFakeTimers()
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-header-project"]')
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) =>
      button.textContent?.includes('Move to collection')
    ) as HTMLButtonElement
    for (const [openKey, closeKey] of [['ArrowRight', 'ArrowLeft'], ['Enter', 'Escape'], [' ', 'ArrowLeft']]) {
      act(() => {
        move.focus()
        move.dispatchEvent(new KeyboardEvent('keydown', { key: openKey, bubbles: true }))
      })

      const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
      expect(submenu).toBeTruthy()
      expect(submenu.contains(document.activeElement)).toBe(true)

      act(() => submenu.dispatchEvent(new KeyboardEvent('keydown', { key: closeKey, bubbles: true })))
      act(() => { vi.advanceTimersByTime(200) }) // exit motion keeps the submenu mounted briefly
      expect(document.body.querySelector('[role="menu"][aria-label="Move to collection"]')).toBeNull()
      expect(document.activeElement).toBe(move)
      expect(document.body.querySelector('[data-viewport-menu]')).toBeTruthy()
    }

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })

  it('marks a missing workspace folder accessibly without removing its chats', () => {
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, missing: true })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const warning = host.querySelector('[data-testid="folder-missing-project"]')
    expect(warning?.getAttribute('aria-label')).toBe('Workspace folder missing: /tmp/project')
    expect(host.textContent).toContain('Chat 1')

    act(() => root.unmount())
    host.remove()
  })

  it('moves siblings aside while dragging and persists the dropped order', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async (ids: string[]) => {
      useAppStore.setState((state) => ({ folders: ids.flatMap((id) => state.folders.filter((folder) => folder.id === id)) }))
      return true
    })
    useAppStore.setState({ folders: [makeFolder(), second], sessions: [], reorderFolders })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    stubRect(source, 0, 40)
    stubRect(target, 40, 40)
    const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!

    // A press that moves less than the threshold stays a click.
    act(() => pointer(header, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 11, 12))
    expect(source.style.transform).toBe('')
    act(() => pointer(document, 'pointermove', 10, 65))
    expect(source.style.transform).toBe('translate3d(0, 55px, 0) scale(1.02)')
    expect(target.style.transform).toBe('translate3d(0, -40px, 0)')
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Moving Project. Press Escape to cancel.')
    const expanded = header.getAttribute('aria-expanded')
    await act(async () => {
      pointer(document, 'pointerup', 10, 65)
      // The click that follows the release ends the drag; it does not toggle the workspace.
      header.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(header.getAttribute('aria-expanded')).toBe(expanded)
    expect(reorderFolders).toHaveBeenCalledWith(['second', 'project'])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Project moved to position 2 of 2')
    // Held transforms drop once the saved order is on screen.
    expect([...host.querySelectorAll('[data-sort-id]')].map((row) => (row as HTMLElement).dataset.sortId)).toEqual(['second', 'project'])
    expect(source.style.transform).toBe('')
    expect(target.style.transform).toBe('')

    await releaseClickGuard()
    act(() => root.unmount())
    host.remove()
  })

  it('drags a workspace out of its collection to the top level', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    useAppStore.setState({
      folders: [makeFolder(), second],
      sessions: [],
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const outside = host.querySelector<HTMLElement>('[data-testid="folder-header-second"]')!
    Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: () => outside })
    try {
      const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!
      act(() => pointer(header, 'pointerdown', 10, 10))
      act(() => pointer(document, 'pointermove', 10, 80))
      await act(async () => { pointer(document, 'pointerup', 10, 80); await Promise.resolve() })
    } finally {
      delete (document as { elementFromPoint?: unknown }).elementFromPoint
    }
    expect(useAppStore.getState().resourceCollections[0].references).toEqual([])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Project removed from its collection')

    await releaseClickGuard()
    act(() => root.unmount())
    host.remove()
  })

  it('reorders Pinned chats by drag and keeps the saved order', async () => {
    const settled = new Date(now).toISOString()
    useAppStore.setState({
      sessions: [1, 2, 3].map((index) => ({ ...makeSession(index), isPinned: true, settledAt: settled })),
      // chat-snoozed is a pin hidden by its snooze; reordering the visible pins keeps its place.
      sidebarPinnedChatOrder: ['chat-1', 'chat-2', 'chat-3', 'chat-snoozed'],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const pinned = () => [...host.querySelectorAll<HTMLElement>('#pinned-chat-list [data-sort-id]')]
    // No saved order yet: the default, most recent first.
    expect(pinned().map((row) => row.dataset.sortId)).toEqual(['chat-1', 'chat-2', 'chat-3'])
    pinned().forEach((row, index) => stubRect(row, index * 30, 30))
    const [first, second, third] = pinned()
    // Pinned rows drag by pointer, not by the native split-pane drag.
    expect(first.draggable).toBe(false)

    act(() => pointer(first.querySelector('[data-session-id]')!, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 10, 85))
    expect(second.style.transform).toBe('translate3d(0, -30px, 0)')
    expect(third.style.transform).toBe('translate3d(0, -30px, 0)')
    await act(async () => { pointer(document, 'pointerup', 10, 85); await Promise.resolve() })

    expect(useAppStore.getState().sidebarPinnedChatOrder).toEqual(['chat-2', 'chat-3', 'chat-1', 'chat-snoozed'])
    expect(pinned().map((row) => row.dataset.sortId)).toEqual(['chat-2', 'chat-3', 'chat-1'])
    expect(first.style.transform).toBe('')
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Chat 1 moved to position 3 of 3')
    // A press inside the row's portalled menu bubbles through React to the row but never drags it.
    const menuRow = pinned()[0]
    act(() => { menuRow.querySelector('[data-session-id]')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 20, clientY: 20 })) })
    const menu = document.querySelector<HTMLElement>('[role="menu"]')!
    expect(menuRow.contains(menu)).toBe(false)
    act(() => pointer(menu, 'pointerdown', 20, 20))
    act(() => pointer(document, 'pointermove', 20, 80))
    expect(menuRow.hasAttribute('data-sort-dragging')).toBe(false)
    act(() => pointer(document, 'pointerup', 20, 80))
    act(() => { document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
    // Workspace chats keep their time order and native drag.
    expect((host.querySelector('[data-testid="folder-row-project"] [data-session-id="chat-1"]')?.parentElement as HTMLElement).draggable).toBe(true)
    await releaseClickGuard()

    act(() => root.unmount())
    host.remove()
  })

  it('moves an Active chat with Arrow keys, announces it and keeps focus', async () => {
    // Recent unsettled work puts both chats in Active.
    const recent = (index: number) => { const at = new Date(Date.now() - index * 60_000); return { ...makeSession(index), createdAt: at, updatedAt: at, lastOpenedAt: at } }
    useAppStore.setState({ sessions: [recent(1), recent(2)], sidebarActiveChatOrder: [] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const active = () => [...host.querySelectorAll<HTMLElement>('#active-chat-list [data-sort-id]')].map((row) => row.dataset.sortId)
    expect(active()).toEqual(['chat-1', 'chat-2'])
    const row = host.querySelector<HTMLElement>('#active-chat-list [data-session-id="chat-1"]')!
    expect(row.getAttribute('aria-keyshortcuts')).toBe('ArrowUp ArrowDown')
    row.focus()

    await act(async () => { row.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })); await Promise.resolve() })
    expect(useAppStore.getState().sidebarActiveChatOrder).toEqual(['chat-2', 'chat-1'])
    expect(active()).toEqual(['chat-2', 'chat-1'])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Chat 1 moved to position 2 of 2')
    expect((document.activeElement as HTMLElement | null)?.dataset.sessionId).toBe('chat-1')

    // The last row has nowhere further down to go.
    await act(async () => { document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })); await Promise.resolve() })
    expect(active()).toEqual(['chat-2', 'chat-1'])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Chat 1 is already last')

    // Search shows a filtered view, so rows are not sortable there.
    act(() => root.render(<FolderTree searchQuery="Chat" />))
    expect(host.querySelector('#active-chat-list [data-sort-id]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('opens a dragged Pinned chat in the pane it is dropped on', async () => {
    const settled = new Date(now).toISOString()
    const setActiveSession = vi.fn()
    useAppStore.setState({ sessions: [1, 2].map((index) => ({ ...makeSession(index), isPinned: true, settledAt: settled })), setActiveSession })
    const layouts: ChatGridLayout[] = []
    const unsubscribe = subscribeChatGridLayout((layout) => layouts.push(layout))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const leafId = chatGridLeaves(readChatGridLayout().root)[0].id
    const pane = document.createElement('div')
    pane.dataset.leafId = leafId
    document.body.appendChild(pane)
    Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: () => pane })
    try {
      const row = host.querySelector<HTMLElement>('#pinned-chat-list [data-sort-id="chat-2"]')!
      act(() => pointer(row.querySelector('[data-session-id]')!, 'pointerdown', 10, 10))
      act(() => pointer(document, 'pointermove', 400, 10))
      expect(pane.hasAttribute('data-pointer-drop')).toBe(true)
      await act(async () => { pointer(document, 'pointerup', 400, 10); await Promise.resolve() })
      expect(pane.hasAttribute('data-pointer-drop')).toBe(false)
    } finally {
      delete (document as { elementFromPoint?: unknown }).elementFromPoint
      pane.remove()
    }
    unsubscribe()
    expect(chatGridLeaves(layouts.at(-1)!.root)[0].sessionId).toBe('chat-2')
    expect(setActiveSession).toHaveBeenCalledWith('chat-2')
    expect(useAppStore.getState().sidebarPinnedChatOrder).toEqual([])
    await releaseClickGuard()
    useAppStore.setState({ setActiveSession: originalSetActiveSession })

    act(() => root.unmount())
    host.remove()
  })

  it('keeps a short press a click and glides back when a dropped order fails to save', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async () => false)
    useAppStore.setState({ folders: [makeFolder(), second], sessions: [], reorderFolders })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    stubRect(source, 0, 40)
    stubRect(target, 40, 40)
    const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!

    // Under the threshold the press stays a click and toggles the workspace.
    const expanded = header.getAttribute('aria-expanded')
    act(() => {
      pointer(header, 'pointerdown', 10, 10)
      pointer(document, 'pointermove', 12, 11)
      pointer(document, 'pointerup', 12, 11)
      header.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(header.getAttribute('aria-expanded')).not.toBe(expanded)

    act(() => pointer(header, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 10, 65))
    await act(async () => { pointer(document, 'pointerup', 10, 65); await Promise.resolve() })
    expect(reorderFolders).toHaveBeenCalledWith(['second', 'project'])
    // The refused save leaves the order alone and hands the rows back to React.
    expect([...host.querySelectorAll('[data-sort-id]')].map((row) => (row as HTMLElement).dataset.sortId)).toEqual(['project', 'second'])
    expect(source.style.transform).toBe('')
    expect(source.hasAttribute('data-sort-dragging')).toBe(false)
    expect(target.style.transform).toBe('')
    await releaseClickGuard()

    // A later drag still starts.
    act(() => pointer(header, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 10, 65))
    expect(source.hasAttribute('data-sort-dragging')).toBe(true)
    act(() => { document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
    act(() => pointer(document, 'pointerup', 10, 65))
    await releaseClickGuard()

    act(() => root.unmount())
    host.remove()
  })

  it('drags a workspace from one collection into another', async () => {
    useAppStore.setState({
      sessions: [],
      resourceCollections: [
        { id: 'work', name: 'Work', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] },
        { id: 'home', name: 'Home', collapsed: false, references: [] },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const home = host.querySelector<HTMLElement>('[data-testid="collection-header-home"]')!
    Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: () => home })
    try {
      const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!
      act(() => pointer(header, 'pointerdown', 10, 10))
      act(() => pointer(document, 'pointermove', 10, 80))
      expect(host.querySelector('[data-testid="folder-collection-home"]')?.classList.contains('uam-sb-drop-target')).toBe(true)
      expect(host.querySelector('[data-testid="folder-collection-work"]')?.classList.contains('uam-sb-drop-target')).toBe(false)
      await act(async () => { pointer(document, 'pointerup', 10, 80); await Promise.resolve() })
    } finally {
      delete (document as { elementFromPoint?: unknown }).elementFromPoint
    }
    const collections = useAppStore.getState().resourceCollections
    expect(collections.find((collection) => collection.id === 'work')?.references).toEqual([])
    expect(collections.find((collection) => collection.id === 'home')?.references.map((reference) => reference.target)).toEqual(['project'])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Project moved into Home')
    await releaseClickGuard()

    act(() => root.unmount())
    host.remove()
  })

  it('cancels a workspace drag with Escape and saves nothing', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async () => true)
    useAppStore.setState({ folders: [makeFolder(), second], sessions: [], reorderFolders })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    stubRect(source, 0, 40)
    stubRect(target, 40, 40)
    const header = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!

    act(() => pointer(header, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 10, 65))
    expect(target.style.transform).toBe('translate3d(0, -40px, 0)')
    act(() => { document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
    const expanded = header.getAttribute('aria-expanded')
    act(() => {
      pointer(document, 'pointerup', 10, 65)
      header.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(header.getAttribute('aria-expanded')).toBe(expanded)
    expect(source.style.transform).toBe('')
    expect(target.style.transform).toBe('')
    expect(reorderFolders).not.toHaveBeenCalled()
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Move of Project cancelled')

    await releaseClickGuard()
    act(() => root.unmount())
    host.remove()
  })

  it('persists dragged workspace order inside a collection', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async () => true)
    // Saves apply like the native acknowledgement does, so the drop settles and the keyboard path can follow.
    const reorderResourceReferences = vi.fn(async (collectionId: string, ids: string[]) => {
      useAppStore.setState((state) => ({ resourceCollections: state.resourceCollections.map((collection) => collection.id === collectionId
        ? { ...collection, references: ids.flatMap((id) => collection.references.filter((reference) => reference.id === id)) }
        : collection) }))
      return true
    })
    useAppStore.setState({
      folders: [makeFolder(), second],
      sessions: [],
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [
          { id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' },
          { id: 'website-ref', type: 'website', target: 'https://example.com', label: 'Example' },
          { id: 'second-ref', type: 'workspace-folder', target: 'second', label: 'Second' },
        ],
      }],
      reorderFolders,
      reorderResourceReferences,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const keyboardSource = host.querySelector('[data-testid="folder-header-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    stubRect(source, 0, 40)
    stubRect(target, 40, 40)

    act(() => pointer(keyboardSource, 'pointerdown', 10, 10))
    act(() => pointer(document, 'pointermove', 10, 65))
    await act(async () => { pointer(document, 'pointerup', 10, 65); await Promise.resolve() })

    expect(host.querySelector('[data-testid="folder-drag-handle-project"]')).toBeNull()
    expect(reorderResourceReferences).toHaveBeenCalledWith('work', ['second-ref', 'website-ref', 'project-ref'])
    expect(reorderFolders).not.toHaveBeenCalled()

    reorderResourceReferences.mockClear()
    const header = () => host.querySelector('[data-testid="folder-header-project"]') as HTMLElement
    await act(async () => { header().dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true })); await Promise.resolve() })
    expect(reorderResourceReferences).toHaveBeenCalledWith('work', ['project-ref', 'website-ref', 'second-ref'])
    expect(host.querySelector('[data-testid="sidebar-move-status"]')?.textContent).toBe('Project moved to position 1 of 2')

    await releaseClickGuard()
    act(() => root.unmount())
    host.remove()
  })

  it('selects a visible chat row after folders are grouped for rendering', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    const chatTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 2')
    expect(chatTitle).toBeTruthy()

    act(() => {
      chatTitle?.parentElement?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows every assigned pane colour when a collapsed folder layout changes', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, isExpanded: false })) }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.textContent).not.toContain('Chat 3')
    expect((host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement).style.color).toBe('var(--text-3)')

    act(() => writeChatGridLayout(gridLayout('chat-1', 'chat-3', '', '')))

    const folderIcon = host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement
    expect(folderIcon.getAttribute('stroke')).not.toMatch(/^url\(#.+\)$/)
    expect(folderIcon.style.color).toBe('var(--text-3)')
    expect(folderIcon.style.filter).toBe('none')
    expect(host.querySelector('[role="img"][aria-label="Shown in panes 1 and 2"]')).toBeTruthy()
    expect(Array.from(host.querySelectorAll('[data-testid="folder-icon-project-marker"]')).map((marker) => (marker as HTMLElement).style.background)).toEqual([
      'rgb(249, 115, 22)', 'rgb(236, 72, 153)',
    ])

    act(() => root.unmount())
    host.remove()
  })

  it('shows every hidden workspace pane colour on a collapsed collection', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: true,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => writeChatGridLayout(gridLayout('chat-1', 'chat-3', '', '')))

    const collectionIcon = host.querySelector('[data-testid="collection-icon-work"]') as HTMLElement
    expect(collectionIcon.getAttribute('stroke')).not.toMatch(/^url\(#.+\)$/)
    expect(collectionIcon.style.color).toBe('var(--text-3)')
    expect(host.querySelector('[role="img"][aria-label="Shown in panes 1 and 2"]')).toBeTruthy()
    expect(Array.from(host.querySelectorAll('[data-testid="collection-icon-work-marker"]')).map((marker) => (marker as HTMLElement).style.background)).toEqual([
      'rgb(249, 115, 22)', 'rgb(236, 72, 153)',
    ])

    act(() => root.unmount())
    host.remove()
  })

  it('selects a chat row after filters change the rendered group', () => {
    useAppStore.setState((state) => ({
      activeSessionId: 'chat-1',
      sessions: state.sessions.map((session) =>
        session.id === 'chat-6' ? { ...session, providerId: 'codex-cli' } : session
      ),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" filters={{ providerIds: ['codex-cli'], statusIds: [] }} />)
    })

    expect(host.textContent).toContain('Chat 6')
    expect(host.textContent).not.toContain('Chat 1')

    const chatTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 6')
    expect(chatTitle).toBeTruthy()

    act(() => {
      chatTitle?.parentElement?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-6')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps Claude launch, streaming, completion and resume visible in two views', () => {
    const hosts = [document.createElement('div'), document.createElement('div')]
    const roots = hosts.map((host) => { document.body.appendChild(host); return createRoot(host) })
    act(() => {
      useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli' } : session) }))
      roots.forEach((root) => root.render(<FolderTree searchQuery="" />))
    })
    for (const state of [
      { running: false, processing: false, lifecycleState: 'starting', readySinceLastSelect: false },
      { running: true, processing: true, lifecycleState: 'processing', readySinceLastSelect: false },
      { running: true, processing: false, lifecycleState: 'ready', readySinceLastSelect: true },
      { running: false, processing: false, lifecycleState: 'stopped', readySinceLastSelect: true },
      { running: false, processing: false, lifecycleState: 'starting', readySinceLastSelect: true },
    ]) {
      act(() => useAppStore.setState({ acpBindingBySessionId: { 'chat-1': state } }))
      hosts.forEach((host) => expect(host.querySelector('[data-testid="active-chats"] [data-session-id="chat-1"]')).toBeTruthy())
    }
    act(() => useAppStore.setState({ acpBindingBySessionId: { 'chat-1': { running: false, processing: false, lifecycleState: 'stopped', readySinceLastSelect: false } } }))
    hosts.forEach((host) => expect(host.querySelector('[data-testid="active-chats"] [data-session-id="chat-1"]')).toBeNull())
    act(() => roots.forEach((root) => root.unmount()))
    hosts.forEach((host) => host.remove())
  })

  it('shows runtime-active chats with state counts above pinned chats without requiring a status filter', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-2' ? { ...session, isPinned: true } : session),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })
    expect(host.textContent).not.toContain('Active chats')

    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { processing: true })
      useAppStore.getState().setCliBinding('chat-3', { readySinceLastSelect: true })
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-2': { running: true, processing: false, lifecycleState: 'waitingPermission', attentionKind: 'permission' },
        },
      }))
    })

    expect(host.textContent).toContain('Active chats')
    expect(host.textContent!.indexOf('Active chats')).toBeLessThan(host.textContent!.indexOf('Pinned chats'))
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 attention')
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 done')

    act(() => useAppStore.getState().setCliBinding('chat-1', {
      processing: false,
      readySinceLastSelect: true,
    }))

    expect(host.textContent).toContain('Active chats')
    expect(host.querySelector('[data-testid="active-chats"] [data-session-id="chat-1"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="folder-row-project"] [data-session-id="chat-1"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('uses the T3-style folder, status and host layout only for Active chats', () => {
    useAppStore.setState({
      sessions: [
        { ...makeSession(1), id: 'active', name: 'Build fixes', workspaceDirectory: '/workspaces/uam', updatedAt: now },
        { ...makeSession(2), id: 'pinned', name: 'Pinned chat', isPinned: true },
      ],
    })
    act(() => useAppStore.setState({ acpBindingBySessionId: { active: { running: true, processing: true, lifecycleState: 'processing', processingStartedAtMs: null } as never } }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const active = host.querySelector<HTMLElement>('[data-testid="active-chats"] [data-session-id="active"]')
    const pinned = host.querySelector<HTMLElement>('[data-testid="pinned-chats"] [data-session-id="pinned"]')
    expect(active?.className).toContain('py-1.5')
    expect(active?.style.opacity).toBe('0.95')
    expect(active?.querySelector('[role="status"]')?.textContent).toBe('Working')
    expect(active?.textContent).toContain('Project')
    expect(active?.textContent).toContain('Build fixes')
    expect(active?.querySelector('[aria-label="Runs on This computer"], [aria-label^="Runs on"]')).toBeTruthy()
    expect(active?.querySelector('button[aria-label^="Mark"]')).toBeNull()
    expect(pinned?.className).toContain('min-h-[26px]')
    act(() => useAppStore.setState({ acpBindingBySessionId: { active: { running: true, processing: false, lifecycleState: 'ready', backgroundTaskCount: 1, readySinceLastSelect: true, processingStartedAtMs: null } as never } }))
    expect(host.querySelector('[data-testid="active-chats"] [data-session-id="active"] [role="status"]')?.textContent).toBe('Working')
    act(() => useAppStore.setState({ acpBindingBySessionId: { active: { running: true, processing: false, lifecycleState: 'ready', backgroundTaskCount: 0, readySinceLastSelect: true, processingStartedAtMs: null } as never } }))
    expect(host.querySelector('[data-testid="active-chats"] [data-session-id="active"] [role="status"]')?.textContent).not.toBe('Working')


    act(() => root.unmount())
    host.remove()
  })

  it('labels a held automation chat as waiting and offers no Done or Snooze', () => {
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-2' ? { ...session, updatedAt: new Date(), attentionRevision: 'held', automationPendingPrompt: 'Fix the parser' } : session) })))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.textContent).toContain('Waiting for approval')
    expect(host.querySelector('[aria-label^="Mark "][aria-label$=" done"]')).toBeNull()
    expect(host.querySelector('[aria-label^="Snooze "]')).toBeNull()
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 attention')
    // Clear all, bulk Done and ageing only touch settledAt or time; none may hide an unapproved prompt.
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-2' ? { ...session, updatedAt: new Date(0), settledAt: new Date().toISOString(), snoozedUntilMs: Date.now() + 3_600_000 } : session) })))
    expect(host.textContent).toContain('Waiting for approval')
    expect(host.textContent).not.toContain('Snoozed')
    act(() => root.unmount())
    host.remove()
  })

  it('shows a started automation chat as Working through the worktree wait even if it was settled', () => {
    const chat = useAppStore.getState().sessions.find((session) => session.id === 'chat-2')!
    const started = { ...chat, automationTokenName: 'n8n', updatedAt: new Date(0), settledAt: new Date().toISOString() }
    addOutgoingAttempt(outgoingScope(chat.id, chat.createdAt), { key: automationPromptRowKey(chat.id), actionCorrelationId: 'start-1', text: 'Fix', nativeIndex: 0, attachments: [], receiptIds: new Set(), createdAt: new Date(), phase: 'accepted' })
    act(() => useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === chat.id ? started : session),
      acpBindingBySessionId: { ...state.acpBindingBySessionId, [chat.id]: { running: false, processing: false, pendingDirectPrompts: [{ originRequestId: 'start-1' }] } as never },
    })))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.textContent).toContain('Working')
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 running')
    act(() => root.unmount())
    host.remove()
    resetOutgoingPresentation()
  })

  it('collapses Active chats and keeps an attention strip that expands the section', () => {
    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { processing: true })
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-2': { running: true, processing: false, lifecycleState: 'waitingPermission', attentionKind: 'permission' },
        },
      }))
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const toggle = host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Active chats"]')
    const list = host.querySelector<HTMLElement>('#active-chat-list')
    expect(toggle?.tagName).toBe('BUTTON')
    expect(toggle?.getAttribute('aria-expanded')).toBe('true')
    act(() => toggle?.focus())
    expect(document.activeElement).toBe(toggle)

    act(() => toggle?.click())
    expect(host.querySelector('button[aria-label="Expand Active chats"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(list?.hidden).toBe(true)
    expect(list?.hasAttribute('inert')).toBe(true)
    expect(list?.querySelectorAll('[data-session-id]').length).toBe(0)
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 attention')
    const strip = host.querySelector<HTMLButtonElement>('[data-testid="active-attention-strip"]')
    expect(strip?.textContent).toContain('1 chat needs attention')

    act(() => root.render(<FolderTree searchQuery="Chat 2" />))
    expect(host.querySelector('button[aria-label="Collapse Active chats"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(list?.hidden).toBe(false)
    expect(host.querySelector('[data-testid="active-attention-strip"]')).toBeNull()

    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelector('button[aria-label="Expand Active chats"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(list?.hidden).toBe(true)
    const restoredStrip = host.querySelector<HTMLButtonElement>('[data-testid="active-attention-strip"]')
    expect(restoredStrip?.textContent).toContain('1 chat needs attention')

    act(() => restoredStrip?.click())
    expect(host.querySelector('button[aria-label="Collapse Active chats"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(list?.hidden).toBe(false)
    expect(list?.hasAttribute('inert')).toBe(false)
    expect(host.querySelector('[data-testid="active-attention-strip"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('collapses Pinned chats but reveals matching rows while searching', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, isPinned: true } : session
      ),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const toggle = host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Pinned chats"]')
    const list = host.querySelector<HTMLElement>('#pinned-chat-list')
    expect(toggle?.tagName).toBe('BUTTON')
    act(() => toggle?.focus())
    expect(document.activeElement).toBe(toggle)
    act(() => toggle?.click())
    expect(host.querySelector('button[aria-label="Expand Pinned chats"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(list?.hidden).toBe(true)
    expect(list?.hasAttribute('inert')).toBe(true)
    expect(list?.querySelectorAll('[data-session-id]').length).toBe(0)

    act(() => root.render(<FolderTree searchQuery="Chat 1" />))
    expect(host.querySelector('button[aria-label="Collapse Pinned chats"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(list?.hidden).toBe(false)
    expect(list?.hasAttribute('inert')).toBe(false)
    expect(list?.textContent).toContain('Chat 1')

    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelector('button[aria-label="Expand Pinned chats"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(list?.hidden).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('unmounts Snoozed rows while collapsed and restores current chat data', () => {
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) =>
      session.id === 'chat-1' ? { ...session, snoozedUntilMs: Date.now() + 600000 } : session) }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const list = host.querySelector<HTMLElement>('#snoozed-chat-list')
    expect(list?.querySelector('[data-session-id="chat-1"]')).not.toBeNull()
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Snoozed"]')?.click())
    expect(list?.querySelectorAll('[data-session-id]').length).toBe(0)
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) =>
      session.id === 'chat-1' ? { ...session, name: 'Renamed snoozed chat' } : session) })))
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand Snoozed"]')?.click())
    expect(list?.textContent).toContain('Renamed snoozed chat')
    act(() => root.unmount())
    host.remove()
  })

  it('unmounts collapsed All chats rows and restores them on expand or search', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const list = host.querySelector<HTMLElement>('#all-chat-list')
    expect(list?.querySelectorAll('[data-session-id]').length).toBeGreaterThan(0)
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Collapse All chats"]')?.click())
    expect(list?.querySelectorAll('[data-session-id]').length).toBe(0)
    act(() => root.render(<FolderTree searchQuery="Chat 1" />))
    expect(list?.querySelector('[data-session-id="chat-1"]')).not.toBeNull()
    act(() => root.render(<FolderTree searchQuery="" />))
    expect(list?.querySelectorAll('[data-session-id]').length).toBe(0)
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand All chats"]')?.click())
    expect(list?.querySelector('[data-session-id="chat-1"]')).not.toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('shift-selects from the clicked duplicate row occurrence', () => {
    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { processing: true })
      useAppStore.getState().setCliBinding('chat-3', { processing: true })
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folder = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const first = folder.querySelector('[data-session-id="chat-1"]') as HTMLElement
    const second = folder.querySelector('[data-session-id="chat-2"]') as HTMLElement
    act(() => first.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    act(() => second.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))

    expect(host.textContent).toContain('2 chats selected')
    expect(host.querySelector('[data-testid="session-row-chat-3"][data-selected="true"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('renders pinned chats above folders and pin clicks do not select chats', () => {
    useAppStore.setState((state) => ({
      activeSessionId: 'chat-3',
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, isPinned: true } : session
      ),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    expect(host.textContent).toContain('Pinned chats')
    expect(host.textContent).toContain('Chat 1')

    expect(host.querySelector('button[aria-label="Unpin Chat 1"]')).toBeTruthy()
    const pinButton = host.querySelector<HTMLButtonElement>('button[aria-label="Pin chat"]')
    expect(pinButton).toBeTruthy()
    const actions = pinButton?.closest<HTMLElement>('[data-testid^="session-actions-"]')
    expect(actions?.className).toContain('absolute')
    expect(actions?.className).toContain('opacity-0')
    expect(actions?.className).toContain('group-hover:opacity-100')
    expect(actions?.className).toContain('group-focus-within:opacity-100')

    act(() => {
      pinButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-3')
    expect(useAppStore.getState().sessions.find((session) => session.id === 'chat-2')?.isPinned).toBe(true)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('offers no standalone Project memory window from the folder actions', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    // Folder actions are now in an overflow menu — open it first.
    act(() => {
      (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    // Memory lives in Settings → Memory Store only (David, 2026-10-25).
    const menu = document.body.querySelector('[role="menu"][aria-label="Project actions"]')
    expect(menu?.textContent).toContain('New chat')
    expect(menu?.textContent).not.toContain('memory')
    expect(useAppStore.getState().openFolderMemoryLibrary).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('treats folder actions as a keyboard menu and restores trigger focus', () => {
    const previousResizeObserver = globalThis.ResizeObserver
    Object.defineProperty(globalThis, 'ResizeObserver', {
      configurable: true,
      value: class {
        observe() {}
        unobserve() {}
        disconnect() {}
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const trigger = host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement
    trigger.focus()
    act(() => trigger.click())
    const menu = document.body.querySelector('[role="menu"][aria-label="Project actions"]')
    expect(trigger.getAttribute('aria-expanded')).toBe('true')
    expect(document.activeElement?.textContent).toContain('New chat')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Review chats')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.contains(menu)).toBe(false)
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
    Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, value: previousResizeObserver })
  })

  it('confirms a single workspace delete with the shared sidebar delete confirmation', async () => {
    const deleteFolder = vi.fn(async () => true)
    useAppStore.setState({ deleteFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const openDelete = () => {
      act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
      act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Delete folder'))!.click())
      return document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label^="Delete workspace Project"]')!
    }
    const dialog = openDelete()
    expect(dialog).toBeTruthy()
    expect(dialog.textContent).toContain('Delete workspace and chats?')
    expect(dialog.textContent).toMatch(/The workspace “Project” and \d+ chats? will be permanently deleted\. This cannot be undone\./)
    expect(dialog.textContent).toContain('The folder on disk is kept.')
    expect(dialog.textContent).toContain('Deleted chats cannot be restored from this app.')
    expect(dialog.textContent).toContain('/tmp/project')
    expect(Array.from(dialog.querySelectorAll('button')).map((button) => button.textContent)).toEqual(['Cancel', 'Delete workspace'])
    expect(document.activeElement?.textContent).toBe('Cancel')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    expect(deleteFolder).not.toHaveBeenCalled()
    const again = openDelete()
    await act(async () => Array.from(again.querySelectorAll('button')).find((button) => button.textContent === 'Delete workspace')!.click())
    expect(deleteFolder).toHaveBeenCalledExactlyOnceWith('project')
    act(() => root.unmount())
    host.remove()
  })

  it('reads a one-workspace selection delete like the menu delete, omits zero chats and returns focus to the row', () => {
    useAppStore.setState({ folders: [makeFolder(), { ...makeFolder(), id: 'empty', name: 'Empty', directory: '/tmp/empty' }] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const row = host.querySelector<HTMLElement>('[data-workspace-id="project"]')!
    act(() => row.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Delete 1 selected workspace"]')!.click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete workspace Project and 7 chats"]')!
    expect(dialog.textContent).toContain('The workspace “Project” and 7 chats will be permanently deleted.')
    expect(dialog.textContent).toContain('/tmp/project')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.activeElement).toBe(row)

    act(() => host.querySelector<HTMLElement>('[data-workspace-id="empty"]')!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Delete folder'))!.click())
    const empty = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Delete workspace Empty"]')!
    expect(empty.textContent).toContain('The workspace “Empty” will be permanently deleted.')
    expect(empty.textContent).not.toContain('0 chats')
    act(() => Array.from(empty.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')!.click())
    expect(document.activeElement).toBe(host.querySelector('[data-workspace-id="empty"]'))
    act(() => root.unmount())
    host.remove()
  })

  it('keeps folder rename and delete failures visible', async () => {
    const renameFolder = vi.fn(async () => false)
    const deleteFolder = vi.fn(async () => false)
    useAppStore.setState({ renameFolder, deleteFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
    const rename = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Rename folder'))!
    act(() => rename.click())
    const directory = host.querySelector('input[placeholder="Workspace directory"]') as HTMLInputElement
    act(() => {
      directory.value = '/tmp/other'
      directory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save')!.click())
    expect(host.textContent).toContain('workspace could not be updated')
    expect(host.querySelector('input[placeholder="Workspace directory"]')).toBeTruthy()

    act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
    const remove = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Delete folder'))!
    act(() => remove.click())
    await act(async () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Delete workspace')!.click())
    expect(document.body.querySelector('[role="alertdialog"][aria-label^="Delete workspace "]')?.textContent).toContain('workspace could not be deleted')

    act(() => root.unmount())
    host.remove()
  })

	it('creates a workspace with the selected execution computer', async () => {
	  const addFolder = vi.fn(async () => true)
	  useAppStore.setState({
		addFolder,
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'New workspace')!.click())
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Workspace computer"]')!.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((button) => button.textContent?.includes('Homelab'))!.click())
	  const name = host.querySelector<HTMLInputElement>('input[placeholder="Folder name"]')!
	  const directory = host.querySelector<HTMLInputElement>('input[placeholder="Absolute directory on selected computer"]')!
	  const setInputValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
	  act(() => {
		setInputValue?.call(name, 'Containers')
		name.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  act(() => {
		setInputValue?.call(directory, '/opt/containers')
		directory.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  const create = host.querySelector<HTMLButtonElement>('button[aria-label="Create workspace"]')!
	  expect(create.disabled).toBe(false)
	  await act(async () => create.click())

	  expect(addFolder).toHaveBeenCalledWith('Containers', null, '/opt/containers', 'lab')
	  act(() => root.unmount())
	  host.remove()
	})

	it('shows an existing workspace computer with the same read-only field styling', () => {
	  useAppStore.setState({
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
		folders: [{ id: 'project', name: 'Containers', parentId: null, directory: '/opt/containers', executionHostId: 'lab', isExpanded: true, createdAt: new Date() }],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Rename folder'))!.click())
	  const computer = host.querySelector<HTMLInputElement>('input[aria-label="Workspace computer"]')!
	  const name = host.querySelector<HTMLInputElement>('input[aria-label="Workspace name"]')!
	  const directory = host.querySelector<HTMLInputElement>('input[aria-label="Workspace directory"]')!
	  expect(computer.value).toBe('Homelab')
	  expect(computer.readOnly).toBe(true)
	  expect(computer.className).toBe(name.className)
	  expect(computer.style.background).toBe(name.style.background)
	  expect(computer.style.border).toBe(directory.style.border)

	  act(() => root.unmount())
	  host.remove()
	})

	it('keeps Browse enabled for an installed helper in error state', () => {
	  useAppStore.setState({
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'error', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))
	  act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'New workspace')!.click())
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Workspace computer"]')!.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((button) => button.textContent?.includes('Homelab'))!.click())
	  const browse = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Browse')
	  expect(browse?.disabled).toBe(false)
	  act(() => root.unmount())
	  host.remove()
	})

	it('browses remote directories only after an explicit Browse action', async () => {
	  const listRemoteDirectories = vi.fn(async (_hostId: string, directory: string) => ({
		ok: true as const,
		listing: {
		  directory,
		  parentDirectory: directory === '/' ? '' : '/',
		  directories: directory === '/' ? [{ name: 'srv', path: '/srv' }] : [],
		  truncated: false,
		},
	  }))
	  useAppStore.setState({
		listRemoteDirectories,
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'New workspace')!.click())
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Workspace computer"]')!.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((button) => button.textContent?.includes('Homelab'))!.click())
	  expect(listRemoteDirectories).not.toHaveBeenCalled()
	  await act(async () => {
		Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Browse')!.click()
		await Promise.resolve()
		await Promise.resolve()
	  })
	  expect(listRemoteDirectories).toHaveBeenCalledWith('lab', '/')
	  await act(async () => {
		Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'srv')!.click()
		await Promise.resolve()
		await Promise.resolve()
	  })
	  expect(listRemoteDirectories).toHaveBeenLastCalledWith('lab', '/srv')
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Use this directory')!.click())
	  expect(host.querySelector<HTMLInputElement>('input[placeholder="Absolute directory on selected computer"]')!.value).toBe('/srv')

	  act(() => root.unmount())
	  host.remove()
	})

  it('opens the shared new-chat flow from the folder context menu', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-row-project"]')?.firstElementChild
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const newChat = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('New chat'))
    act(() => newChat?.click())

    expect(useAppStore.getState()).toMatchObject({ isNewChatModalOpen: true, newChatFolderId: 'project' })

    act(() => root.unmount())
    host.remove()
  })

  it('opens native history review from the folder context menu', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => {
      host.querySelector('[data-testid="folder-header-project"]')
        ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }))
    })
    const rescan = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Review chats')
    )
    act(() => rescan?.click())

    expect(useAppStore.getState().rescanFolderChats).toHaveBeenCalledWith('project')

    act(() => root.unmount())
    host.remove()
  })

  it('offers the same native history review for a remote workspace', () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
        { id: 'gaming-ai', label: 'Windows workstation', transport: 'ssh', sshAlias: 'uam-windows-ai', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha-2', platform: 'windows', architecture: 'x86_64', lastSeenAt: '' },
      ],
      folders: [{ ...makeFolder(), directory: 'C:\\Users\\david\\project', executionHostId: 'gaming-ai' }],
      sessions: [{ ...makeSession(1), workspaceDirectory: 'C:\\Users\\david\\project', executionHostId: 'gaming-ai' }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector('[data-testid="folder-header-project"]')
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const rescan = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Review chats'))
    act(() => rescan?.click())

    expect(useAppStore.getState().rescanFolderChats).toHaveBeenCalledWith('project')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps a failed native history review visible instead of looking empty', async () => {
    useAppStore.setState({ rescanFolderChats: vi.fn(() => Promise.resolve(false)) })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector('[data-testid="folder-header-project"]')
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const rescan = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Review chats'))
    await act(async () => { rescan?.click(); await Promise.resolve() })

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not rescan chats for Project')
    const dismiss = host.querySelector<HTMLButtonElement>('[aria-label="Dismiss folder action error"]')
    expect(dismiss).not.toBeNull()
    act(() => dismiss?.click())
    expect(host.querySelector('[role="alert"]')).toBeNull()

    act(() => host.querySelector('[data-testid="folder-header-project"]')
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const retry = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Review chats'))
    await act(async () => { retry?.click(); await Promise.resolve() })
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not rescan chats for Project')


    act(() => root.unmount())
    host.remove()
  })

  it('collapses Unsorted visually but reveals matching chats while searching', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 2).map((session) => ({ ...session, folderId: null })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const toggle = host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Unsorted"]')
    const children = host.querySelector<HTMLElement>('#unsorted-chat-list')
    expect(toggle?.getAttribute('aria-expanded')).toBe('true')
    expect(toggle?.textContent).toContain('Unsorted')
    expect(toggle?.textContent).toContain('2')
    expect(children?.getAttribute('aria-hidden')).toBe('false')
    expect(host.textContent).toContain('Chat 1')
    expect(host.textContent).toContain('Chat 2')

    act(() => toggle?.click())
    expect(host.querySelector('button[aria-label="Expand Unsorted"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(children?.getAttribute('aria-hidden')).toBe('true')
    expect(children?.hasAttribute('inert')).toBe(true)

    act(() => root.render(<FolderTree searchQuery="Chat 1" />))
    expect(host.querySelector('button[aria-label="Collapse Unsorted"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(children?.getAttribute('aria-hidden')).toBe('false')
    expect(children?.hasAttribute('inert')).toBe(false)
    expect(host.textContent).toContain('Chat 1')

    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelector('button[aria-label="Expand Unsorted"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(children?.getAttribute('aria-hidden')).toBe('true')

    act(() => root.unmount())
    host.remove()
  })

  it('previews and applies workspace recovery from the Unsorted context menu', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    const previewUnsortedWorkspaceFolders = vi.fn(async () => ({
      groups: [
        { title: 'Alpha', directory: '/tmp/Alpha', existingFolderId: '', chatIds: ['chat-1', 'chat-2'] },
        { title: 'Project', directory: '/tmp/project', existingFolderId: 'project', chatIds: ['chat-3'] },
      ],
      missing: [{ id: 'chat-4', title: 'Chat 4', directory: '/missing/workspace', reason: 'Folder not found' }],
      unavailable: [],
      noLocation: [{ id: 'chat-5', title: 'Chat 5', directory: '', reason: 'No workspace location recorded' }],
    }))
    const rebuildUnsortedWorkspaceFolders = vi.fn(async () => true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 5).map((session) => ({ ...session, folderId: null })),
      previewUnsortedWorkspaceFolders,
      rebuildUnsortedWorkspaceFolders,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector('button[aria-label="Collapse Unsorted"]')?.parentElement
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 40, clientY: 50 })))
    const menu = document.body.querySelector('[role="menu"][aria-label="Unsorted actions"]')
    expect(menu?.textContent).toContain('Rebuild workspace folders')
    const rebuild = menu?.querySelector('button') as HTMLButtonElement
    await act(async () => { rebuild.click(); await Promise.resolve(); await Promise.resolve() })

    expect(previewUnsortedWorkspaceFolders).toHaveBeenCalledTimes(1)
    const dialog = document.body.querySelector('[role="dialog"][aria-label="Rebuild workspace folders"]') as HTMLElement
    expect(dialog).toBeTruthy()
    expect(dialog.contains(document.activeElement)).toBe(true)
    expect(dialog.textContent).toContain('Ready · 3 chats')
    expect(dialog.textContent).toContain('Alpha')
    expect(dialog.textContent).toContain('Create · 2')
    expect(dialog.textContent).toContain('Use existing · 1')
    expect(dialog.textContent).toContain('Location not found · 1')
    expect(dialog.textContent).toContain('No saved location · 1')
    expect(rebuildUnsortedWorkspaceFolders).not.toHaveBeenCalled()

    const apply = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent?.includes('Create 1 folder and organise 3 chats'))
    await act(async () => { apply?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(rebuildUnsortedWorkspaceFolders).toHaveBeenCalledTimes(1)
    expect(document.body.querySelector('[aria-label="Rebuild workspace folders"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('keeps unavailable chats by default and reuses bulk confirmation before deletion', async () => {
    const deleteSessions = vi.fn(async () => true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 3).map((session) => ({ ...session, folderId: null })),
      deleteSessions,
      previewUnsortedWorkspaceFolders: vi.fn(async () => ({
        groups: [],
        missing: [{ id: 'chat-1', title: 'Chat 1', directory: '/missing/one', reason: 'Folder not found' }],
        unavailable: [{ id: 'chat-2', title: 'Chat 2', directory: '/blocked/two', reason: 'Permission denied' }],
        noLocation: [{ id: 'chat-3', title: 'Chat 3', directory: '', reason: 'No workspace location recorded' }],
      })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Actions for Unsorted"]')?.click())
    const recoveryAction = document.body.querySelector<HTMLButtonElement>('[role="menu"][aria-label="Unsorted actions"] button')
    await act(async () => { recoveryAction?.click(); await Promise.resolve(); await Promise.resolve() })
    const dialog = document.body.querySelector('[role="dialog"][aria-label="Rebuild workspace folders"]') as HTMLElement
    expect(dialog.textContent).toContain('stay in Unsorted unless you explicitly delete')
    expect(deleteSessions).not.toHaveBeenCalled()

    const deleteUnavailable = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent?.includes('Delete 2 unavailable'))
    act(() => deleteUnavailable?.click())
    expect(deleteSessions).not.toHaveBeenCalled()
    const confirmation = document.body.querySelector('[role="alertdialog"][aria-label="Delete 2 selected chats"]') as HTMLElement
    expect(confirmation.textContent).toContain('This cannot be undone')
    const confirm = Array.from(confirmation.querySelectorAll('button')).find((button) => button.textContent === 'Delete chats')
    await act(async () => confirm?.click())
    expect(deleteSessions).toHaveBeenCalledWith(['chat-1', 'chat-2'])

    act(() => root.unmount())
    host.remove()
  })

  it.each(['c', 'd'])('right-click deletion respects workspace selection for target %s', async (target) => {
    const deleteFolder = vi.fn(originalDeleteFolder)
    const deleteFolders = vi.fn(originalDeleteFolders)
    useAppStore.setState({
      folders: ['a', 'b', 'c', 'd'].map((id) => ({ ...makeFolder(), id, name: id })),
      sessions: ['a', 'b', 'c', 'd'].map((id, index) => ({ ...makeSession(index), folderId: id })),
      deleteFolder,
      deleteFolders,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-a"]')!.click())
      act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-c"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
      act(() => host.querySelector<HTMLElement>(`[data-testid="folder-header-${target}"]`)!.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
      const selectedTarget = target === 'c'
      const menuLabel = selectedTarget ? 'Delete 3 workspaces…' : 'Delete folder'
      const menuItem = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menu"] button')).find((button) => button.textContent === menuLabel)!
      expect(menuItem).toBeTruthy()
      act(() => menuItem.click())
      expect(deleteFolders).not.toHaveBeenCalled()
      expect(deleteFolder).not.toHaveBeenCalled()
      const dialog = document.body.querySelector(selectedTarget ? '[role="alertdialog"][aria-label="Delete 3 selected workspaces"]' : '[role="alertdialog"][aria-label="Delete workspace d and 1 chat"]')!
      expect(dialog.textContent).toContain(selectedTarget ? '3 workspaces and 3 chats' : 'The workspace “d” and 1 chat')
      const confirm = Array.from(dialog.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === (selectedTarget ? 'Delete workspaces' : 'Delete workspace'))!
      await act(async () => confirm.click())
      if (selectedTarget) {
        expect(deleteFolders).toHaveBeenCalledExactlyOnceWith(['a', 'b', 'c'])
        expect(deleteFolder).not.toHaveBeenCalled()
        expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['d'])
        expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['chat-3'])
      } else {
        expect(deleteFolder).toHaveBeenCalledExactlyOnceWith('d')
        expect(deleteFolders).not.toHaveBeenCalled()
        expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['a', 'b', 'c'])
        expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['chat-0', 'chat-1', 'chat-2'])
      }
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('selects workspace ranges, cancels, reports failures, and submits one batch', async () => {
    const deleteFolders = vi.fn(async () => { useAppStore.setState({ workspaceDeletionError: 'Remote stop could not be confirmed.' }); return false })
    useAppStore.setState({
      folders: ['a', 'b', 'c'].map((id) => ({ ...makeFolder(), id, name: id })),
      sessions: ['a', 'b', 'c'].map((id, index) => ({ ...makeSession(index), folderId: id })),
      deleteFolders,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-a"]')!.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-c"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]')).toHaveLength(3)
    // Selected workspace headers share the Settings selected-item style with selected chats.
    expect(Array.from(host.querySelectorAll<HTMLElement>('[data-workspace-id][aria-label$="selected for bulk actions"]')).every((row) => row.classList.contains('uam-selected-item') && row.style.background === '')).toBe(true)
    const openDelete = () => host.querySelector<HTMLButtonElement>('[aria-label="Delete 3 selected workspaces"]')!.click()
    const dialogButton = (label: string) => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')).find((button) => button.textContent === label)!
    act(openDelete)
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('3 workspaces and 3 chats')
    act(() => dialogButton('Cancel').click())
    expect(deleteFolders).not.toHaveBeenCalled()
    act(openDelete)
    await act(async () => dialogButton('Delete workspaces').click())
    expect(deleteFolders).toHaveBeenCalledWith(['a', 'b', 'c'])
    expect(document.body.querySelector('[role="alert"]')?.textContent).toBe('Remote stop could not be confirmed.')
    expect(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]')).toHaveLength(3)
    deleteFolders.mockResolvedValue(true)
    await act(async () => dialogButton('Delete workspaces').click())
    expect(deleteFolders).toHaveBeenCalledTimes(2)
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    expect(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]')).toHaveLength(0)
    act(() => root.unmount())
    host.remove()
  })

  it('prunes workspace selection when a collection hides its rows and supports keyboard selection', async () => {
    useAppStore.setState({
      folders: ['a', 'b', 'c'].map((id) => ({ ...makeFolder(), id, name: id })),
      resourceCollections: [{ id: 'group', title: 'Group', collapsed: false, references: [{ id: 'ref', type: 'workspace-folder', target: 'b', label: 'b' }] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-b"]')!.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-c"]')!.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, key: 'Enter', shiftKey: true })))
    expect(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]')).toHaveLength(3)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Delete 3 selected workspaces"]')!.click())
    await act(async () => {
      useAppStore.setState({ resourceCollections: [{ ...useAppStore.getState().resourceCollections[0], collapsed: true }] })
      await Promise.resolve()
    })
    expect(host.querySelector('[data-workspace-id="b"]')?.getAttribute('aria-label')).toBe('b')
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('2 workspaces')
    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(host.querySelectorAll('[data-workspace-id][aria-label$="selected for bulk actions"]')).toHaveLength(0)
    act(() => root.unmount())
    host.remove()
  })

  it('shows determinate batch delete progress when the backend reports it and indeterminate otherwise', async () => {
    let finish!: (deleted: boolean) => void
    useAppStore.setState({ deleteFolders: vi.fn(() => new Promise<boolean>((resolve) => { finish = resolve })) })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Delete 1 selected workspace"]')!.click())
    const confirm = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')).find((button) => button.textContent === 'Delete workspace')!
    await act(async () => { confirm.click(); await Promise.resolve() })
    expect(document.body.querySelector('[role="alertdialog"] [role="progressbar"]')?.hasAttribute('aria-valuenow')).toBe(false)
    await act(async () => {
      useAppStore.setState({ deletionProgress: { completed: 3, total: 12, currentLabel: 'alpha' } })
      await Promise.resolve()
    })
    const bar = document.body.querySelector<HTMLElement>('[role="alertdialog"] [role="progressbar"]')!
    expect(bar.getAttribute('aria-valuenow')).toBe('3')
    expect(bar.getAttribute('aria-valuemax')).toBe('12')
    expect(bar.style.getPropertyValue('--progress')).toBe('0.25')
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('3 of 12 removed')
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('alpha')
    await act(async () => finish(true))
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    useAppStore.setState({ deletionProgress: undefined })
    act(() => root.unmount())
    host.remove()
  })

  it('prevents duplicate workspace delete requests and dismissal while a batch is pending', async () => {
    let finish!: (deleted: boolean) => void
    const deleteFolders = vi.fn(() => new Promise<boolean>((resolve) => { finish = resolve }))
    useAppStore.setState({ deleteFolders })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Delete 1 selected workspace"]')!.click())
    const confirm = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')).find((button) => button.textContent === 'Delete workspace')!
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"]')!
    confirm.focus()
    await act(async () => { confirm.click(); await Promise.resolve() })
    expect(confirm.disabled).toBe(true)
    expect(document.activeElement).toBe(dialog)
    expect(document.body.querySelector('[role="alertdialog"] [role="status"]')?.textContent).toBe('Deleting 1 workspace and 7 chats…')
    const tab = new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true })
    act(() => { confirm.click(); document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); document.dispatchEvent(tab) })
    expect(tab.defaultPrevented).toBe(true)
    expect(document.activeElement).toBe(dialog)
    expect(deleteFolders).toHaveBeenCalledTimes(1)
    expect(document.body.querySelector('[role="alertdialog"]')).toBeTruthy()
    await act(async () => finish(true))
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('selects a visible Shift-click range and confirms one bulk delete', async () => {
    const deleteSessions = vi.fn(async () => true)
    useAppStore.setState({ deleteSessions })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')?.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-4"]')
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))

    expect(Array.from(host.querySelectorAll('[data-testid^="session-row-"][data-selected="true"]'))
      .map((row) => row.getAttribute('data-session-id'))).toEqual(['chat-1', 'chat-2', 'chat-3', 'chat-4'])
    const bulkDelete = host.querySelector<HTMLButtonElement>('button[aria-label="Delete 4 selected chats"]')
    expect(bulkDelete).toBeTruthy()
    act(() => bulkDelete?.click())
    expect(deleteSessions).not.toHaveBeenCalled()

    const dialog = document.body.querySelector('[role="alertdialog"][aria-label="Delete 4 selected chats"]')
    expect(dialog?.textContent).toContain('This cannot be undone')
    const confirm = Array.from(dialog?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Delete chats')
    await act(async () => confirm?.click())

    expect(deleteSessions).toHaveBeenCalledTimes(1)
    expect(deleteSessions).toHaveBeenCalledWith(['chat-1', 'chat-2', 'chat-3', 'chat-4'])
    expect(host.querySelector('[aria-label="Delete 4 selected chats"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('clears selected chats when their workspace is collapsed', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')?.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-2"]')
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(host.textContent).toContain('2 chats selected')

    await act(async () => {
      host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')?.click()
      await Promise.resolve()
    })

    expect(host.textContent).not.toContain('selected')
    expect(host.querySelectorAll('[data-selected="true"]')).toHaveLength(0)

    act(() => root.unmount())
    host.remove()
  })

  it('shows a helpful empty search state', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="nothing-matches-this" />))

    expect(host.textContent).toContain('No chats match this search')
    expect(host.textContent).toContain('Try another term or clear the filters.')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the new-folder form visible in the scrolling sidebar', () => {
    const scrollIntoView = vi.fn()
    const previousScrollIntoView = HTMLElement.prototype.scrollIntoView
    HTMLElement.prototype.scrollIntoView = scrollIntoView
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const actions = Array.from(host.querySelectorAll('button')).filter((button) => ['New workspace', 'New collection'].includes(button.textContent ?? ''))
    expect(actions.map((button) => button.textContent)).toEqual(['New workspace', 'New collection'])
    expect(actions[0].parentElement?.className).toContain('justify-center')
    const newFolder = actions[0]
    act(() => newFolder?.click())

    expect(scrollIntoView).toHaveBeenCalledWith({ block: 'nearest' })

    act(() => root.unmount())
    host.remove()
    HTMLElement.prototype.scrollIntoView = previousScrollIntoView
  })

  it('uses right-aligned icon actions and clears cancelled workspace fields', async () => {
    const addFolder = vi.fn(async () => true)
    useAppStore.setState({ addFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    const openForm = () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'New workspace')?.click()
    act(openForm)
    const confirm = host.querySelector<HTMLButtonElement>('button[aria-label="Create workspace"]')!
    const cancel = host.querySelector<HTMLButtonElement>('button[aria-label="Cancel new workspace"]')!
    expect(confirm.textContent).toBe('')
    expect(confirm.disabled).toBe(true)
    expect(confirm.style.background).toBe('var(--accent)')
    expect(cancel.textContent).toBe('')
    expect(cancel.style.color).toBe('var(--error)')
    expect(cancel.parentElement?.className).toContain('uam-inline-form__actions')
    const setInputValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
    const setField = (placeholder: string, value: string) => {
      const input = host.querySelector<HTMLInputElement>(`input[placeholder="${placeholder}"]`)!
      act(() => {
        setInputValue?.call(input, value)
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
    }
    setField('Folder name', 'Cancelled')
    expect(host.querySelector<HTMLButtonElement>('button[aria-label="Create workspace"]')!.disabled).toBe(true)
    setField('Workspace directory', '/tmp/cancelled')
    act(() => cancel.click())
    expect(host.querySelector('input[placeholder="Folder name"]')).toBeNull()
    expect(addFolder).not.toHaveBeenCalled()
    act(openForm)
    expect(host.querySelector<HTMLInputElement>('input[placeholder="Folder name"]')!.value).toBe('')
    expect(host.querySelector<HTMLInputElement>('input[placeholder="Workspace directory"]')!.value).toBe('')
    setField('Folder name', 'Workspace')
    setField('Workspace directory', '/tmp/workspace')
    await act(async () => { host.querySelector<HTMLButtonElement>('button[aria-label="Create workspace"]')!.click(); await Promise.resolve() })
    expect(addFolder).toHaveBeenCalledWith('Workspace', null, '/tmp/workspace', 'local')
    act(() => root.unmount())
    host.remove()
  })

  it('uses labelled confirm and cancel actions when naming a collection', async () => {
    const createResourceCollection = vi.fn(async (name: string) => ({ id: 'ideas', name, collapsed: false, references: [] }))
    useAppStore.setState({ createResourceCollection })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const openForm = () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'New collection')?.click()
    act(openForm)

    const confirm = host.querySelector('button[aria-label="Create collection"]') as HTMLButtonElement
    const cancel = host.querySelector('button[aria-label="Cancel new collection"]') as HTMLButtonElement
    expect(confirm.textContent).toBe('Create')
    expect(confirm.disabled).toBe(true)
    expect(confirm.className).toContain('uam-btn--primary')
    expect(cancel.textContent).toBe('Cancel')

    act(() => cancel.click())
    expect(host.querySelector('input[aria-label="Collection name"]')).toBeNull()

    act(openForm)
    const input = host.querySelector('input[aria-label="Collection name"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Ideas')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const enabledConfirm = host.querySelector('button[aria-label="Create collection"]') as HTMLButtonElement
    expect(enabledConfirm.disabled).toBe(false)
    await act(async () => { enabledConfirm.click(); await Promise.resolve() })
    expect(createResourceCollection).toHaveBeenCalledWith('Ideas')

    act(() => root.unmount())
    host.remove()
  })
})
