import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { NewChatModal } from './NewChatModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('NewChatModal', () => {
  beforeEach(() => {
    useAppStore.setState({
      folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {},
      newChatFolderId: 'project',
    })
  })

  it('creates a folder chat with the provider and model selected by the user', () => {
    const addSession = vi.fn()
    const setNewChatModalOpen = vi.fn()
    useAppStore.setState({ addSession, setNewChatModalOpen })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    act(() => {
      host.querySelector<HTMLButtonElement>('button[aria-label="Provider"]')?.click()
    })
    const codex = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Codex'))
    act(() => codex?.click())

    expect(host.querySelector('select[aria-label="Model"]')).toBeNull()
    const model = host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')!
    act(() => {
      model.click()
    })
    expect(model.getAttribute('aria-expanded')).toBe('true')
    const modelList = host.querySelector<HTMLElement>('[role="listbox"][aria-label="Model"]')!
    expect(modelList).toBeTruthy()

    act(() => {
      modelList.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    })
    expect(host.querySelector('[role="listbox"][aria-label="Model"]')).toBeNull()
    expect(setNewChatModalOpen).not.toHaveBeenCalled()

    act(() => model.click())
    expect(host.querySelector('[role="listbox"][aria-label="Model"]')).toBeTruthy()
    act(() => document.body.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(host.querySelector('[role="listbox"][aria-label="Model"]')).toBeNull()

    act(() => model.click())
    const gpt = Array.from(host.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) =>
      option.textContent?.includes('GPT-5.4')
    )
    act(() => gpt?.click())

    const create = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create chat')
    act(() => create?.click())

    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', 'gpt-5.4')

    act(() => root.unmount())
    host.remove()
  })
})
