import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ExecutionRoutingSettings, type ExecutionRoutingSettingsHandle } from './ExecutionRoutingSettings'
import { useAppStore } from '../../store/useAppStore'
import type { ExecutionRoutingSettings as RoutingSettings } from '../../types/executionRouting'

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
    expect(document.body.querySelector('[role="alert"]')?.textContent).toBe('Disk full')
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
    const select = document.body.querySelector<HTMLSelectElement>('[aria-label="Destination for source-a on Server A"]')!
    act(() => { select.value = 'target-b'; select.dispatchEvent(new Event('change', { bubbles: true })) })
    expect(buttonNamed('Save routing').disabled).toBe(false)
    act(() => { select.value = 'target-a'; select.dispatchEvent(new Event('change', { bubbles: true })) })
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
  it.each(['page', 'dialog'] as const)('keeps invalid capacity disabled and enables only valid changes in the %s', async variant => {
    const save = vi.fn(async () => {})
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings variant={variant} />)
    const capacity = document.body.querySelector<HTMLInputElement>('[aria-label="Concurrent sessions on Server A"]')!
    const change = (value: string) => act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(capacity, value)
      capacity.dispatchEvent(new Event('input', { bubbles: true }))
    })
    change('1.5')
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('whole number')
    await act(async () => buttonNamed('Save routing').click())
    expect(save).not.toHaveBeenCalled()
    change('2')
    expect(buttonNamed('Save routing').disabled).toBe(false)
    change('1')
    expect(buttonNamed('Save routing').disabled).toBe(true)
    expect(document.body.querySelector('[role="alert"]')).toBeNull()
  })

  it('disables only Save and leave for invalid routing and saves a corrected new draft', async () => {
    const handle = createRef<ExecutionRoutingSettingsHandle>()
    const leave = vi.fn()
    const save = vi.fn(async () => {})
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingSettings ref={handle} variant="page" />)
    const capacity = document.body.querySelector<HTMLInputElement>('[aria-label="Concurrent sessions on Server A"]')!
    const change = (value: string) => act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(capacity, value)
      capacity.dispatchEvent(new Event('input', { bubbles: true }))
    })
    change('1025')
    act(() => handle.current!.requestLeave(leave))
    expect(buttonNamed('Save and leave').disabled).toBe(true)
    expect(buttonNamed('Keep editing').disabled).toBe(false)
    expect(buttonNamed('Discard').disabled).toBe(false)
    expect(document.body.querySelector('[role="alertdialog"] [role="alert"]')?.textContent).toContain('whole number')
    act(() => buttonNamed('Keep editing').click())
    change('2')
    act(() => handle.current!.requestLeave(leave))
    expect(buttonNamed('Save and leave').disabled).toBe(false)
    await act(async () => buttonNamed('Save and leave').click())
    expect(save).toHaveBeenCalledOnce()
    expect(leave).toHaveBeenCalledOnce()
  })

})
