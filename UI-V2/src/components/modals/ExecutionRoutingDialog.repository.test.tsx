import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ExecutionRoutingDialog } from './ExecutionRoutingDialog'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'

vi.mock('../../ipc/cefBridge', async (importOriginal) => ({ ...(await importOriginal<typeof import('../../ipc/cefBridge')>()), sendToCEF: vi.fn() }))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const mounted: { host: HTMLElement; root: ReturnType<typeof createRoot> }[] = []
const workspaces = { workspaces: [
  { folderId: 'source', repository: { kind: 'git', identity: 'git@github.com:owner/repo.git' }, matches: [{ executionHostId: 'ssh-a', remoteDirectory: '/remote', source: 'automatic' }] },
  { folderId: 'other', repository: null, matches: [{ executionHostId: 'ssh-a', remoteDirectory: null, source: 'automatic' }] },
] }

function mount(component: React.ReactNode) { const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host); mounted.push({ host, root }); act(() => root.render(component)); return host }
function buttonsNamed(text: string) { return Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).filter((button) => button.textContent === text) }
function choose(selector: string, optionText: string) { act(() => document.body.querySelector<HTMLButtonElement>(selector)!.click()); act(() => Array.from(document.body.querySelectorAll<HTMLElement>('[role="option"]')).find((item) => item.textContent?.includes(optionText))!.click()) }
async function flush() { await act(async () => { await Promise.resolve() }) }

describe('SSH routing repository matching', () => {
  beforeEach(() => {
    vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: workspaces, requestId: 'r' })
    useAppStore.setState({
      executionRouting: { enabled: false, hosts: [], repositoryMatching: true },
      executionHosts: [{ id: 'ssh-a', label: 'Server A', transport: 'ssh', sshAlias: 'server', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }],
      folders: [
        { id: 'source', name: 'Source', directory: '/source', parentId: null, isExpanded: true, createdAt: new Date() },
        { id: 'other', name: 'Other', directory: '/other', parentId: null, isExpanded: true, createdAt: new Date() },
        { id: 'target', name: 'Remote checkout', directory: '/picked', executionHostId: 'ssh-a', parentId: null, isExpanded: true, createdAt: new Date() },
      ],
      providers: [],
    })
  })
  afterEach(() => { for (const { host, root } of mounted.splice(0)) { act(() => root.unmount()); host.remove() } vi.clearAllMocks() })

  it('keeps today\'s manual mapping and never asks for matches when the capability is off', () => {
    useAppStore.setState((state) => ({ executionRouting: { ...state.executionRouting, repositoryMatching: undefined } }))
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    expect(sendToCEF).not.toHaveBeenCalled()
    expect(buttonsNamed('Map folder…')).toHaveLength(1)
    expect(document.body.textContent).not.toContain('Same repository')
    expect(document.body.textContent).toContain('Not mapped')
  })

  it('shows the repository identity and automatic match, and saves a manual override without the capability flag', async () => {
    const save = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'listRepositoryMatches', payload: {} })
    const body = document.body.textContent ?? ''
    expect(body).toContain('github.com/owner/repo')
    expect(body).toContain('Same repository')
    expect(body).toContain('/remote')
    expect(body).toContain('1 workspace without a remote folder')
    expect(body).toContain('/other · No repository')

    act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Choose a different folder for Source on Server A"]')!.click())
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select Source"]')!.checked).toBe(true)
    choose('[aria-label="Remote folder for Source on Server A"]', 'Remote checkout')
    act(() => buttonsNamed('Map 1')[0].click())
    expect(document.body.querySelector('.uam-rt-map-row')?.textContent).toContain('Chosen')
    expect(document.body.querySelector('.uam-rt-map-row')?.textContent).toContain('/picked')
    await act(async () => buttonsNamed('Save routing')[0].click())
    expect(save).toHaveBeenCalledWith(expect.objectContaining({ hosts: [expect.objectContaining({ workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] })] }))
    expect(save.mock.calls[0][0]).not.toHaveProperty('repositoryMatching')
  })

  it('clears a saved override with useAutomaticMatch so the repository match applies again', async () => {
    const save = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState({
      setExecutionRoutingSettings: save,
      executionRouting: { enabled: true, repositoryMatching: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] }] },
    })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    expect(document.body.querySelector('.uam-rt-map-row')?.textContent).toContain('Chosen')
    act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Use repository match for Source on Server A"]')!.click())
    expect(document.body.querySelector('.uam-rt-map-row')?.textContent).toContain('Same repository')
    await act(async () => buttonsNamed('Save routing')[0].click())
    expect(save).toHaveBeenCalledWith(expect.objectContaining({ hosts: [expect.objectContaining({ workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: '', useAutomaticMatch: true }] })] }))
  })
})
