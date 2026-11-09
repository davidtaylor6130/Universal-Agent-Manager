import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { AutomationSettings } from './AutomationSettings'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const send = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => false, sendToCEF: send }))
vi.mock('../../utils/appFailure', () => ({ reportAppFailure: vi.fn() }))

const token = { id: 'at_1', name: 'n8n homelab', trusted: false, workspaces: ['folder-a'], maxGitPermission: 'commit', createdAt: '2026-11-09' }
const secret = `uamat_${'a'.repeat(64)}`
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>

const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve() })
const button = (text: string) => [...document.querySelectorAll('button')].find(item => item.textContent?.trim() === text || item.getAttribute('aria-label') === text)!
const type = (input: HTMLInputElement, value: string) => act(() => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
  input.dispatchEvent(new Event('input', { bubbles: true }))
})

beforeEach(async () => {
  previous = useAppStore.getState()
  useAppStore.setState({ folders: [{ id: 'folder-a', name: 'Parser', parentId: null, directory: '/work/parser', isExpanded: true, createdAt: new Date() }] })
  send.mockImplementation(async ({ action }: { action: string }) => {
    if (action === 'getAutomationTokens') return { ok: true, data: { tokens: [token] } }
    if (action === 'getCompanionSettings') return { ok: true, data: { effectiveUrl: 'https://uam.local:58950/companion' } }
    if (action === 'createAutomationToken') return { ok: true, data: { secret, token: { ...token, id: 'at_2', name: 'New one' } } }
    return { ok: true, data: {} }
  })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  act(() => root.render(<AutomationSettings />))
  await flush()
})

afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  document.body.innerHTML = ''
  vi.clearAllMocks()
})

describe('Automation settings', () => {
  it('lists token metadata and the endpoint', () => {
    expect(host.textContent).toContain('n8n homelab')
    expect(host.textContent).toContain('Waits for approval')
    expect(host.textContent).toContain('https://uam.local:58950/automation/chats')
  })

  it('shows a new secret once and forgets it when closed', async () => {
    act(() => button('New token').click())
    expect((button('Create token') as HTMLButtonElement).disabled).toBe(true)
    type(document.querySelector<HTMLInputElement>('[aria-label="Token name"]')!, 'New one')
    act(() => document.querySelector<HTMLElement>('[aria-label="Allow Parser"]')!.click())
    expect((button('Create token') as HTMLButtonElement).disabled).toBe(false)
    act(() => document.querySelector<HTMLElement>('[aria-label="Allow Parser"]')!.click())
    expect((button('Create token') as HTMLButtonElement).disabled).toBe(true)
    act(() => document.querySelector<HTMLElement>('[aria-label="Allow Parser"]')!.click())
    await act(async () => { button('Create token').click(); await Promise.resolve() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'createAutomationToken', payload: { name: 'New one', trusted: false, maxGitPermission: 'commit', workspaces: ['folder-a'] } })
    expect(document.querySelector<HTMLInputElement>('[aria-label="Automation token"]')?.value).toBe(secret)
    act(() => button('Done').click())
    await flush()
    expect(document.body.innerHTML).not.toContain(secret)
    expect(host.textContent).toContain('New one')
    act(() => button('New token').click())
    expect(document.querySelector('[aria-label="Automation token"]')).toBeNull()
  })

  it('revokes only after confirmation', async () => {
    act(() => button('Revoke n8n homelab').click())
    expect(send).not.toHaveBeenCalledWith(expect.objectContaining({ action: 'revokeAutomationToken' }))
    await act(async () => { button('Revoke token').click(); await Promise.resolve() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'revokeAutomationToken', payload: { id: 'at_1' } })
    expect(host.textContent).not.toContain('n8n homelab')
  })
})
