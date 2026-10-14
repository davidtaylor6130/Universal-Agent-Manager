import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ExecutionRoutingSettings, type ExecutionRoutingSettingsHandle } from './ExecutionRoutingSettings'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const mounted: { host: HTMLElement; root: ReturnType<typeof createRoot> }[] = []
function mount(component: React.ReactNode) { const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host); mounted.push({ host, root }); act(() => root.render(component)); return host }
function buttonNamed(text: string) { return Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === text)! }

describe('SSH Routing settings page', () => {
  beforeEach(() => {
    useAppStore.setState({
      executionRouting: { enabled: false, hosts: [] },
      executionHosts: [{ id: 'ssh-a', label: 'Server A', transport: 'ssh', sshAlias: 'server', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }],
      folders: [], providers: [],
    })
  })
  afterEach(() => { for (const { host, root } of mounted.splice(0)) { act(() => root.unmount()); host.remove() } })

  it('keeps Save and Discard disabled until a draft differs, then saves the routing draft', async () => {
    const save = vi.fn().mockResolvedValue(undefined)
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
})
