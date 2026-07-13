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
        acceptsFiles: true,
        acceptsFolders: true,
        enabled: true,
        openWorkspace: false,
      }],
      shellActionNotification: '',
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
    const enabled = Array.from(host.querySelectorAll('input[type="checkbox"]'))[0] as HTMLInputElement
    const inputType = host.querySelector('select[aria-label="Input type for Review Selection"]') as HTMLSelectElement

    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(label, 'Review résumé')
      label.dispatchEvent(new Event('input', { bubbles: true }))
      enabled.click()
      inputType.value = 'files'
      inputType.dispatchEvent(new Event('change', { bubbles: true }))
    })

    const apply = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Apply')
    await act(async () => { apply?.click() })

    expect(useAppStore.getState().setShellActions).toHaveBeenCalledWith([
      expect.objectContaining({ label: 'Review résumé', enabled: false, acceptsFiles: true, acceptsFolders: false }),
    ])
    expect(useAppStore.getState().applyShellActions).toHaveBeenCalledTimes(1)

    const remove = host.querySelector('button[aria-label="Remove Review résumé"]') as HTMLButtonElement
    act(() => remove.click())
    expect(host.textContent).not.toContain('Review résumé')

    act(() => root.unmount())
    host.remove()
  })
})
