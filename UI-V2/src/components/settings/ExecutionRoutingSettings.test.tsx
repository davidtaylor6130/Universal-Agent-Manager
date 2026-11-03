import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ExecutionRoutingSettings, type ExecutionRoutingSettingsHandle } from './ExecutionRoutingSettings'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'

import type { ExecutionRoutingSettings as RoutingSettings } from '../../types/executionRouting'
vi.mock('../../utils/appFailure', async importOriginal => ({ ...await importOriginal<typeof import('../../utils/appFailure')>(), reportAppFailure: vi.fn() }));

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const originalSave = useAppStore.getState().setExecutionRoutingSettings
const mounted: { host: HTMLElement; root: ReturnType<typeof createRoot> }[] = []
function mount(component: React.ReactNode) { const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host); mounted.push({ host, root }); act(() => root.render(component)); return host }
function buttonNamed(text: string) { return Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === text)! }

describe('SSH Routing settings page', () => {
  beforeEach(() => {
    useAppStore.setState({
      executionRouting: { enabled: false, hosts: [] }, setExecutionRoutingSettings: originalSave,
      executionHosts: [{ id: 'ssh-a', label: 'Server A', transport: 'ssh', sshAlias: 'server', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }],
      folders: [], providers: [],
    })
  })
  afterEach(() => { for (const { host, root } of mounted.splice(0)) { act(() => root.unmount()); host.remove() } })

  it('keeps Save and Discard disabled until a draft differs, then saves the routing draft', async () => {
    const save = vi.fn(async (settings: RoutingSettings) => { useAppStore.setState({ executionRouting: settings }) })
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings variant="page" />)
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(buttonNamed('Discard').disabled).toBe(true)
    act(() => (document.body.querySelector('#routing-enabled') as HTMLInputElement).click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    act(() => buttonNamed('Discard').click())
    expect((document.body.querySelector('#routing-enabled') as HTMLInputElement).checked).toBe(false)
    act(() => (document.body.querySelector('#routing-enabled') as HTMLInputElement).click())
    await act(async () => buttonNamed('Save routing').click())
    expect(save).toHaveBeenCalledWith({ enabled: true, hosts: [] })
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(buttonNamed('Discard').disabled).toBe(true)
    expect((document.body.querySelector('#routing-enabled') as HTMLInputElement).checked).toBe(true)
  })

  it.each(['page', 'dialog'] as const)('disables unchanged and reverted routing drafts in the %s', async (variant) => {
    const save = vi.fn()
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings variant={variant} />)
    expect(buttonNamed('Save routing').disabled).toBe(true)
    await act(async () => buttonNamed('Save routing').click())
    expect(save).not.toHaveBeenCalled()
    const enabled = document.body.querySelector<HTMLInputElement>('#routing-enabled')!
    act(() => enabled.click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    act(() => enabled.click())
    expect(buttonNamed('Save routing').disabled).toBe(true)
  })

  it('uses acknowledged native confirmation identities as the clean draft and keeps failed edits retryable', async () => {
    useAppStore.setState({ providers: [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '' }] })
    const save = vi.fn(async (settings: RoutingSettings) => {
      useAppStore.setState({ executionRouting: { ...settings, hosts: settings.hosts.map((host) => ({ ...host, providers: [{ providerId: 'codex-cli', connectionIdentity: 'saved-connection', providerConfigurationIdentity: 'saved-configuration' }] })) } })
    })
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings variant="page" />)
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Codex signed in on Server A"]')!.click())
    await act(async () => buttonNamed('Save routing').click())
    expect(save.mock.calls[0][0].hosts[0].providers[0].confirm).toBe(true)
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(buttonNamed('Discard').disabled).toBe(true)
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Codex signed in on Server A"]')!.checked).toBe(true)
    act(() => document.body.querySelector<HTMLInputElement>('#routing-enabled')!.click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    save.mockImplementationOnce(async () => {
      useAppStore.setState({ executionRouting: structuredClone(useAppStore.getState().executionRouting) })
      throw new Error('Disk full')
    })
    await act(async () => buttonNamed('Save routing').click())
    expect(vi.mocked(reportAppFailure)).toHaveBeenCalledWith('Routing settings could not be saved.', 'Disk full')
    expect(buttonNamed('Save routing').disabled).toBe(false)
    expect(document.body.querySelector<HTMLInputElement>('#routing-enabled')!.checked).toBe(true)
    act(() => document.body.querySelector<HTMLInputElement>('#routing-enabled')!.click())
    expect(buttonNamed('Save routing').disabled).toBe(true)
  })

  it('cleans reverted host defaults and preserves existing host order', () => {
    mount(<ExecutionRoutingSettings variant="page" />)
    const toggle = document.body.querySelector<HTMLInputElement>('[aria-label="Route to Server A"]')!
    act(() => toggle.click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    act(() => toggle.click())
    expect(buttonNamed('Save routing').disabled).toBe(true)
    act(() => useAppStore.setState({ executionRouting: { enabled: false, hosts: [
      { hostId: 'ssh-a', enabled: false, maxActiveTurns: 2, providers: [], workspaceMappings: [] },
      { hostId: 'ssh-b', enabled: false, maxActiveTurns: 3, providers: [], workspaceMappings: [] },
    ] } }))
    act(() => toggle.click())
    act(() => toggle.click())
    expect(buttonNamed('Save routing').disabled).toBe(true)
  })

  it('retains edited intent against a newer native baseline until explicitly discarded', () => {
    mount(<ExecutionRoutingSettings variant="page" />)
    const toggle = document.body.querySelector<HTMLInputElement>('#routing-enabled')!
    act(() => toggle.click())
    const newer: RoutingSettings = { enabled: false, hosts: [{ hostId: 'ssh-a', enabled: false, maxActiveTurns: 5, providers: [], workspaceMappings: [] }] }
    act(() => useAppStore.setState({ executionRouting: newer }))
    expect(toggle.checked).toBe(true)
    act(() => toggle.click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    expect(buttonNamed('Discard').disabled).toBe(false)
    act(() => buttonNamed('Discard').click())
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Concurrent sessions on Server A"]')!.value).toBe('5')
    expect(buttonNamed('Save routing').disabled).toBe(true)
  })

  it('retains a newer native baseline when an in-flight save fails', async () => {
    let reject!: (error: Error) => void
    useAppStore.setState({ setExecutionRoutingSettings: vi.fn(() => new Promise<void>((_resolve, fail) => { reject = fail })) })
    mount(<ExecutionRoutingSettings variant="page" />)
    const toggle = document.body.querySelector<HTMLInputElement>('#routing-enabled')!
    act(() => toggle.click())
    await act(async () => buttonNamed('Save routing').click())
    act(() => useAppStore.setState({ executionRouting: { enabled: false, hosts: [{ hostId: 'ssh-a', enabled: false, maxActiveTurns: 5, providers: [], workspaceMappings: [] }] } }))
    await act(async () => reject(new Error('Disk full')))
    expect(toggle.checked).toBe(true)
    act(() => toggle.click())
    expect(buttonNamed('Save routing').disabled).toBe(false)
    act(() => buttonNamed('Discard').click())
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Concurrent sessions on Server A"]')!.value).toBe('5')
    expect(buttonNamed('Save routing').disabled).toBe(true)
  })

  it('keeps reverted destination replacements clean without reordering existing mappings', () => {
    const folder = (id: string, directory: string, executionHostId?: string) => ({ id, name: id, directory, executionHostId, parentId: null, isExpanded: true, createdAt: new Date() })
    useAppStore.setState({ folders: [folder('source-a', '/a'), folder('source-b', '/b'), folder('target-a', '/remote/a', 'ssh-a'), folder('target-b', '/remote/b', 'ssh-a')],
      executionRouting: { enabled: false, hosts: [{ hostId: 'ssh-a', enabled: false, maxActiveTurns: 1, providers: [], workspaceMappings: [
        { sourceFolderId: 'source-a', sourceDirectory: '/a', destinationFolderId: 'target-a' },
        { sourceFolderId: 'source-b', sourceDirectory: '/b', destinationFolderId: 'target-b' },
      ] }] } })
    mount(<ExecutionRoutingSettings variant="page" />)
    const choose = (label: string) => {
      act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Choose a different folder for source-a on Server A"]')!.click())
      act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Remote folder for source-a on Server A"]')!.click())
      act(() => Array.from(document.body.querySelectorAll<HTMLElement>('[role="option"]')).find((item) => item.textContent?.includes(label))!.click())
      act(() => buttonNamed('Map 1').click())
    }
    choose('target-b')
    expect(buttonNamed('Save routing').disabled).toBe(false)
    choose('target-a')
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(buttonNamed('Discard').disabled).toBe(true)
  })

  it('blocks editing, discard and navigation until the native save acknowledges', async () => {
    let finish!: () => void
    const save = vi.fn((settings: RoutingSettings) => new Promise<void>(resolve => {
      finish = () => { useAppStore.setState({ executionRouting: settings }); resolve() }
    }))
    useAppStore.setState({ setExecutionRoutingSettings: save })
    const handle = createRef<ExecutionRoutingSettingsHandle>()
    const leave = vi.fn()
    mount(<ExecutionRoutingSettings ref={handle} variant="page" />)
    const input = document.body.querySelector<HTMLInputElement>('#routing-enabled')!
    act(() => input.click())
    await act(async () => buttonNamed('Save routing').click())
    expect(input.closest('fieldset')!.disabled).toBe(true)
    expect(input.matches(':disabled')).toBe(true)
    expect(buttonNamed('Discard').disabled).toBe(true)
    expect(buttonNamed('Save routing').disabled).toBe(true)
    act(() => { input.click(); buttonNamed('Discard').click(); handle.current!.requestLeave(leave) })
    expect(input.checked).toBe(true)
    expect(leave).not.toHaveBeenCalled()
    await act(async () => finish())
    expect(input.closest('fieldset')!.disabled).toBe(false)
    expect(input.checked).toBe(true)
    expect(buttonNamed('Save routing').disabled).toBe(true)
    act(() => handle.current!.requestLeave(leave))
    expect(leave).toHaveBeenCalledOnce()
  })

  it('asks before leaving with unsaved edits and stays when the user keeps editing', () => {
    const handle = createRef<ExecutionRoutingSettingsHandle>()
    const leave = vi.fn()
    mount(<ExecutionRoutingSettings ref={handle} variant="page" />)
    act(() => handle.current!.requestLeave(leave))
    expect(leave).toHaveBeenCalledTimes(1)
    act(() => (document.body.querySelector('#routing-enabled') as HTMLInputElement).click())
    act(() => handle.current!.requestLeave(leave))
    expect(leave).toHaveBeenCalledTimes(1)
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('Unsaved routing settings')
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')).find((button) => button.textContent === 'Keep editing')!.click())
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    expect(leave).toHaveBeenCalledTimes(1)
  })
  it('clamps concurrent sessions into range on blur and enables Save for the clamped draft', () => {
    const save = vi.fn(async () => {})
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings variant="page" />)
    const capacity = document.body.querySelector<HTMLInputElement>('[aria-label="Concurrent sessions on Server A"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(capacity, '1025')
      capacity.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect(buttonNamed('Save routing').disabled).toBe(true)
    act(() => capacity.focus())
    act(() => capacity.blur())
    expect(capacity.value).toBe('1024')
    expect(buttonNamed('Save routing').disabled).toBe(false)
  })

  describe('folders', () => {
    const folder = (id: string, directory: string, executionHostId?: string) => ({ id, name: id, directory, executionHostId, parentId: null, isExpanded: true, createdAt: new Date() })
    const host = (id: string, label: string) => ({ id, label, transport: 'ssh' as const, sshAlias: id, runnerStatus: 'ready' as const, runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' })
    const summaries = () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('section[aria-label="Folders"] .uam-disclosure__summary'))
    const pick = (label: string, option: string) => {
      act(() => document.body.querySelector<HTMLButtonElement>(`[aria-label="${label}"]`)!.click())
      act(() => Array.from(document.body.querySelectorAll<HTMLElement>('[role="option"]')).find((item) => item.textContent?.includes(option))!.click())
    }
    beforeEach(() => useAppStore.setState({
      executionHosts: [host('ssh-a', 'Server A'), host('ssh-b', 'Server B')],
      folders: [folder('ok', '/ok'), folder('broken', '/broken'), folder('plain-1', '/plain-1'), folder('plain-2', '/plain-2'), folder('target-a', '/remote/a', 'ssh-a')],
    }))

    it('sorts problem workspaces first and open, then mapped ones, and gathers the rest in one Not mapped group', () => {
      useAppStore.setState({ executionRouting: { enabled: false, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [
        { sourceFolderId: 'ok', sourceDirectory: '/ok', destinationFolderId: 'target-a' },
        { sourceFolderId: 'broken', sourceDirectory: '/broken', destinationFolderId: 'gone' },
      ] }] } })
      mount(<ExecutionRoutingSettings variant="page" />)
      const [problem, mapped, rest] = summaries()
      expect(summaries()).toHaveLength(3)
      expect(problem.textContent).toContain('broken')
      expect(problem.textContent).toContain('Folder no longer exists')
      expect(problem.getAttribute('aria-expanded')).toBe('true')
      expect(mapped.textContent).toContain('1 computer')
      expect(rest.textContent).toBe('Not mapped3 workspaces without a remote folder')
      expect(rest.getAttribute('aria-expanded')).toBe('false')
      expect(document.body.querySelector('section[aria-label="Folders"]')!.textContent).toContain('2 of 5 workspaces mapped')
      expect(document.body.querySelector('[data-problem] .uam-rt-map-row__host')?.textContent).toBe('Server A')
      expect(buttonNamed('Save routing').disabled).toBe(true)
      act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Remove mapping for broken on Server A"]')!.click())
      expect(summaries()).toHaveLength(2)
      expect(buttonNamed('Save routing').disabled).toBe(false)
    })

    it('stages chooser mappings in the unsaved draft without saving, and Escape closes the chooser', async () => {
      const save = vi.fn(async (settings: RoutingSettings) => { useAppStore.setState({ executionRouting: settings }) })
      useAppStore.setState({ setExecutionRoutingSettings: save, executionRouting: { enabled: false, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [{ sourceFolderId: 'ok', sourceDirectory: '/ok', destinationFolderId: 'target-a' }] }] } })
      mount(<ExecutionRoutingSettings variant="page" />)
      const opener = buttonNamed('Map folder…')
      act(() => { opener.focus(); opener.click() })
      const chooser = document.body.querySelector<HTMLElement>('[role="dialog"][aria-label="Map folders to a computer"]')!
      expect(chooser.textContent).toContain('ok already has a folder on Server A.')
      const filter = chooser.querySelector<HTMLInputElement>('[aria-label="Filter workspaces"]')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(filter, 'plain'); filter.dispatchEvent(new Event('input', { bubbles: true })) })
      expect(Array.from(chooser.querySelectorAll('.uam-rv-row__title')).map((item) => item.textContent)).toEqual(['plain-1', 'plain-2'])
      act(() => chooser.querySelector<HTMLInputElement>('[aria-label="Select all workspaces"]')!.click())
      expect(chooser.textContent).toContain('2 selected')
      expect(buttonNamed('Map 2').disabled).toBe(true)
      expect(chooser.textContent).toContain('Choose a remote folder for all 2 selected workspaces.')
      pick('Remote folder for plain-1 on Server A', 'target-a')
      pick('Remote folder for plain-2 on Server A', 'target-a')
      act(() => buttonNamed('Map 2').click())
      expect(document.body.querySelector('[aria-label="Map folders to a computer"]')).toBeNull()
      expect(document.activeElement).toBe(opener)
      expect(save).not.toHaveBeenCalled()
      expect(summaries().map((item) => item.textContent)).toEqual(['ok1 computer', 'plain-11 computer', 'plain-21 computer', 'Not mapped2 workspaces without a remote folder'])
      expect(buttonNamed('Save routing').disabled).toBe(false)

      act(() => buttonNamed('Map…').click())
      expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select broken"]')!.checked).toBe(true)
      act(() => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })) })
      expect(document.body.querySelector('[aria-label="Map folders to a computer"]')).toBeNull()

      await act(async () => buttonNamed('Save routing').click())
      expect(save.mock.calls[0][0].hosts[0].workspaceMappings).toEqual([
        { sourceFolderId: 'ok', sourceDirectory: '/ok', destinationFolderId: 'target-a' },
        { sourceFolderId: 'plain-1', sourceDirectory: '/plain-1', destinationFolderId: 'target-a' },
        { sourceFolderId: 'plain-2', sourceDirectory: '/plain-2', destinationFolderId: 'target-a' },
      ])
    })

    it('says when a computer has no folders to map to', () => {
      mount(<ExecutionRoutingSettings variant="page" />)
      act(() => buttonNamed('Map folder…').click())
      act(() => buttonNamed('Server B').click())
      expect(document.body.querySelector('[role="dialog"]')!.textContent).toContain('No folders on Server B yet. Add a folder on Server B first.')
      expect(buttonNamed('Map').disabled).toBe(true)
    })
  })
})
