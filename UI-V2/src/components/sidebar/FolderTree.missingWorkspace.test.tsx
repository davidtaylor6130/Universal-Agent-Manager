import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type CppAppState } from '../../store/useAppStore'
import { FolderTree } from './FolderTree'

vi.mock('../../ipc/cefBridge', async (original) => ({
  ...await original<typeof import('../../ipc/cefBridge')>(),
  isCefContext: () => true,
  sendToCEF: vi.fn(async () => ({ ok: false, error: 'No request transport in push fixture' })),
}))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const initial = useAppStore.getState()

// Native sends a chat's saved folder ID unchanged, even when no saved workspace has it.
function pushState(chats: Array<{ id: string; title: string; folderId: string }>, folders: Array<{ id: string; title: string }>) {
  window.uamPush?.({
    type: 'stateUpdate',
    data: {
      stateRevision: 1, selectedChatId: '', folders: folders.map((folder) => ({ ...folder, directory: `/tmp/${folder.id}`, collapsed: false })),
      resourceCollections: [], providers: [], settings: {}, statusLine: '',
      chats: chats.map((chat) => ({ ...chat, providerId: 'codex-cli', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' })),
    } as unknown as CppAppState,
  })
}

describe('chats whose workspace no longer exists', () => {
  afterEach(() => useAppStore.setState(initial, true))

  it('lists them under Unsorted with workspace recovery, and leaves their saved folder ID alone', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    const previewUnsortedWorkspaceFolders = vi.fn(async () => ({ groups: [], missing: [], unavailable: [], noLocation: [] }))
    useAppStore.setState({ lastAppliedStateRevision: -1, previewUnsortedWorkspaceFolders, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
    // No chat has an empty folder ID: every unsorted chat here points at a workspace that is gone.
    pushState([
      { id: 'kept', title: 'Kept chat', folderId: 'project' },
      { id: 'lost-1', title: 'Lost chat one', folderId: 'gone-a' },
      { id: 'lost-2', title: 'Lost chat two', folderId: 'gone-b' },
    ], [{ id: 'project', title: 'Project' }])
    expect(useAppStore.getState().sessions.map((session) => [session.id, session.folderId])).toEqual([
      ['kept', 'project'], ['lost-1', 'gone-a'], ['lost-2', 'gone-b'],
    ])

    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      const toggle = host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Unsorted"]')
      expect(toggle?.textContent).toContain('2')
      const unsorted = host.querySelector<HTMLElement>('#unsorted-chat-list')!
      expect(unsorted.textContent).toContain('Lost chat one')
      expect(unsorted.textContent).toContain('Lost chat two')
      expect(unsorted.textContent).not.toContain('Kept chat')
      expect(host.querySelector('[data-testid="folder-sessions-project"]')?.textContent).toContain('Kept chat')

      act(() => root.render(<FolderTree searchQuery="lost" />))
      expect(host.querySelector('#unsorted-chat-list')?.textContent).toContain('Lost chat two')

      act(() => root.render(<FolderTree searchQuery="" />))
      act(() => host.querySelector('button[aria-label="Collapse Unsorted"]')?.parentElement
        ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 40, clientY: 50 })))
      const menu = document.body.querySelector('[role="menu"][aria-label="Unsorted actions"]')
      expect(menu?.textContent).toContain('Rebuild workspace folders')
      await act(async () => { menu?.querySelector('button')?.click(); await Promise.resolve(); await Promise.resolve() })
      expect(previewUnsortedWorkspaceFolders).toHaveBeenCalledTimes(1)
      expect(useAppStore.getState().sessions.map((session) => session.folderId)).toEqual(['project', 'gone-a', 'gone-b'])
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.unstubAllGlobals()
    }
  })

  it('shows All chats and Unsorted when every saved workspace is gone', () => {
    useAppStore.setState({ lastAppliedStateRevision: -1, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
    pushState(Array.from({ length: 30 }, (_, index) => ({ id: `lost-${index}`, title: `Lost ${index}`, folderId: 'gone' })), [])
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<FolderTree searchQuery="" />))
      expect(host.querySelector('[data-testid="all-chats"]')).toBeTruthy()
      expect(host.querySelector('button[aria-label="Collapse Unsorted"]')?.textContent).toContain('30')
      expect(host.querySelector('#unsorted-chat-list')?.textContent).toContain('Lost 29')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
