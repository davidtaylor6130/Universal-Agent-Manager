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

  it('keeps folder mapping usable while matches are checked, bounds a slow check and lets Retry rejoin it', async () => {
    vi.useFakeTimers()
    try {
      let answer!: (value: unknown) => void
      vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { answer = resolve }) as never)
      mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
      await flush()
      expect(document.body.textContent).toContain('Checking repositories')
      expect(document.body.textContent).toContain('/other · Checking repository…')
      expect(buttonsNamed('Map folder…')[0].disabled).toBe(false)

      await act(async () => { vi.advanceTimersByTime(30_000); await Promise.resolve() })
      expect(document.body.textContent).toContain('Repository matching is unavailable right now')
      expect(document.body.textContent).not.toContain('Checking repositories')

      act(() => buttonsNamed('Retry')[0].click())
      expect(document.activeElement?.textContent).toBe('Map folder…')
      await flush()
      // The slow check still holds its native slot, so Retry waits on it rather than sending another.
      expect(sendToCEF).toHaveBeenCalledTimes(1)
      expect(document.body.textContent).toContain('Checking repositories')
      await act(async () => { answer({ ok: true, data: workspaces, requestId: 'r' }); await Promise.resolve(); await Promise.resolve() })
      expect(document.body.textContent).toContain('Same repository')
    } finally { vi.useRealTimers() }
  })

  it('drops a check that stays silent through a rejoined wait, so the next Retry sends a new one', async () => {
    vi.useFakeTimers()
    try {
      vi.mocked(sendToCEF).mockReturnValueOnce(new Promise(() => {}) as never)
      mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
      await flush()
      await act(async () => { vi.advanceTimersByTime(30_000); await Promise.resolve() })
      act(() => buttonsNamed('Retry')[0].click())
      await flush()
      expect(sendToCEF).toHaveBeenCalledTimes(1)
      await act(async () => { vi.advanceTimersByTime(30_000); await Promise.resolve() })
      act(() => buttonsNamed('Retry')[0].click())
      await flush()
      expect(sendToCEF).toHaveBeenCalledTimes(2)
      expect(document.body.textContent).toContain('Same repository')
    } finally { vi.useRealTimers() }
  })

  it('announces the check through a live region that is mounted before its text arrives', async () => {
    let answer!: (value: unknown) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { answer = resolve }) as never)
    const records: MutationRecord[] = []
    const observer = new MutationObserver((batch) => { records.push(...batch) })
    observer.observe(document.body, { childList: true, subtree: true, characterData: true })
    try {
      mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
      await flush()
      // One live region for the check: the visible rows no longer carry their own status role.
      const live = Array.from(document.body.querySelectorAll('[role="status"]')).filter((region) => /repositor/i.test(region.textContent ?? ''))
      expect(live).toHaveLength(1)
      expect(live[0].getAttribute('aria-live')).toBe('polite')
      // The text arrives as a change inside the already-mounted region, which screen readers announce.
      expect(records.some((record) => record.target === live[0] || record.target.parentNode === live[0])).toBe(true)
      expect(live[0].textContent).toBe('Checking repositories…')
      await act(async () => { answer({ ok: false, error: 'no', requestId: 'r' }); await Promise.resolve(); await Promise.resolve() })
      expect(live[0].isConnected).toBe(true)
      expect(live[0].textContent).toBe('Repository matching is unavailable right now.')
    } finally { observer.disconnect() }
  })

  it('rejoins a running check when repository matching turns off and on again', async () => {
    let answer!: (value: unknown) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { answer = resolve }) as never)
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    act(() => useAppStore.setState((state) => ({ executionRouting: { ...state.executionRouting, repositoryMatching: undefined } })))
    act(() => useAppStore.setState((state) => ({ executionRouting: { ...state.executionRouting, repositoryMatching: true } })))
    await flush()
    expect(sendToCEF).toHaveBeenCalledTimes(1)
    await act(async () => { answer({ ok: true, data: workspaces, requestId: 'r' }); await Promise.resolve(); await Promise.resolve() })
    expect(document.body.textContent).toContain('Same repository')
  })

  it('does not ask for matches when no local folder has a path to match', async () => {
    useAppStore.setState({ folders: [{ id: 'target', name: 'Remote checkout', directory: '/picked', executionHostId: 'ssh-a', parentId: null, isExpanded: true, createdAt: new Date() }] })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    expect(sendToCEF).not.toHaveBeenCalled()
    expect(document.body.textContent).not.toContain('Checking repositories')
    act(() => useAppStore.setState((state) => ({ folders: [...state.folders, { id: 'source', name: 'Source', directory: '/source', parentId: null, isExpanded: true, createdAt: new Date() }] })))
    await flush()
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'listRepositoryMatches', payload: {} })
  })

  it('decides which folder groups start open once matches arrive, and keeps a toggle the user made', async () => {
    let answer!: (value: unknown) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { answer = resolve }) as never)
    const local = (id: string) => ({ id, name: id.toUpperCase(), directory: `/${id}`, parentId: null, isExpanded: true, createdAt: new Date() })
    useAppStore.setState({
      folders: [local('source'), local('a'), local('b'), local('c'), { id: 'target', name: 'Remote checkout', directory: '/picked', executionHostId: 'ssh-a', parentId: null, isExpanded: true, createdAt: new Date() }],
      executionRouting: { enabled: true, repositoryMatching: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: ['a', 'b', 'c'].map((id) => ({ sourceFolderId: id, sourceDirectory: `/${id}`, destinationFolderId: 'target' })) }] },
    })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    const expanded = () => Object.fromEntries(Array.from(document.body.querySelectorAll<HTMLButtonElement>('.uam-rt-folders .uam-disclosure__summary')).map((button) => [button.querySelector('.uam-set-row__title')?.textContent, button.getAttribute('aria-expanded')]))
    // While checking, the first two of A, B, C are open.
    expect(expanded()).toMatchObject({ A: 'true', B: 'true', C: 'false' })
    // The user closes A before the check finishes.
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('.uam-rt-folders .uam-disclosure__summary')).find((button) => button.textContent?.includes('A'))!.click())
    await act(async () => { answer({ ok: true, data: workspaces, requestId: 'r' }); await Promise.resolve(); await Promise.resolve() })
    // Source's repository match now comes first, so B is third and closes; A keeps the user's choice.
    expect(expanded()).toMatchObject({ SOURCE: 'true', A: 'false', B: 'false', C: 'false' })
  })

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

  it('clears a saved override by removing it, since Save replaces the list, so the repository match applies again', async () => {
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
    expect(save).toHaveBeenCalledWith(expect.objectContaining({ hosts: [expect.objectContaining({ workspaceMappings: [] })] }))
  })

  it('offers the repository match, not Remove mapping, for a broken chosen override that has one', async () => {
    useAppStore.setState({
      executionRouting: { enabled: true, repositoryMatching: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'gone' }] }] },
    })
    mount(<ExecutionRoutingDialog onClose={vi.fn()} />)
    await flush()
    expect(document.body.querySelector('[data-problem]')?.textContent).toContain('Folder no longer exists')
    expect(document.body.querySelector('[aria-label="Remove mapping for Source on Server A"]')).toBeNull()
    act(() => document.body.querySelector<HTMLButtonElement>('[aria-label="Use repository match for Source on Server A"]')!.click())
    expect(document.body.querySelector('[data-problem]')).toBeNull()
    expect(document.body.querySelector('.uam-rt-map-row')?.textContent).toContain('Same repository')
    expect(buttonsNamed('Save routing')[0].disabled).toBe(false)
  })
})
