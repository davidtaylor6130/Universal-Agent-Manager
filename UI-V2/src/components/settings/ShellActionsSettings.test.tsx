import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { ShellActionsSettings, type ShellActionsHandle } from './ShellActionsSettings'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('ShellActionsSettings', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      shellActions: [{
        id: 'review-selection',
        label: 'Review Selection',
        skillPath: '/tmp/Skills/Review ü.uam',
        providerId: 'codex-cli',
        modelId: 'gpt-5.4',
        groupPath: ['GitHub', 'Review'],
        acceptsFiles: true,
        acceptsFolders: true,
        enabled: true,
        openWorkspace: false,
      }],
      shellActionNotification: '',
      defaultNewChatProviderId: 'gemini-cli',
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#4285f4', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#10a37f', description: '' },
      ],
      markdownStoreEntries: [{
        id: 'review',
        title: 'Review skill',
        maker: '',
        review: '',
        dateCreated: '',
        dateUpdated: '',
        preview: '',
        filePath: '/tmp/Skills/Review ü.uam',
      }],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      setShellActions: vi.fn(() => Promise.resolve(true)),
      applyShellActions: vi.fn(() => Promise.resolve(true)),
    })
  })

  it('edits, disables, removes, and applies configurable actions', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<ShellActionsSettings />) })

    expect(host.textContent).not.toContain('Finder / Explorer actions')
    expect(host.textContent).toContain('Review skill')
    const label = host.querySelector('input[aria-label="Label for Review Selection"]') as HTMLInputElement
    const groupPath = host.querySelector('button[aria-label="Group for Review Selection"]') as HTMLInputElement
    const enabled = Array.from(host.querySelectorAll('input[type="checkbox"]'))[0] as HTMLInputElement
    const inputType = host.querySelector('button[aria-label="Input type for Review Selection"]') as HTMLButtonElement
    const provider = host.querySelector('button[aria-label="Provider for Review Selection"]') as HTMLButtonElement
    expect(host.querySelector('select')).toBeNull()
    expect(inputType.querySelector('[data-menu-select-icon]')).not.toBeNull()

    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(label, 'Review résumé')
      label.dispatchEvent(new Event('input', { bubbles: true }))
      enabled.click()
      groupPath.click()
    })
    const existingGroup = Array.from(document.body.querySelectorAll('[role="option"]')).find(option => option.textContent === 'GitHub') as HTMLButtonElement
    await act(async () => { existingGroup.click(); inputType.click() })
    const filesOnly = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Files only')) as HTMLButtonElement
    await act(async () => { filesOnly.click() })

    await act(async () => { provider.click() })
    const gemini = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Gemini')) as HTMLButtonElement
    await act(async () => { gemini.click() })
    const model = host.querySelector('button[aria-label="Model for Review résumé"]') as HTMLButtonElement
    await act(async () => { model.click() })
    const flash = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('FlashPrioritize speed')) as HTMLButtonElement
    await act(async () => { flash.click() })

    const apply = host.querySelector<HTMLButtonElement>('button[aria-label="Save shell actions"]')
    await act(async () => { apply?.click() })

    expect(useAppStore.getState().setShellActions).toHaveBeenCalledWith([
      expect.objectContaining({
        label: 'Review résumé',
        providerId: 'gemini-cli',
        modelId: 'flash',
        groupPath: ['GitHub'],
        enabled: false,
        acceptsFiles: true,
        acceptsFolders: false,
      }),
    ])
    expect(useAppStore.getState().applyShellActions).toHaveBeenCalledTimes(1)

    const remove = host.querySelector('button[aria-label="Remove Review résumé"]') as HTMLButtonElement
    act(() => remove.click())
    expect(host.textContent).toContain('Delete “Review résumé”?')
    const confirmDelete = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Delete action')
    act(() => confirmDelete?.click())
    expect(host.textContent).not.toContain('Review résumé')

    act(() => root.unmount())
    host.remove()
  })

  it('applies shell actions only once before the applying state rerenders', async () => {
    const finishes: Array<(ok: boolean) => void> = []
    const save = vi.fn(() => new Promise<boolean>((resolve) => finishes.push(resolve)))
    useAppStore.setState({ setShellActions: save })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<ShellActionsSettings />) })
    act(() => (host.querySelector('button[aria-label="Add shell action"]') as HTMLButtonElement).click())
    const apply = host.querySelector<HTMLButtonElement>('button[aria-label="Save shell actions"]') as HTMLButtonElement

    act(() => {
      apply.click()
      apply.click()
    })

    expect(save).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish(false))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })
  it('keeps the exit guard and draft when OS application fails, then leaves after a successful retry', async () => {
    const ref = createRef<ShellActionsHandle>()
    const leave = vi.fn()
    const apply = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    useAppStore.setState({applyShellActions:apply})
    const host=document.createElement('div'); document.body.append(host)
    const root=createRoot(host)
    await act(async () => {root.render(<ShellActionsSettings ref={ref}/> )})
    act(() => (host.querySelector('[aria-label="Add shell action"]') as HTMLButtonElement).click())
    act(() => ref.current?.requestLeave(leave))
    const save = () => Array.from(host.querySelectorAll('button')).find(button => button.textContent==='Save and leave')!
    await act(async () => {save().click()})
    expect(leave).not.toHaveBeenCalled()
    expect(host.querySelector('[aria-label="Unsaved shell actions"]')).not.toBeNull()
    expect(host.textContent).toContain('New action')
    await act(async () => {save().click()})
    expect(leave).toHaveBeenCalledOnce()
    expect(apply).toHaveBeenCalledTimes(2)
    act(() => root.unmount()); host.remove()
  })

})
