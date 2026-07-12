import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

vi.mock('./Sidebar', () => ({
  Sidebar: () => <div data-testid="sidebar">Sidebar</div>,
}))

vi.mock('./MainPanel', () => ({
  MainPanel: () => <div data-testid="main-panel">Main panel</div>,
}))

import { AppShell } from './AppShell'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('AppShell', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      sessions: [],
      activeSessionId: null,
      isNewChatModalOpen: false,
      isSettingsOpen: false,
      memoryLibraryScope: null,
      isMemoryScanModalOpen: false,
      isMarkdownStoreOpen: false,
      sidebarCollapsed: false,
      commitPanelOpen: false,
      sidebarWidthPx: 320,
      commitPanelWidthPx: 420,
    })
  })

  it('renders edge-to-edge with side rails and without a total header bar', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    expect(host.querySelector('.uam-app')).toBeTruthy()
    expect(host.querySelector('.uam-titlebar')).toBeNull()
    expect(host.querySelector('[aria-label="Main navigation"]')).toBeTruthy()
    expect(host.querySelector('[aria-label="Tool windows"]')).toBeTruthy()
    expect(host.querySelector('.uam-window')).toBeNull()
    expect(host.querySelector('.uam-window-controls')).toBeNull()
    expect(host.querySelector('.uam-window-dot')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the rail actions wired', () => {
    const openAllMemoryLibrary = vi.fn().mockResolvedValue(true)
    const setSettingsOpen = vi.fn()
    useAppStore.setState({
      openAllMemoryLibrary,
      setSettingsOpen,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const memoryButton = host.querySelector('button[aria-label="Memory library"]') as HTMLButtonElement
    const settingsButton = host.querySelector('button[aria-label="Settings"]') as HTMLButtonElement

    act(() => {
      memoryButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      settingsButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openAllMemoryLibrary).toHaveBeenCalledTimes(1)
    expect(setSettingsOpen).toHaveBeenCalledWith(true)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === '+New Chat')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('clamps sidebar and commit panel widths independently', () => {
    useAppStore.setState({
      sidebarWidthPx: 320,
      commitPanelWidthPx: 420,
      commitPanelOpen: true,
    })

    act(() => {
      useAppStore.getState().setSidebarWidthPx(900)
    })

    expect(useAppStore.getState().sidebarWidthPx).toBe(520)
    expect(useAppStore.getState().commitPanelWidthPx).toBe(420)

    act(() => {
      useAppStore.getState().setCommitPanelWidthPx(100)
    })

    expect(useAppStore.getState().sidebarWidthPx).toBe(520)
    expect(useAppStore.getState().commitPanelWidthPx).toBe(320)
  })

  it('collapses and expands the sidebar without changing the active chat', () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-2',
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const collapseButton = host.querySelector('button[aria-label="Collapse chat selector"]') as HTMLButtonElement
    act(() => {
      collapseButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(host.querySelector('[data-testid="sidebar"]')).toBeNull()

    const expandButton = host.querySelector('button[aria-label="Expand chat selector"]') as HTMLButtonElement
    act(() => {
      expandButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(host.querySelector('[data-testid="sidebar"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('opens and closes the commit panel without changing the active chat', async () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: false,
        vcsTypes: [],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: '',
        changedFiles: [],
        warning: 'No Git or SVN repository detected for this workspace.',
        error: '',
      }),
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
    expect(host.textContent).toContain('Commit')
    expect(host.textContent).toContain('No Git or SVN repository detected for this workspace.')

    const closeButton = host.querySelector('button[aria-label="Close commit panel"]') as HTMLButtonElement
    act(() => {
      closeButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches the VCS panel to the newly focused chat', async () => {
    const getVcsCommitStatus = vi.fn(async (chatId: string) => ({
      available: true,
      vcsTypes: ['git' as const],
      activeVcsType: 'git' as const,
      workspaceDirectory: `/tmp/${chatId}`,
      branchOrRevision: chatId,
      changedFiles: [],
      lineStatsReady: true,
      warning: '',
      error: '',
    }))
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/chat-1', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/chat-2', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      commitPanelOpen: true,
      getVcsCommitStatus,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<AppShell />)
      await Promise.resolve()
    })
    expect(host.textContent).toContain('/tmp/chat-1')

    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      await Promise.resolve()
    })
    expect(host.textContent).toContain('/tmp/chat-2')
    expect(getVcsCommitStatus).toHaveBeenCalledWith('chat-2', 'git', expect.any(Object))

    act(() => root.unmount())
    host.remove()
  })

  it('renders commit files as checklist rows with line stats and no diff view', async () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: true,
        vcsTypes: ['git'],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: 'main',
        changedFiles: [
          { path: 'src/app.ts', status: ' M', staged: false, additions: 12, deletions: 3, binary: false },
          { path: 'assets/logo.png', status: '??', staged: false, additions: 0, deletions: 0, binary: true },
        ],
        warning: '',
        error: '',
      }),
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(host.textContent).toContain('2 changed files')
    expect(host.textContent).toContain('src/app.ts')
    expect(host.textContent).toContain('+12')
    expect(host.textContent).toContain('-3')
    expect(host.textContent).toContain('assets/logo.png')
    expect(host.textContent).toContain('BIN')
    expect(host.textContent).not.toContain('Diff')
    expect(host.textContent).not.toContain('No diff available')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('generates and commits title and description for selected files', async () => {
    const generateVcsCommitMessage = vi.fn().mockResolvedValue({
      title: 'Update commit panel',
      description: '- Refresh checklist file controls',
    })
    const commitVcsChanges = vi.fn().mockResolvedValue({
      ok: true,
      message: 'Git commit created.',
      error: '',
    })
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: true,
        vcsTypes: ['git'],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: 'main',
        changedFiles: [
          { path: 'src/app.ts', status: ' M', staged: false, additions: 12, deletions: 3, binary: false },
        ],
        warning: '',
        error: '',
      }),
      generateVcsCommitMessage,
      commitVcsChanges,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    const fileCheckbox = Array.from(host.querySelectorAll('input[type="checkbox"]')).find((input) =>
      input.getAttribute('aria-label') !== 'Select all changed files'
    ) as HTMLInputElement
    await act(async () => {
      fileCheckbox.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    const aiButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'AI') as HTMLButtonElement
    await act(async () => {
      aiButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(generateVcsCommitMessage).toHaveBeenCalledWith('chat-1', 'git', ['src/app.ts'])
    expect((host.querySelector('input[placeholder="Summary"]') as HTMLInputElement).value).toBe('Update commit panel')
    expect((host.querySelector('textarea[placeholder="Description"]') as HTMLTextAreaElement).value).toBe('- Refresh checklist file controls')

    const commitButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Commit selected files') as HTMLButtonElement
    await act(async () => {
      commitButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(commitVcsChanges).toHaveBeenCalledWith('chat-1', 'git', 'Update commit panel\n\n- Refresh checklist file controls', ['src/app.ts'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
