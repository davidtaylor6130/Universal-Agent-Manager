import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionFromCppChat } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const caller: Session = {
  id: 'chat-create-caller', name: 'Planning chat', providerId: 'codex-cli', viewMode: 'chat', folderId: null,
  createdAt: new Date('2026-11-01T09:00:00Z'), updatedAt: new Date('2026-11-01T09:00:00Z'), messageCount: 0,
}
const started: Session = { ...caller, id: 'chat-create-started', name: 'Parser work', createdByChatId: caller.id }
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>

beforeEach(() => {
  previous = useAppStore.getState()
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  vi.stubGlobal('requestAnimationFrame', () => 0)
  vi.stubGlobal('cancelAnimationFrame', () => undefined)
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  vi.stubGlobal('IntersectionObserver', class { observe() {} unobserve() {} disconnect() {} })
  useAppStore.setState({ sessions: [caller, started], providers: [fallbackProviderForId('codex-cli')], folders: [],
    activeSessionId: started.id, messages: { [caller.id]: [], [started.id]: [] }, acpBindingBySessionId: {}, cliBindingBySessionId: {},
    historyStartIndexBySessionId: { [caller.id]: 0, [started.id]: 0 }, chatHistoryErrorBySessionId: {}, goalsByChatId: {}, activeGoalIdByChatId: {},
    loadSessionMessages: vi.fn().mockResolvedValue(undefined), unloadSessionMessages: vi.fn(),
    getVcsCommitStatus: vi.fn().mockResolvedValue(null), discoverProviderModels: vi.fn().mockResolvedValue(undefined) })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})

afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  vi.unstubAllGlobals(); vi.restoreAllMocks()
})

const provenance = () => host.querySelector('nav[aria-label="Chat provenance"]')

describe('chat_create provenance', () => {
  it('projects the native creator ID into the session', () => {
    const chat = sanitizeCppChat({ id: started.id, title: 'Parser work', providerId: 'codex-cli', createdByChatId: caller.id })!
    expect(sessionFromCppChat(chat, undefined, [{ id: 'codex-cli' }]).createdByChatId).toBe(caller.id)
    expect(sanitizeCppChat({ id: 'plain', title: 'Plain' })!.createdByChatId).toBeUndefined()
  })

  it('shows a Started by marker that opens the creating chat', async () => {
    await act(async () => root.render(<ChatView session={started} />))
    const button = provenance()?.querySelector('button')
    expect(button?.textContent).toBe('Started by Planning chat')
    await act(async () => button!.click())
    expect(useAppStore.getState().activeSessionId).toBe(caller.id)
  })

  it('keeps the marker without a link when the creating chat is gone, and omits it for ordinary chats', async () => {
    useAppStore.setState({ sessions: [started] })
    await act(async () => root.render(<ChatView session={started} />))
    expect(provenance()?.querySelector('button')).toBeNull()
    expect(provenance()?.textContent).toContain('Started by a chat that is no longer available')
    await act(async () => root.render(<ChatView session={caller} />))
    expect(provenance()).toBeNull()
  })
})
