import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type AcpAttentionKind, type AcpBinding } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { SessionItem } from './SessionItem'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const now = new Date('2026-01-01T12:00:00.000Z')

function makeSession(): Session {
  return {
    id: 'chat-1',
    name: 'Chat 1',
    viewMode: 'chat',
    folderId: 'project',
    createdAt: now,
    updatedAt: now,
    lastOpenedAt: now,
  }
}

function makeAcpBinding(overrides: Partial<AcpBinding> = {}): AcpBinding {
  return {
    sessionId: 'native-1',
    providerId: 'gemini-cli',
    protocolKind: 'gemini-acp',
    threadId: '',
    running: true,
    lifecycleState: 'ready',
    processing: false,
    readySinceLastSelect: false,
    attentionKind: null,
    processingStartedAtMs: null,
    lastError: '',
    recentStderr: '',
    lastExitCode: null,
    diagnostics: [],
    toolCalls: [],
    planSummary: '',
    planEntries: [],
    availableModes: [],
    currentModeId: 'default',
    availableModels: [],
    currentModelId: '',
    turnEvents: [],
    turnUserMessageIndex: -1,
    turnAssistantMessageIndex: -1,
    turnSerial: 0,
    pendingPermission: null,
    pendingUserInput: null,
    agentInfo: null,
    ...overrides,
  }
}

function renderSessionItem() {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<SessionItem sessionId="chat-1" />)
  })

  return { host, root }
}

describe('SessionItem status icons', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState({
      folders: [],
      sessions: [makeSession()],
      activeSessionId: 'chat-2',
      messages: {},
      cliBindingBySessionId: {},
      acpBindingBySessionId: {},
      cliTranscriptBySessionId: {},
      isNewChatModalOpen: false,
      newChatFolderId: null,
    })
  })

  it.each([
    ['question' as AcpAttentionKind, 'Needs answer'],
    ['plan' as AcpAttentionKind, 'Plan needs review'],
    ['memory' as AcpAttentionKind, 'Memory input needed'],
    ['permission' as AcpAttentionKind, 'Permission needed'],
    ['command' as AcpAttentionKind, 'Command approval needed'],
    ['file' as AcpAttentionKind, 'File approval needed'],
    ['error' as AcpAttentionKind, 'Needs attention'],
  ])('renders %s attention before generic processing', (attentionKind, label) => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({
          lifecycleState: 'waitingUserInput',
          processing: true,
          attentionKind,
        }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector(`[aria-label="${label}"]`)).toBeTruthy()
    expect(host.querySelector('[aria-label="Gemini running"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the spinner for processing without a user attention kind', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ lifecycleState: 'processing', processing: true }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('[aria-label="Gemini running"]')).toBeTruthy()
    expect(host.querySelector('[aria-label="Done"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the dot for done chats without active attention', () => {
    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': makeAcpBinding({ readySinceLastSelect: true }),
      },
    })

    const { host, root } = renderSessionItem()

    expect(host.querySelector('[aria-label="Done"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows its pane colour and can move the chat from the context menu', () => {
    writeChatGridLayout({
      ...defaultChatGridLayout,
      paneCount: 2,
      activePane: 0,
      sessionIds: ['chat-1', 'chat-2'],
    })
    useAppStore.setState({ activeSessionId: 'chat-1' })
    const { host, root } = renderSessionItem()

    expect(host.querySelector('[aria-label^="Chat shown in pane"]')).toBeNull()
    const sessionRow = host.querySelector('.cursor-pointer') as HTMLElement
    expect(sessionRow.style.borderLeft).toContain('rgb(249, 115, 22)')
    act(() => sessionRow.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const paneTwo = host.querySelector('button[aria-label="Show Chat 1 in pane 2"]') as HTMLButtonElement
    expect(paneTwo).toBeTruthy()
    expect(paneTwo.textContent).toBe('')
    act(() => paneTwo.click())

    expect(readChatGridLayout().activePane).toBe(1)
    expect(readChatGridLayout().sessionIds).toEqual(['chat-2', 'chat-1'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
    expect(sessionRow.style.borderLeft).toContain('rgb(236, 72, 153)')

    act(() => root.unmount())
    host.remove()
  })
})
