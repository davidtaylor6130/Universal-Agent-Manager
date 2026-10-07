import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ExecutionRoutingDialog, ExecutionRoutingWaitNotice } from './ExecutionRoutingDialog'
import { useAppStore } from '../../store/useAppStore'
import { createExecutionRoutingSlice } from '../../store/slices/executionRoutingSlice'
import type { Session } from '../../types/session'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const originalSave = useAppStore.getState().setExecutionRoutingSettings
const originalRelease = useAppStore.getState().releaseExecutionRoutingReservation
const mounted: { host: HTMLElement; root: ReturnType<typeof createRoot> }[] = []
function mount(component: React.ReactNode) { const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host); mounted.push({ host, root }); act(() => root.render(component)); return host }
function input(selector: string, value: string) { const element = document.body.querySelector<HTMLInputElement>(selector)!; act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value); element.dispatchEvent(new Event('input', { bubbles: true })) }) }

describe('SSH routing controls', () => {
  beforeEach(() => {
    useAppStore.setState({ executionRouting: { enabled: false, hosts: [] },
      setExecutionRoutingSettings: originalSave, releaseExecutionRoutingReservation: originalRelease,
      executionHosts: [{ id: 'ssh-a', label: 'Server A', transport: 'ssh', sshAlias: 'server', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }],
      folders: [
        { id: 'source', name: 'Source', directory: '/source', parentId: null, isExpanded: true, createdAt: new Date() },
        { id: 'target', name: 'Destination', directory: '/remote', executionHostId: 'ssh-a', parentId: null, isExpanded: true, createdAt: new Date() },
        { id: 'wrong', name: 'Other computer folder', directory: '/other', executionHostId: 'ssh-b', parentId: null, isExpanded: true, createdAt: new Date() },
      ], providers: [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '', supportsStructured: true }],
    })
  })
  afterEach(() => { for (const { host, root } of mounted.splice(0)) { act(() => root.unmount()); host.remove() } useAppStore.setState({ setExecutionRoutingSettings: originalSave, releaseExecutionRoutingReservation: originalRelease }); delete window.cefQuery })

  it('requires integer capacity, maps only registered destinations on this SSH host, and keeps configuration after save failure', async () => {
    const save = vi.fn().mockRejectedValue(new Error('Disk full'))
    const close = vi.fn()
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingDialog onClose={close} />)
    act(() => (document.body.querySelector('[aria-label="Route to Server A"]') as HTMLInputElement).click())
    input('[aria-label="Concurrent sessions on Server A"]', '1.5')
    const submit = () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save routing')!
    await act(async () => submit().click())
    expect(save).not.toHaveBeenCalled()
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('whole number')
    input('[aria-label="Concurrent sessions on Server A"]', '2')
    const destination = document.body.querySelector<HTMLSelectElement>('[aria-label="Destination for Source on Server A"]')!
    expect(Array.from(destination.options).map((option) => option.value)).toEqual(['', 'target'])
    act(() => { destination.value = 'target'; destination.dispatchEvent(new Event('change', { bubbles: true })); (document.body.querySelector('[aria-label="Codex signed in on Server A"]') as HTMLInputElement).click() })
    await act(async () => submit().click())
    expect(save).toHaveBeenCalledWith(expect.objectContaining({ hosts: [expect.objectContaining({ maxActiveTurns: 2, providers: [{ providerId: 'codex-cli', connectionIdentity: '', providerConfigurationIdentity: '', confirm: true }], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] })] }))
    expect(close).not.toHaveBeenCalled()
    expect(document.body.querySelector('[role="alert"]')?.textContent).toBe('Disk full')
    expect(destination.value).toBe('target')
  })

  it('shows an actionable wait and rejects release when the assignment changes during confirmation', async () => {
    const session: Session = { id: 'chat', name: 'Waiting', viewMode: 'chat', folderId: 'target', executionHostId: 'ssh-a', workspaceDirectory: '/remote', createdAt: new Date(), updatedAt: new Date(), routingWaitReason: 'All configured computers are busy.', routingReservationPending: true, routingUnresolvedReservation: true, routingAssignedConnectionIdentity: 'host-v1', routingAssignedProviderConfigurationIdentity: 'provider-v1' }
    useAppStore.setState({ sessions: [session], releaseExecutionRoutingReservation: createExecutionRoutingSlice(useAppStore.getState).releaseExecutionRoutingReservation })
    const host = mount(<ExecutionRoutingWaitNotice session={session} />)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('busy')
    expect(host.querySelector('[role="alert"]')).toBeNull()
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Release reservation')!.click())
    expect(document.body.querySelector('[role="alertdialog"]')?.textContent).toContain('runner process may still exist')
    useAppStore.setState({ sessions: [{ ...session, routingAssignedConnectionIdentity: 'host-v2' }] })
    const query = vi.fn(); window.cefQuery = query
    await act(async () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')).find((button) => button.textContent === 'Release reservation')!.click())
    expect(document.body.querySelector('[role="alertdialog"] [role="alert"]')?.textContent).toContain('assignment changed')
    expect(query).not.toHaveBeenCalled()
  })
  it('requires manual reconfirmation after provider configuration changes and leaves a newer dialog focused on unmount', async () => {
    const save = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState((state) => ({
      setExecutionRoutingSettings: save,
      providers: [...state.providers, { id: 'terminal-provider', name: 'Terminal provider', shortName: 'Terminal', color: '', description: '', supportsStructured: false, supportsCli: true }],
      executionRouting: { enabled: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [{ providerId: 'codex-cli', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1' }], workspaceMappings: [] }] },
      cliVersionManager: { providers: [], remoteProviders: [{ providerId: 'codex-cli', executionHostId: 'ssh-a', routingConnectionIdentity: 'host-v1', routingProviderConfigurationIdentity: 'provider-v2', installedVersion: '', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }] },
    }))
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    const confirmation = document.body.querySelector<HTMLInputElement>('[aria-label="Codex signed in on Server A"]')!
    expect(confirmation.checked).toBe(false)
    expect(document.body.querySelector('[aria-label="Terminal provider signed in on Server A"]')).not.toBeNull()
    act(() => confirmation.click())
    await act(async () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save routing')!.click())
    expect(save.mock.calls[0][0].hosts[0].providers[0]).toMatchObject({ providerId: 'codex-cli', confirm: true })
    const newer = document.createElement('div'); newer.setAttribute('role', 'dialog'); newer.setAttribute('aria-modal', 'true')
    const input = document.createElement('input'); newer.appendChild(input); document.body.appendChild(newer); input.focus()
    const component = mounted.pop()!
    act(() => component.root.unmount()); component.host.remove()
    expect(document.activeElement).toBe(input)
    newer.remove()
  })

})
