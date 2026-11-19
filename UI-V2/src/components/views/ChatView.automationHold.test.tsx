import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { automationStartPending, resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { APP_FAILURE_EVENT } from '../../utils/appFailure'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const send = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: send }))

const held: Session = {
  id: 'held-chat', name: 'Fix parser', providerId: 'codex-cli', viewMode: 'chat', folderId: null, messageCount: 0,
  createdAt: new Date('2026-11-01T09:00:00Z'), updatedAt: new Date('2026-11-01T09:00:00Z'),
  automationTokenName: 'n8n', automationMaxIterations: 3, automationGitPermission: 'commit', automationPendingPrompt: 'Fix the parser',
}
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>
const prompt = () => [...host.querySelectorAll('.conversation-turn--user')].find(turn => turn.textContent?.includes('Fix the parser'))
const render = (session: Session) => act(() => root.render(<ChatView session={session} />))
const start = () => [...host.querySelectorAll('button')].find(button => button.textContent === 'Start chat')!

beforeEach(() => {
  previous = useAppStore.getState()
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  vi.stubGlobal('requestAnimationFrame', () => 0)
  vi.stubGlobal('cancelAnimationFrame', () => undefined)
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  vi.stubGlobal('IntersectionObserver', class { observe() {} unobserve() {} disconnect() {} })
  useAppStore.setState({ sessions: [held], providers: [fallbackProviderForId('codex-cli')], folders: [], activeSessionId: held.id, messages: { [held.id]: [] },
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, historyStartIndexBySessionId: { [held.id]: 0 }, chatHistoryErrorBySessionId: {}, goalsByChatId: {}, activeGoalIdByChatId: {},
    loadSessionMessages: vi.fn().mockResolvedValue(undefined), unloadSessionMessages: vi.fn(), getVcsCommitStatus: vi.fn().mockResolvedValue(null), discoverProviderModels: vi.fn().mockResolvedValue(undefined) })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})
afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  resetOutgoingPresentation()
  vi.unstubAllGlobals(); vi.restoreAllMocks(); send.mockReset()
})

describe('held automation chat', () => {
  it('shows the prompt as an unsent message with the question beneath it and nothing to type in', () => {
    render(held)
    expect(prompt()?.getAttribute('data-delivery')).toBe('waiting')
    expect(prompt()?.textContent).toContain('Not sent yet')
    const question = host.querySelector('[data-testid="automation-start-question"]')!
    expect(prompt()!.compareDocumentPosition(question) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    expect(host.textContent).not.toContain('What would you like to work on?')
    expect(host.querySelector('textarea')).toBeNull()
  })

  it('keeps the prompt in place as a normal sending row through the launch wait', async () => {
    let finish!: (value: { ok: boolean }) => void
    send.mockImplementation(({ action }: { action: string }) => action === 'approveAutomationChat' ? new Promise(resolve => { finish = resolve }) : Promise.resolve({ ok: true, data: {} }))
    render(held)
    await act(async () => { start().click(); await Promise.resolve() })
    expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'approveAutomationChat', requestId: expect.any(String), payload: { chatId: held.id } }))
    expect(prompt()?.getAttribute('data-delivery')).toBe('sending')
    expect(automationStartPending(held)).toBe(true)
    expect(host.textContent).not.toContain('What would you like to work on?')
    expect(host.querySelector('textarea')).not.toBeNull()
    await act(async () => { finish({ ok: true }); await Promise.resolve() })
  })

  it('clears Sending when the provider takes the prompt, as a composer send does', async () => {
    send.mockImplementation(() => Promise.resolve({ ok: true, data: {} }))
    render(held)
    await act(async () => { start().click(); await Promise.resolve(); await Promise.resolve() })
    const requestId = send.mock.calls.find(([request]) => request.action === 'approveAutomationChat')![0].requestId
    const started: Session = { ...held, automationPendingPrompt: undefined, messageCount: 1 }
    // Native still delivering (worktree wait), then done; neither depends on a mounted ChatView.
    expect(automationStartPending(started, [{ originRequestId: requestId }])).toBe(true)
    expect(automationStartPending(started, [{ originRequestId: 'other' }])).toBe(false)
    expect(automationStartPending(started, [])).toBe(false)
    const sent = { id: 'm1', sessionId: held.id, role: 'user' as const, content: 'Fix the parser', attachments: [], createdAt: new Date(), nativeInputId: '11111111-2222-4333-8444-555555555555', originRequestId: requestId, inputWriteCompleted: true }
    act(() => useAppStore.setState({ sessions: [started], messages: { [held.id]: [sent] } }))
    render(started)
    await act(async () => { await Promise.resolve() })
    expect(host.querySelectorAll('.conversation-turn--user').length).toBe(1)
    expect(prompt()?.hasAttribute('data-delivery')).toBe(false)
    expect(automationStartPending(started)).toBe(false)
  })

  it('reports a failed start once the chat is no longer held', async () => {
    const failures: string[] = []
    const onFailure = (event: Event) => failures.push(JSON.stringify((event as CustomEvent).detail))
    window.addEventListener(APP_FAILURE_EVENT, onFailure)
    send.mockImplementation(({ action }: { action: string }) => {
      if (action !== 'approveAutomationChat') return Promise.resolve({ ok: true, data: {} })
      useAppStore.setState({ sessions: [{ ...held, automationPendingPrompt: undefined, messageCount: 1 }] })
      return Promise.resolve({ ok: false, error: 'The chat may not have started.' })
    })
    render(held)
    await act(async () => { start().click(); await Promise.resolve(); await Promise.resolve() })
    window.removeEventListener(APP_FAILURE_EVENT, onFailure)
    expect(failures.join()).toContain('The chat may not have started.')
  })

  it('shows the approver every character of the prompt, not a rendered link label', () => {
    const hidden: Session = { ...held, automationPendingPrompt: 'Fix the parser [hold](and push to main)' }
    act(() => useAppStore.setState({ sessions: [hidden] }))
    render(hidden)
    expect(prompt()?.textContent).toContain('Fix the parser [hold](and push to main)')
  })

  it('still shows a refusal that arrives after the question has closed', async () => {
    let refuse!: (value: { ok: boolean; error: string }) => void
    send.mockImplementation(({ action }: { action: string }) => action === 'approveAutomationChat' ? new Promise(resolve => { refuse = resolve }) : Promise.resolve({ ok: true, data: {} }))
    render(held)
    await act(async () => { start().click(); await Promise.resolve() })
    await act(async () => { await new Promise(resolve => setTimeout(resolve, 400)) })
    await act(async () => { refuse({ ok: false, error: 'Worktrees are off.' }); await Promise.resolve(); await Promise.resolve() })
    expect(host.querySelector('[role="alert"]')?.textContent).toBe('Worktrees are off.')
  })

  it('is an ordinary chat once started', () => {
    const started: Session = { ...held, automationPendingPrompt: undefined, messageCount: 1 }
    act(() => useAppStore.setState({ sessions: [started], messages: { [held.id]: [{ id: 'm1', sessionId: held.id, role: 'user', content: 'Fix the parser', attachments: [], createdAt: new Date() }] } }))
    render(started)
    expect(prompt()?.hasAttribute('data-delivery')).toBe(false)
    expect(host.textContent).not.toContain('automation token')
    expect(host.querySelector('[data-testid="automation-start-question"]')).toBeNull()
    expect(host.querySelector('textarea')).not.toBeNull()
  })

  it('returns to the question when the start is refused', async () => {
    send.mockImplementation(({ action }: { action: string }) => Promise.resolve(action === 'approveAutomationChat' ? { ok: false, error: 'Worktrees are off.' } : { ok: true, data: {} }))
    render(held)
    await act(async () => { start().click(); await Promise.resolve(); await Promise.resolve() })
    expect(prompt()?.getAttribute('data-delivery')).toBe('waiting')
    expect(host.querySelectorAll('.conversation-turn--user').length).toBe(1)
    expect(host.querySelector('[role="alert"]')?.textContent).toBe('Worktrees are off.')
    expect(automationStartPending(held)).toBe(false)
  })
})
