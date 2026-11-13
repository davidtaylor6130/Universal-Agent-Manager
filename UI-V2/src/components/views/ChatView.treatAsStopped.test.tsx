import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import type { ExecutionHost, Session } from '../../types/session'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import { acpBindingFromCppChat } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const companion = vi.hoisted(() => ({ on: false }))
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => companion.on }))

const UNCONFIRMED = 'Remote stop could not be confirmed; reconnecting to preserve the active turn.'
const remote: Session = {
  id: 'remote-chat', name: 'Remote job', providerId: 'codex-cli', viewMode: 'chat', folderId: null, messageCount: 1,
  createdAt: new Date('2026-11-11T09:00:00Z'), updatedAt: new Date('2026-11-11T09:00:00Z'),
  executionHostId: 'build-box', workspaceDirectory: '/srv/projects/api',
}
const buildBox = { id: 'build-box', label: 'Build box', transport: 'ssh', sshAlias: 'build-box' } as ExecutionHost
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>
const force = vi.fn()
const binding = (acp: Record<string, unknown>) => acpBindingFromCppChat({ id: remote.id, acpSession: sanitizeCppAcpSession({ lifecycleState: 'error', ...acp }) } as never, undefined)
const show = (acp: Record<string, unknown>) => act(() => useAppStore.setState({ acpBindingBySessionId: { [remote.id]: binding(acp) } }))
const button = (label: string, within: ParentNode = document.body) => [...within.querySelectorAll('button')].find(candidate => candidate.textContent === label)
const dialog = () => document.body.querySelector('[role="alertdialog"]')

beforeEach(() => {
  previous = useAppStore.getState()
  companion.on = false
  force.mockReset().mockResolvedValue(true)
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  vi.stubGlobal('requestAnimationFrame', () => 0)
  vi.stubGlobal('cancelAnimationFrame', () => undefined)
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  vi.stubGlobal('IntersectionObserver', class { observe() {} unobserve() {} disconnect() {} })
  useAppStore.setState({ sessions: [remote], providers: [fallbackProviderForId('codex-cli')], folders: [], executionHosts: [buildBox], activeSessionId: remote.id, messages: { [remote.id]: [] },
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, historyStartIndexBySessionId: { [remote.id]: 0 }, chatHistoryErrorBySessionId: {}, goalsByChatId: {}, activeGoalIdByChatId: {},
    loadSessionMessages: vi.fn().mockResolvedValue(undefined), unloadSessionMessages: vi.fn(), getVcsCommitStatus: vi.fn().mockResolvedValue(null), discoverProviderModels: vi.fn().mockResolvedValue(undefined),
    treatRemoteStopAsStopped: force })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  act(() => root.render(<ChatView session={remote} />))
})
afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  resetOutgoingPresentation()
  vi.unstubAllGlobals(); vi.restoreAllMocks()
})

describe('Treat as stopped', () => {
  it('is not offered while the stop has only just gone unconfirmed', () => {
    show({ lastError: UNCONFIRMED })
    expect(host.textContent).toContain(UNCONFIRMED)
    expect(button('Treat as stopped')).toBeUndefined()
  })

  it('is offered on the existing error notice once native reports the wait, and asks before doing anything', async () => {
    show({ lastError: UNCONFIRMED, remoteStopForceAvailable: true })
    const notice = host.querySelector('[role="alert"]') ?? host
    await act(async () => { button('Treat as stopped', notice)!.click() })
    expect(force).not.toHaveBeenCalled()
    // The warning names the computer and the folder a process may still be running in.
    expect(dialog()?.textContent).toContain('Build box')
    expect(dialog()?.textContent).toContain('/srv/projects/api')
    expect(dialog()?.textContent).toContain('may still be running')
    await act(async () => { button('Cancel', dialog()!)!.click() })
    expect(dialog()).toBeNull()
    expect(force).not.toHaveBeenCalled()
  })

  it('forces only on confirmation, for this chat', async () => {
    show({ lastError: UNCONFIRMED, remoteStopForceAvailable: true })
    await act(async () => { button('Treat as stopped')!.click() })
    await act(async () => { button('Treat as stopped', dialog()!)!.click(); await Promise.resolve() })
    expect(force).toHaveBeenCalledTimes(1)
    expect(force).toHaveBeenCalledWith(remote.id)
    expect(dialog()).toBeNull()
  })

  it('comes back on a dismissed notice and cannot be dismissed while offered, since the notice is the only place for it', async () => {
    show({ lastError: UNCONFIRMED })
    await act(async () => { (host.querySelector('[aria-label="Dismiss composer error"]') as HTMLButtonElement).click() })
    expect(host.textContent).not.toContain(UNCONFIRMED)
    show({ lastError: UNCONFIRMED, remoteStopForceAvailable: true })
    expect(button('Treat as stopped')).toBeDefined()
    expect(host.querySelector('[aria-label="Dismiss composer error"]')).toBeNull()
    // An ordinary error can be dismissed again once the stop is settled.
    show({ lastError: 'Another error.' })
    expect(host.querySelector('[aria-label="Dismiss composer error"]')).not.toBeNull()
  })

  it('still shows a notice to act on when the stop left no error text', () => {
    show({ lastError: '', remoteStopForceAvailable: true })
    expect(host.textContent).toContain('Remote stop could not be confirmed.')
    expect(button('Treat as stopped')).toBeDefined()
  })

  it('closes its confirmation when the host answers after all', async () => {
    show({ lastError: UNCONFIRMED, remoteStopForceAvailable: true })
    await act(async () => { button('Treat as stopped')!.click() })
    expect(dialog()).not.toBeNull()
    show({ lastError: '' })
    expect(dialog()).toBeNull()
  })

  it('is never offered on the phone', () => {
    companion.on = true
    show({ lastError: UNCONFIRMED, remoteStopForceAvailable: true })
    expect(button('Treat as stopped')).toBeUndefined()
  })
})
