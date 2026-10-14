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
function choose(selector: string, value: string) { const select = document.body.querySelector<HTMLSelectElement>(selector)!; act(() => { select.value = value; select.dispatchEvent(new Event('change', { bubbles: true })) }) }
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
    expect(document.body.querySelector('[aria-label="Destination for Source on Server A"]')).not.toBeNull()
    expect(buttonsNamed('Choose folder…')).toHaveLength(0)
  })

  it('shows the repository identity and automatic match, and saves a manual override without the capability flag', async () => {
    const save = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState({ setExecutionRoutingSettings: save })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'listRepositoryMatches', payload: {} })
    const body = document.body.textContent ?? ''
    expect(body).toContain('github.com/owner/repo')
    expect(body).toContain('Matched by repository')
    expect(body).toContain('/remote')
    expect(body).toContain('No matching checkout')
    expect(body).toContain('No repository detected')
    expect(document.body.querySelector('[aria-label="Destination for Source on Server A"]')).toBeNull()

    act(() => buttonsNamed('Choose folder…')[0].click())
    choose('[aria-label="Choose folder for Source on Server A"]', 'target')
    expect(document.body.textContent).toContain('Chosen folder')
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
    expect(document.body.textContent).toContain('Chosen folder')
    act(() => buttonsNamed('Use automatic match')[0].click())
    expect(document.body.textContent).toContain('Matched by repository')
    await act(async () => buttonsNamed('Save routing')[0].click())
    expect(save).toHaveBeenCalledWith(expect.objectContaining({ hosts: [expect.objectContaining({ workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: '', useAutomaticMatch: true }] })] }))
  })
})
