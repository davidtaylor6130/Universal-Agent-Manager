import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { ShellActionsSettings } from './ShellActionsSettings'

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

    expect(host.textContent).toContain('Finder / Explorer actions')
    expect(host.textContent).toContain('Review skill')
    const label = host.querySelector('input[aria-label="Label for Review Selection"]') as HTMLInputElement
    const groupPath = host.querySelector('input[aria-label="Group path for Review Selection"]') as HTMLInputElement
    const enabled = Array.from(host.querySelectorAll('input[type="checkbox"]'))[0] as HTMLInputElement
    const inputType = host.querySelector('button[aria-label="Input type for Review Selection"]') as HTMLButtonElement
    const provider = host.querySelector('button[aria-label="Provider for Review Selection"]') as HTMLButtonElement
    expect(host.querySelector('select')).toBeNull()
    expect(inputType.querySelector('[data-menu-select-icon]')).not.toBeNull()

    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(label, 'Review résumé')
      label.dispatchEvent(new Event('input', { bubbles: true }))
      valueSetter?.call(groupPath, 'GitHub / Pull requests')
      groupPath.dispatchEvent(new Event('input', { bubbles: true }))
      enabled.click()
      inputType.click()
    })
    const filesOnly = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Files only')) as HTMLButtonElement
    await act(async () => { filesOnly.click() })

    await act(async () => { provider.click() })
    const gemini = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Gemini')) as HTMLButtonElement
    await act(async () => { gemini.click() })
    const model = host.querySelector('button[aria-label="Model for Review résumé"]') as HTMLButtonElement
    await act(async () => { model.click() })
    const flash = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('FlashPrioritize speed')) as HTMLButtonElement
    await act(async () => { flash.click() })

    const apply = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Apply')
    await act(async () => { apply?.click() })

    expect(useAppStore.getState().setShellActions).toHaveBeenCalledWith([
      expect.objectContaining({
        label: 'Review résumé',
        providerId: 'gemini-cli',
        modelId: 'flash',
        groupPath: ['GitHub', 'Pull requests'],
        enabled: false,
        acceptsFiles: true,
        acceptsFolders: false,
      }),
    ])
    expect(useAppStore.getState().applyShellActions).toHaveBeenCalledTimes(1)

    vi.spyOn(window, 'confirm').mockReturnValue(true)
    const remove = host.querySelector('button[aria-label="Remove Review résumé"]') as HTMLButtonElement
    act(() => remove.click())
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
    const apply = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Apply') as HTMLButtonElement

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
})
