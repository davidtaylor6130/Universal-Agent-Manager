import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { Folder, Session } from '../../types/session'
import { FolderTree } from './FolderTree'
import { defaultChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const now = new Date('2026-01-01T12:00:00.000Z')

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
      isNewChatModalOpen: false,
      newChatFolderId: null,
      openFolderMemoryLibrary: vi.fn(() => Promise.resolve(true)),
    })
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
    expect(host.textContent).toContain('See more')
    expect(host.textContent).toContain('+2')
    const activeTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 1')
    expect(activeTitle?.getAttribute('style')).toContain('var(--text)')
    expect(activeTitle?.getAttribute('style')).not.toContain('#ffffff')

    const seeMoreButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('See more')
    )
    expect(seeMoreButton).toBeTruthy()

    act(() => {
      seeMoreButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Chat 6')
    expect(host.textContent).toContain('Chat 7')
    expect(host.textContent).toContain('Show less')

    act(() => {
      root.unmount()
    })
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

  it('colours a collapsed folder from its focused hidden chat pane', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, isExpanded: false })) }))
    writeChatGridLayout({ ...defaultChatGridLayout, paneCount: 2, activePane: 1, sessionIds: ['', 'chat-3'] })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.textContent).not.toContain('Chat 3')
    expect((host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement).style.color).toBe('rgb(59, 130, 246)')

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

    const pinButton = host.querySelector<HTMLButtonElement>('button[aria-label="Pin chat"]')
    expect(pinButton).toBeTruthy()
    expect(pinButton?.className).toContain('opacity-0')
    expect(pinButton?.className).toContain('group-hover:opacity-100')
    expect(pinButton?.className).toContain('group-focus-within:opacity-100')

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

  it('opens project memory from the folder action', () => {
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

    const memoryButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Project memory')
    )
    expect(memoryButton).toBeTruthy()

    act(() => {
      memoryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().openFolderMemoryLibrary).toHaveBeenCalledWith('project')

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
