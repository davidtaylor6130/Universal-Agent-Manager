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
    expect(host.querySelector('input[aria-label="Label for Review Selection"]')).toBeNull()
    act(() => (host.querySelector('button[aria-label="Expand Review Selection"]') as HTMLButtonElement).click())
    expect(host.textContent).toContain('Review skill')
    const label = host.querySelector('input[aria-label="Label for Review Selection"]') as HTMLInputElement
    const groupPath = host.querySelector('button[aria-label="Group for Review Selection"]') as HTMLInputElement
    const enabled = Array.from(host.querySelectorAll('input[type="checkbox"]'))[0] as HTMLInputElement
    // Moving an action to another group relocates its row, so controls are requeried after the move.
    const inputType = () => host.querySelector('button[aria-label="Input type for Review résumé"]') as HTMLButtonElement
    const provider = () => host.querySelector('button[aria-label="Provider for Review résumé"]') as HTMLButtonElement
    expect(host.querySelector('select')).toBeNull()
    expect(host.querySelector('button[aria-label="Input type for Review Selection"]')?.querySelector('[data-menu-select-icon]')).not.toBeNull()

    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(label, 'Review résumé')
      label.dispatchEvent(new Event('input', { bubbles: true }))
      enabled.click()
      groupPath.click()
    })
    const existingGroup = Array.from(document.body.querySelectorAll('[role="option"]')).find(option => option.textContent === 'GitHub') as HTMLButtonElement
    await act(async () => { existingGroup.click() })
    await act(async () => { inputType().click() })
    const filesOnly = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Files only')) as HTMLButtonElement
    await act(async () => { filesOnly.click() })

    await act(async () => { provider().click() })
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
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('Delete “Review résumé”?')
    const confirmDelete = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Delete action')
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
    const save = () => Array.from(document.body.querySelectorAll('button')).find(button => button.textContent==='Save and leave')!
    await act(async () => {save().click()})
    expect(leave).not.toHaveBeenCalled()
    expect(document.body.querySelector('[aria-label="Unsaved shell actions"]')).not.toBeNull()
    expect(host.textContent).toContain('New action')
    await act(async () => {save().click()})
    expect(leave).toHaveBeenCalledOnce()
    expect(apply).toHaveBeenCalledTimes(2)
    act(() => root.unmount()); host.remove()
  })

  it.each([
    ['input', { acceptsFiles: false, acceptsFolders: false }],
    ['skill', { skillPath: '' }],
  ] as const)('disables invalid shell %s saves and enables a corrected draft', async (_label, invalid) => {
    useAppStore.setState({ shellActions: [{ ...useAppStore.getState().shellActions[0], ...invalid }] })
    const ref = createRef<ShellActionsHandle>()
    const leave = vi.fn()
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    await act(async () => root.render(<ShellActionsSettings ref={ref} />))
    try {
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add shell action"]')!.click())
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(true)
      act(() => ref.current?.requestLeave(leave))
      expect(Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Save and leave')!.disabled).toBe(true)
      act(() => Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Go back')!.click())
      expect(document.activeElement?.getAttribute('aria-label')).toBe('Label for Review Selection')
      const control = host.querySelector<HTMLButtonElement>(`[aria-label="${_label === 'input' ? 'Input type' : 'Action'} for Review Selection"]`)!
      act(() => control.click())
      const option = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find(button => button.textContent?.startsWith(_label === 'input' ? 'Files and folders' : 'Open as Workspace'))!
      act(() => option.click())
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(false)
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.click())
      expect(useAppStore.getState().setShellActions).toHaveBeenCalledOnce()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each(['notification', 'backend error'] as const)('keeps a shell %s visible and dismissible beside invalid-draft validation', async kind => {
    const message = kind === 'notification' ? 'Shell integration needs refreshing.' : 'Could not save shell actions.'
    const dismiss = vi.fn(async () => { useAppStore.setState({ shellActionNotification: '' }) })
    useAppStore.setState({
      shellActionNotification: kind === 'notification' ? message : '',
      dismissShellActionNotification: dismiss,
      ...(kind === 'backend error' ? { setShellActions: vi.fn(async () => false) } : {}),
    })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    await act(async () => root.render(<ShellActionsSettings />))
    try {
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add shell action"]')!.click())
      if (kind === 'backend error') await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.click())
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand Review Selection"]')!.click())
      const label = host.querySelector<HTMLInputElement>('[aria-label="Label for Review Selection"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(label, '')
        label.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(Array.from(host.querySelectorAll('[role="status"]'), status => status.textContent).join(' ')).toContain(message)
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Each action needs a label')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(true)
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Dismiss shell action message"]')!.click())
      expect(dismiss).toHaveBeenCalledOnce()
      expect(host.textContent).not.toContain(message)
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Each action needs a label')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Dismiss shell action message"]')).toBeNull()
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps a reverted shell label clean and permits a valid new action', async () => {
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    await act(async () => root.render(<ShellActionsSettings />))
    try {
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand Review Selection"]')!.click())
      const label = host.querySelector<HTMLInputElement>('[aria-label="Label for Review Selection"]')!
      const change = (value: string) => act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(label, value)
        label.dispatchEvent(new Event('input', { bubbles: true }))
      })
      change('Changed')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(false)
      change('Review Selection')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(true)
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add shell action"]')!.click())
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(false)
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.click())
      expect(useAppStore.getState().setShellActions).toHaveBeenCalledOnce()
    } finally { act(() => root.unmount()); host.remove() }
  })

  describe('grouped actions', () => {
    const action = (id: string, label: string, groupPath: string[]) => ({
      id, label, groupPath, skillPath: '', providerId: '', modelId: '',
      acceptsFiles: true, acceptsFolders: true, enabled: true, openWorkspace: true,
    })
    const grouped = () => [
      action('a1', 'Review code', ['Code']),
      action('a2', 'Loose one', []),
      action('a3', 'Résumé check', ['Code', '日本語']),
      action('a4', 'Explain', ['Code']),
      action('a5', 'Lint', ['Code/日本語']),
      action('a6', 'Review GitLab', ['GitLab', 'Review']),
      action('a7', 'Review GitHub', ['GitHub', 'Review']),
    ]
    const titles = (element: Element) => Array.from(element.querySelectorAll('.uam-set-row__title')).map(title => title.textContent)
    const group = (host: HTMLElement, label: string) => host.querySelector<HTMLElement>(`[role="group"][aria-label="${label}"]`)!
    const header = (host: HTMLElement, label: string) => host.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)!
    const type = (element: HTMLInputElement, value: string) => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
      element.dispatchEvent(new Event('input', { bubbles: true }))
    }
    const option = (text: string) => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find(item => item.textContent === text)!
    async function mount() {
      const host = document.createElement('div'); document.body.append(host)
      const root = createRoot(host)
      const ref = createRef<ShellActionsHandle>()
      await act(async () => { root.render(<ShellActionsSettings ref={ref} />) })
      return { host, root, ref }
    }

    it('nests actions by exact path, isolates look-alike and Unicode paths, and keeps order and Ungrouped', async () => {
      useAppStore.setState({ shellActions: grouped() })
      const { host, root } = await mount()
      try {
        for (const label of ['Code', 'Code / 日本語', 'Code/日本語', 'Ungrouped', 'GitLab', 'GitLab / Review', 'GitHub', 'GitHub / Review']) {
          expect(header(host, label)?.getAttribute('aria-expanded'), label).toBe('true')
          expect(header(host, label).getAttribute('aria-controls')).toBe(group(host, label).id)
        }
        expect(titles(group(host, 'Code'))).toEqual(['Review code', 'Résumé check', 'Explain'])
        expect(titles(group(host, 'Code / 日本語'))).toEqual(['Résumé check'])
        expect(titles(group(host, 'Code/日本語'))).toEqual(['Lint'])
        expect(titles(group(host, 'Ungrouped'))).toEqual(['Loose one'])
        expect(titles(group(host, 'GitLab / Review'))).toEqual(['Review GitLab'])
        expect(titles(group(host, 'GitHub / Review'))).toEqual(['Review GitHub'])
        expect(group(host, 'Code').contains(group(host, 'Code / 日本語'))).toBe(true)
        expect(group(host, 'GitHub').contains(group(host, 'GitHub / Review'))).toBe(true)
        expect(group(host, 'GitLab').contains(group(host, 'GitHub / Review'))).toBe(false)
        expect(header(host, 'Code').textContent).toContain('3')
        expect(header(host, 'Code / 日本語').textContent).toContain('日本語')
      } finally { act(() => root.unmount()); host.remove() }
    })

    it('renders a flat list without group headers when nothing is grouped', async () => {
      useAppStore.setState({ shellActions: [action('a1', 'One', []), action('a2', 'Two', [])] })
      const { host, root } = await mount()
      try {
        expect(host.querySelector('[role="group"]')).toBeNull()
        expect(header(host, 'Ungrouped')).toBeNull()
        expect(titles(host)).toEqual(['One', 'Two'])
      } finally { act(() => root.unmount()); host.remove() }
    })

    it('collapses groups with native buttons without losing an open unsaved editor', async () => {
      useAppStore.setState({ shellActions: grouped() })
      const { host, root } = await mount()
      try {
        act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand Review code"]')!.click())
        const label = host.querySelector<HTMLInputElement>('input[aria-label="Label for Review code"]')!
        act(() => type(label, 'Review code ✓'))
        const code = header(host, 'Code')
        expect(code.tagName).toBe('BUTTON')
        expect(code.type).toBe('button')
        expect(code.tabIndex).not.toBe(-1)
        act(() => code.click())
        expect(code.getAttribute('aria-expanded')).toBe('false')
        expect(group(host, 'Code').hidden).toBe(true)
        expect(header(host, 'GitHub').getAttribute('aria-expanded')).toBe('true')
        expect(group(host, 'GitHub').hidden).toBe(false)
        expect(host.querySelector<HTMLButtonElement>('button[aria-label="Save shell actions"]')!.disabled).toBe(false)
        act(() => code.click())
        expect(code.getAttribute('aria-expanded')).toBe('true')
        expect(host.querySelector<HTMLInputElement>('input[aria-label="Label for Review code ✓"]')).toBe(label)
        expect(label.value).toBe('Review code ✓')
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Save shell actions"]')!.click())
        expect(useAppStore.getState().setShellActions).toHaveBeenCalledWith(expect.arrayContaining([expect.objectContaining({ id: 'a1', label: 'Review code ✓', groupPath: ['Code'] })]))
      } finally { act(() => root.unmount()); host.remove() }
    })

    it('keeps identity, draft and editor when renaming or moving an action into a collapsed group', async () => {
      useAppStore.setState({ shellActions: grouped() })
      const { host, root } = await mount()
      try {
        act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand Explain"]')!.click())
        const label = host.querySelector<HTMLInputElement>('input[aria-label="Label for Explain"]')!
        act(() => type(label, 'Explain ü'))
        expect(host.querySelector('input[aria-label="Label for Explain ü"]')).toBe(label)
        act(() => header(host, 'GitHub').click())
        expect(header(host, 'GitHub').getAttribute('aria-expanded')).toBe('false')
        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Group for Explain ü"]')!.click())
        await act(async () => option('GitHub / Review').click())
        expect(header(host, 'GitHub').getAttribute('aria-expanded')).toBe('true')
        expect(header(host, 'GitHub / Review').getAttribute('aria-expanded')).toBe('true')
        expect(titles(group(host, 'GitHub / Review'))).toEqual(['Explain ü', 'Review GitHub'])
        expect(titles(group(host, 'Code'))).toEqual(['Review code', 'Résumé check'])
        expect(host.querySelector<HTMLInputElement>('input[aria-label="Label for Explain ü"]')?.value).toBe('Explain ü')
        expect(document.activeElement).toBe(host.querySelector('button[aria-label="Group for Explain ü"]'))

        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Group for Explain ü"]')!.click())
        await act(async () => option('New group…').click())
        act(() => type(document.body.querySelector<HTMLInputElement>('input[aria-label="Group name"]')!, ' Ünï / Sub '))
        await act(async () => Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Create group')!.click())
        expect(titles(group(host, 'Ünï / Sub'))).toEqual(['Explain ü'])
        expect(host.querySelector<HTMLInputElement>('input[aria-label="Label for Explain ü"]')?.value).toBe('Explain ü')

        await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Save shell actions"]')!.click())
        const saved = vi.mocked(useAppStore.getState().setShellActions).mock.calls[0][0]
        expect(saved.map(item => item.id)).toEqual(['a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'])
        expect(saved[3]).toMatchObject({ id: 'a4', label: 'Explain ü', groupPath: ['Ünï', 'Sub'] })
      } finally { act(() => root.unmount()); host.remove() }
    })

    it('reveals a collapsed Ungrouped group when adding an action', async () => {
      useAppStore.setState({ shellActions: grouped() })
      const { host, root } = await mount()
      try {
        act(() => header(host, 'Ungrouped').click())
        expect(group(host, 'Ungrouped').hidden).toBe(true)
        act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Add shell action"]')!.click())
        expect(header(host, 'Ungrouped').getAttribute('aria-expanded')).toBe('true')
        expect(titles(group(host, 'Ungrouped'))).toEqual(['New action', 'Loose one'])
        expect(host.querySelector('input[aria-label="Label for New action"]')).not.toBeNull()
      } finally { act(() => root.unmount()); host.remove() }
    })

    it('keeps invalid Save disabled and returns focus to the action inside a collapsed group', async () => {
      useAppStore.setState({ shellActions: grouped() })
      const { host, root, ref } = await mount()
      const leave = vi.fn()
      try {
        act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand Review code"]')!.click())
        act(() => type(host.querySelector<HTMLInputElement>('[aria-label="Label for Review code"]')!, ''))
        act(() => header(host, 'Code').click())
        act(() => ref.current?.requestLeave(leave))
        expect(document.body.querySelector('[aria-label="Unsaved shell actions"]')).not.toBeNull()
        expect(host.querySelector<HTMLButtonElement>('[aria-label="Save shell actions"]')!.disabled).toBe(true)
        expect(Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Save and leave')!.disabled).toBe(true)
        expect(document.body.querySelector('[aria-label="Unsaved shell actions"]')?.textContent).toContain('Each action needs a label')
        await act(async () => Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Go back')!.click())
        expect(document.body.querySelector('[aria-label="Unsaved shell actions"]')).toBeNull()
        expect(leave).not.toHaveBeenCalled()
        expect(group(host, 'Code').hidden).toBe(false)
        expect(document.activeElement).toBe(host.querySelector('input[aria-label="Label for "]'))
        expect(useAppStore.getState().setShellActions).not.toHaveBeenCalled()
      } finally { act(() => root.unmount()); host.remove() }
    })
  })
})
