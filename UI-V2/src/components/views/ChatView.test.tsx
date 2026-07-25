import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

// Plan / Accept-Edits / Auto / Memory now live inside the composer "Options" popover.
function openComposerOptions(host: HTMLElement) {
  act(() => {
    (host.querySelector('button[aria-label="Options"]') as HTMLButtonElement | null)
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
}

function openWorkspaceActions(host: HTMLElement) {
  act(() => {
    (host.querySelector('button[aria-label="Workspace actions"]') as HTMLButtonElement | null)
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  const menus = Array.from(document.body.querySelectorAll<HTMLElement>('[role="menu"][aria-label="Workspace actions"]'))
  return menus[menus.length - 1] ?? null
}

describe('ChatView', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      workingDisplayMode: 'verbose',
      folders: [
        {
          id: 'default',
          name: 'Project',
          parentId: null,
          directory: '/tmp/project',
          isExpanded: true,
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          workspaceDirectory: '/tmp/project',
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
          updatedAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      activeSessionId: 'chat-1',
      messages: {
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Please inspect the workspace',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: 'Before tool. After tool.',
            thoughts: 'Persisted thought should not duplicate while turn events are active.',
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' },
        { id: 'claude-cli', name: 'Claude Code', shortName: 'Claude', color: '#7c3aed', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'claude-code-stream-json' },
      ],
      acpBindingBySessionId: {
        'chat-1': {
          sessionId: 'native-1',
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          threadId: '',
          running: true,
          lifecycleState: 'waitingPermission',
          processing: true,
          readySinceLastSelect: false,
            processingStartedAtMs: Date.now(),
            lastError: '',
            recentStderr: '',
            lastExitCode: null,
            diagnostics: [],
            toolCalls: [
            {
              id: 'tool-1',
              title: 'Search symbols',
              kind: 'search',
              status: 'in_progress',
              content: 'Searching workspace symbols',
            },
          ],
          planEntries: [],
          availableModes: [
            { id: 'default', name: 'Default', description: 'Run normally' },
            { id: 'plan', name: 'Plan', description: 'Plan before editing' },
          ],
          currentModeId: 'default',
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Gemini 3 routing' },
            { id: 'gemini-3-flash-preview', name: 'Gemini 3 Flash', description: 'Preview model' },
          ],
          currentModelId: 'auto-gemini-3',
          turnEvents: [
            { type: 'assistant_text', text: 'Before **tool**.\n\n```ts\nconst ok = true\n```' },
            { type: 'thought', text: 'Need to inspect the workspace first.' },
            { type: 'tool_call', toolCallId: 'tool-1' },
            { type: 'permission_request', requestId: '5', toolCallId: 'tool-1' },
            { type: 'assistant_text', text: 'After tool.' },
          ],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 1,
          pendingPermission: {
            requestId: '5',
            toolCallId: 'tool-1',
            title: 'Read file',
            kind: 'read',
            status: 'pending',
            content: 'Read /tmp/project/file.txt',
            safetyRisk: 'warn',
            safetyTier: 'low',
            safetyRequiresApproval: true,
            options: [{ id: 'allow-once', name: 'Allow once', kind: 'allow_once' }],
          },
          pendingUserInput: null,
          agentInfo: { name: 'gemini', title: 'Gemini CLI', version: '0.36.0' },
        },
      },
      cliBindingBySessionId: {},
    })
  })

  it('uses the persisted assistant duration when a completed turn timeline stays visible', () => {
    useAppStore.setState((state) => ({
      workingDisplayMode: 'compact',
      messages: {
        ...state.messages,
        'chat-1': state.messages['chat-1'].map((message) =>
          message.role === 'assistant' ? { ...message, processingTimeMs: 83_000 } : message
        ),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toContain('Worked for 1m 23s')
    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).not.toContain('Worked for 0s')

    act(() => root.unmount())
    host.remove()
  })

  it('uses the composer action as Stop while the runtime is processing', async () => {
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      stopAcpSession,
      sessions: state.sessions.map((session) => ({ ...session, commandSafetyTier: 'off' as const })),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const stopButton = Array.from(host.querySelectorAll('button')).find((button) => button.title === 'Stop runtime') as HTMLButtonElement
    expect(stopButton).toBeTruthy()
    expect(host.querySelector('[data-mode-chip="Permissions: Default"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Permissions: Default"]')).toBeNull()
    expect(host.querySelector('.uam-composer-toolbar .uam-composer-status-chips [data-mode-chip="Permissions: Default"]')).toBeTruthy()
    expect(host.querySelector('.uam-composer-surface')).toBeTruthy()
    expect(host.querySelector('[data-mode-chip="Permissions: Default"] .uam-mode-chip__label--compact')?.textContent).toBe('Default')
    expect(host.querySelector('[aria-label="Chat settings"]')).toBeNull()
    expect(stopButton.classList.contains('shrink-0')).toBe(true)
    expect(host.textContent).not.toContain('Working ')

    await act(async () => {
      stopButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(stopAcpSession).toHaveBeenCalledWith('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows one quiet startup indicator until the first turn event arrives', async () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          turnEvents: [],
          turnAssistantMessageIndex: -1,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelectorAll('[data-testid="turn-starting"]')).toHaveLength(1)
    expect(host.querySelector('[data-testid="turn-starting"]')?.textContent).toContain('Starting')

    await act(async () => {
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            turnEvents: [{ type: 'assistant_text', text: 'First token' }],
          },
        },
      }))
      await Promise.resolve()
    })
    expect(host.querySelector('[data-testid="turn-starting"]')).toBeNull()
    expect(host.textContent).toContain('First token')

    act(() => root.unmount())
    host.remove()
  })

  it('clears local steering state if a queued steer request does not immediately start a new turn', async () => {
    const steerQueuedAcpPrompt = vi.fn(async () => true)
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const sessionId = useAppStore.getState().sessions[0].id

    useAppStore.setState((state) => ({
      steerQueuedAcpPrompt,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        [sessionId]: {
          ...state.acpBindingBySessionId[sessionId],
          turnSerial: 3,
          processing: true,
          turnEvents: [{ type: 'assistant_text', text: 'Streaming now.' }],
          queuedPrompts: [{
            text: 'Re-evaluate this first.',
            markdownStoreFiles: [],
            attachments: [],
            goalMode: false,
            goalId: '',
            prioritySteer: true,
          }],
        },
      },
    }))
    const steerButtonSelector = 'button[aria-label="Steer with this prompt now"]'

    vi.useFakeTimers()
    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
      await Promise.resolve()
    })

    const steerButton = host.querySelector(steerButtonSelector) as HTMLButtonElement
    expect(steerButton).toBeTruthy()
    act(() => {
      steerButton.click()
    })
    expect(steerQueuedAcpPrompt).toHaveBeenCalledWith(sessionId, 0)
    expect(steerButton.disabled).toBe(true)

    await act(async () => {
      vi.advanceTimersByTime(6000)
      await Promise.resolve()
    })

    const clearedSteerButton = host.querySelector(steerButtonSelector) as HTMLButtonElement
    expect(clearedSteerButton.disabled).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
    vi.useRealTimers()
  })

  it('combines provider and model controls and folds workspace actions into the composer', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('.uam-workspace-row')).toBeNull()
    expect(host.querySelector('button[aria-label="Select provider"]')).toBeNull()
    expect(host.querySelector('.uam-composer-toolbar button[aria-label="Workspace actions"]')).toBeTruthy()

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector).toBeTruthy()
    expect(selector.querySelector('svg')).toBeTruthy()
    act(() => selector.click())

    const menu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(menu.textContent).toContain('Provider')
    expect(menu.textContent).toContain('Model')
    expect(menu.textContent).toContain('Gemini')
    expect(menu.textContent).toContain('Auto 3')

    act(() => root.unmount())
    host.remove()
  })

  it('queues a draft during an active turn and keeps it when sending fails', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(false))
    useAppStore.setState({ sendAcpPrompt })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Change direction now')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const queue = host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement
    expect(queue).toBeTruthy()
    await act(async () => { queue.click(); await Promise.resolve() })

    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Change direction now', [])
    expect(textarea.value).toBe('Change direction now')
    expect(host.querySelector('button[aria-label="Steering prompt"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('submits a prompt only once before the submitting state rerenders', async () => {
    const finishSends: Array<(ok: boolean) => void> = []
    const sendAcpPrompt = vi.fn(() => new Promise<boolean>((resolve) => finishSends.push(resolve)))
    useAppStore.setState({ sendAcpPrompt })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Queue only once')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const queue = host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement
    act(() => {
      queue.click()
      queue.click()
    })

    expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishSends.forEach((finish) => finish(false))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })

  it('does not let a send finishing in another chat clear the current draft', async () => {
    let finishSend: (ok: boolean) => void = () => {}
    const sendAcpPrompt = vi.fn(() => new Promise<boolean>((resolve) => { finishSend = resolve }))
    useAppStore.setState((state) => ({
      sendAcpPrompt,
      sessions: [
        state.sessions[0],
        {
          ...state.sessions[0],
          id: 'chat-2',
          name: 'Second chat',
        },
      ],
      messages: { ...state.messages, 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': {
          ...state.acpBindingBySessionId['chat-1'],
          sessionId: 'native-2',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    let textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'First chat prompt')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => (host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement).click())

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[1]} />))
    textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Second chat draft')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      finishSend(true)
      await Promise.resolve()
    })

    expect(textarea.value).toBe('Second chat draft')

    act(() => root.unmount())
    host.remove()
  })

  it('shows one combined queued prompt in a constrained composer panel', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          queuedPrompts: [
            { text: 'First queued prompt\n\nSecond queued prompt', markdownStoreFiles: ['/tmp/review.uam'], attachments: [], goalMode: false, goalId: '' },
          ],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const queue = host.querySelector('[aria-label="Queued prompt"]') as HTMLElement
    expect(queue.textContent?.indexOf('First queued prompt')).toBeLessThan(queue.textContent?.indexOf('Second queued prompt') ?? 0)
    expect(queue.textContent).toContain('1 attachment')
    expect(queue.querySelectorAll('button[aria-label="Steer with this prompt now"]')).toHaveLength(1)
    expect(queue.querySelectorAll('button[aria-label="Remove queued prompt"]')).toHaveLength(1)

    act(() => root.unmount())
    host.remove()
  })

  it('copies, edits, and reverts messages with icon actions and branch-safe requests', async () => {
    const branchFromMessage = vi.fn(() => Promise.resolve('branch-1'))
    useAppStore.setState((state) => ({
      branchFromMessage,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          pendingUserInput: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const copyButton = host.querySelector('button[aria-label="Copy message"]')
    expect(copyButton?.querySelector('svg')).toBeTruthy()
    const editButton = host.querySelector('button[aria-label="Edit message in new branch"]')
    const revertButton = host.querySelector('button[aria-label="Revert to message in new branch"]')
    expect(editButton).toBeTruthy()
    expect(revertButton).toBeTruthy()
    expect(editButton?.parentElement?.classList.contains('uam-message-frame__actions')).toBe(true)

    act(() => editButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    const editTextarea = host.querySelector('textarea[aria-label="Edit message"]') as HTMLTextAreaElement | null
    expect(editTextarea?.value).toBe('Please inspect the workspace')
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editTextarea, 'Inspect only the tests')
      editTextarea?.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const saveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save to new branch')
    await act(async () => saveButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 0, 'Inspect only the tests')

    await act(async () => revertButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 0, undefined)

    act(() => root.unmount())
    host.remove()
  })

  it('marks the edited or reverted branch point', () => {
    const session = { ...useAppStore.getState().sessions[0], branchFromMessageIndex: 0, branchMessageEdited: true }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={session} />))
    expect(host.textContent).toContain('Edited branch')
    act(() => root.render(<ChatView session={{ ...session, branchMessageEdited: false }} />))
    expect(host.textContent).toContain('Reverted branch')
    act(() => root.unmount())
    host.remove()
  })

  it('uses the composer action as Send when the runtime is idle', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const sendButton = Array.from(host.querySelectorAll('button')).find((button) => button.title === 'Send prompt') as HTMLButtonElement
    expect(sendButton).toBeTruthy()
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.title === 'Cancel turn' && button.textContent === 'Stop')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ACP messages and resolves permission choices', () => {
    const resolveAcpPermission = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ resolveAcpPermission })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Before tool.')
    expect(host.textContent).toContain('const ok = true')
    expect(host.textContent).not.toContain('Tool call:')
    expect(host.textContent).toContain('Search symbols')
    expect(host.textContent).toContain('Thinking')
    expect(host.textContent).toContain('Need to inspect the workspace first.')
    expect(host.textContent).not.toContain('Persisted thought should not duplicate while turn events are active.')
    expect(host.textContent).toContain('After tool.')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).not.toContain('ACP')
    expect(host.querySelector('button[aria-label="Select provider and model"]')).toBeTruthy()
    expect(host.querySelector('.uam-workspace-row')).toBeNull()
    expect(host.textContent).not.toContain('Tools on')
    expect(host.textContent).toContain('Read file')

    const streamText = host.textContent ?? ''
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Search symbols'))
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Thinking'))
    expect(streamText.indexOf('Thinking')).toBeLessThan(streamText.indexOf('Search symbols'))
    expect(streamText.indexOf('Search symbols')).toBeLessThan(streamText.indexOf('Read file'))
    expect(streamText.indexOf('Read file')).toBeLessThan(streamText.indexOf('After tool.'))
    const thinkingBlock = host.querySelector('[data-testid="thinking-block"]') as HTMLDetailsElement | null
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)
    expect(thinkingBlock?.tagName).toBe('DETAILS')
    expect(thinkingBlock?.textContent).toContain('Thinking')
    expect(thinkingBlock?.textContent).toContain('Need to inspect the workspace first.')
    expect(thinkingBlock?.hasAttribute('open')).toBe(false)
    expect(thinkingBlock?.dataset.active).toBe('false')
    expect(host.querySelector('.uam-tool-row')?.getAttribute('data-active')).toBe('true')
    expect(host.querySelector('.uam-tool-row__kind')?.textContent).toBe('Tool')
    expect(host.querySelector('.uam-message-frame.is-streaming')).not.toBeNull()
    expect(host.querySelector('.uam-runtime-status')).toBeNull()
    expect((host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).title).toContain('Permission')

    openComposerOptions(host)
    expect(document.body.textContent).toContain('Goal token budget')
    expect(host.textContent).not.toContain('Unavailable')

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Search symbols')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Searching workspace symbols')
    expect(document.body.querySelector('[role="dialog"]')?.parentElement?.className).toContain('fixed')
    expect(document.body.querySelector('[role="dialog"]')?.parentElement?.parentElement).toBe(document.body)

    const closeToolButton = document.body.querySelector('button[aria-label="Close tool details"]')
    expect(closeToolButton).toBeTruthy()
    act(() => {
      closeToolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).not.toContain('Searching workspace symbols')

    const allowButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Allow once')
    )
    expect(allowButton).toBeTruthy()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Command safety warning (low tier)')

    act(() => {
      allowButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(resolveAcpPermission).toHaveBeenCalledWith('chat-1', '5', 'allow-once')

    act(() => {
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            pendingPermission: null,
          },
        },
      }))
    })
    expect(host.textContent).not.toContain('Working')
    expect(host.querySelector('button[title="Cancel turn"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('deduplicates permission actions and clarifies matching labels', () => {
    const resolveAcpPermission = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      resolveAcpPermission,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          pendingPermission: {
            requestId: '5',
            toolCallId: 'tool-1',
            title: 'Command approval',
            kind: 'commandExecution',
            status: 'pending',
            content: 'npm test',
            options: [
              { id: 'accept', name: 'Allow', kind: 'decision' },
              { id: 'accept', name: 'Allow', kind: 'decision' },
              { id: 'acceptForSession', name: 'Allow', kind: 'decision' },
              { id: 'decline', name: 'Deny', kind: 'decision' },
              { id: 'cancelled', name: 'Cancel', kind: 'cancel' },
              { id: 'cancelled', name: 'Cancel', kind: 'cancel' },
              { id: 'cancel', name: 'Cancel', kind: 'cancel' },
            ],
          },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const buttonTexts = Array.from(host.querySelectorAll('button')).map((button) => button.textContent?.trim() ?? '')
    expect(buttonTexts.filter((text) => text === 'Cancel')).toHaveLength(1)
    expect(buttonTexts.filter((text) => text === 'Allow')).toHaveLength(0)
    expect(buttonTexts).toContain('Allow (accept)')
    expect(buttonTexts).toContain('Allow (acceptForSession)')
    expect(buttonTexts).toContain('Deny')

    const cancelButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')
    expect(cancelButton).toBeTruthy()
    act(() => {
      cancelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(resolveAcpPermission).toHaveBeenCalledWith('chat-1', '5', 'cancelled')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('flags stale permission waits and exposes recovery actions', async () => {
    const cancelAcpTurn = vi.fn(() => Promise.resolve(true))
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      cancelAcpTurn,
      stopAcpSession,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          waitIsStale: true,
          waitStaleReason: 'No runtime activity while waiting for command or tool approval.',
          waitSeconds: 143,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.querySelector('[data-testid="stale-wait-warning"]')).toBeTruthy()
    expect(host.textContent).toContain('This approval has not had runtime activity for 143s.')
    expect(host.textContent).toContain('No runtime activity while waiting for command or tool approval.')

    const cancelButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Cancel turn')
    const stopButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Stop runtime')
    expect(cancelButton).toBeTruthy()
    expect(stopButton).toBeTruthy()

    await act(async () => {
      cancelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(cancelAcpTurn).toHaveBeenCalledWith('chat-1')

    await act(async () => {
      stopButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(stopAcpSession).toHaveBeenCalledWith('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders Codex user-input questions and submits answers', async () => {
    const resolveAcpUserInput = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      resolveAcpUserInput,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'waitingUserInput',
          processing: true,
          turnEvents: [
            { type: 'assistant_text', text: 'I need one detail.' },
            { type: 'user_input_request', requestId: '11', toolCallId: 'input-1' },
          ],
          pendingPermission: null,
          pendingUserInput: {
            requestId: '11',
            itemId: 'input-1',
            status: 'pending',
            questions: [
              {
                id: 'scope',
                header: 'Scope',
                question: 'Which scope?',
                isOther: false,
                isSecret: false,
                options: [
                  { label: 'Focused', description: 'Only the bug' },
                  { label: 'Broad', description: 'Include cleanup' },
                ],
              },
              {
                id: 'note',
                header: 'Note',
                question: 'Any extra detail?',
                isOther: true,
                isSecret: false,
                options: [],
              },
            ],
          },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Codex needs input')
    expect(host.textContent).toContain('Which scope?')
    expect(host.textContent).toContain('Focused')
    expect(host.textContent).toContain('Any extra detail?')

    const focusedButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Focused'))
    const noteInput = host.querySelector('input[aria-label="Any extra detail?"]') as HTMLInputElement | null
    const submitButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Submit')
    expect(focusedButton).toBeTruthy()
    expect(noteInput).toBeTruthy()
    expect(submitButton).toBeTruthy()

    await act(async () => {
      focusedButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      if (noteInput) {
        const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value')?.set
        setter?.call(noteInput, 'Extra context')
        noteInput.dispatchEvent(new Event('input', { bubbles: true }))
      }
      await Promise.resolve()
    })

    const enabledSubmitButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Submit')
    expect((enabledSubmitButton as HTMLButtonElement | undefined)?.disabled).toBe(false)

    await act(async () => {
      enabledSubmitButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(resolveAcpUserInput).toHaveBeenCalledWith('chat-1', '11', {
      scope: ['Focused'],
      note: ['Extra context'],
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders Codex provider labels from backend provider metadata', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          threadId: 'thread-1',
          sessionId: 'thread-1',
          availableModels: [],
          currentModelId: '',
          agentInfo: { name: 'codex', title: 'Codex', version: '1.0.0' },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Codex')
    expect(host.textContent).not.toContain('App Server')
    expect(host.querySelector('button[aria-label="Select provider and model"]')?.textContent).toContain('Default')
    expect(host.querySelector('textarea')?.getAttribute('placeholder')).toBe('Message Codex')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the provider label captured on a historical assistant message', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      messages: {
        'chat-1': [{
          id: 'historical-gemini',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Historical Gemini answer',
          providerId: 'gemini-cli',
          createdAt: new Date('2026-01-01T00:00:01.000Z'),
        }],
      },
      acpBindingBySessionId: {},
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const messageFrame = Array.from(host.querySelectorAll('article'))
      .find((article) => article.textContent?.includes('Historical Gemini answer'))
    expect(messageFrame?.textContent).toContain('Gemini')
    expect(messageFrame?.textContent).not.toContain('Codex')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps agent mode separate from permissions and exposes safety only for Auto Decide', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, approvalMode: 'default', commandSafetyTier: 'medium' as const })),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          currentModeId: 'default',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    openComposerOptions(host)

    const agent = document.body.querySelector('button[aria-label="Agent mode"]') as HTMLButtonElement
    expect(agent.textContent).toContain('Default')
    act(() => agent.click())
    const plan = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Plan')) as HTMLButtonElement
    await act(async () => { plan.click(); await Promise.resolve() })
    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'plan')

    const permission = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    expect(permission.textContent).toContain('Auto Decide')
    act(() => permission.click())
    const yolo = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('YOLO')) as HTMLButtonElement
    await act(async () => { yolo.click(); await Promise.resolve() })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')

    const safety = document.body.querySelector('button[aria-label="Auto Decide safety"]') as HTMLButtonElement
    expect(safety.textContent).toContain('Medium')
    expect(host.querySelector('select')).toBeNull()
    act(() => safety.click())
    const low = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Low')) as HTMLButtonElement
    await act(async () => { low.click(); await Promise.resolve() })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'low')

    const optionsButton = host.querySelector('button[aria-label="Options"]') as HTMLButtonElement
    if (optionsButton.getAttribute('aria-expanded') === 'true') {
      act(() => optionsButton.click())
    }
    expect(document.body.querySelector('button[aria-label="Permissions"]')).toBeNull()
    expect(document.body.querySelector('button[aria-label="Auto Decide safety"]')).toBeNull()

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }
    await act(async () => {
      setDraft('/safety high')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'high')
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Command safety changed to High.')

    await act(async () => {
      setDraft('/safety unrestricted')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledTimes(3)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Unsupported command safety tier "unrestricted"')

    act(() => root.unmount())
    host.remove()
  })

  it('discovers, inspects, validates, and applies permission slash commands', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'default',
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    act(() => setDraft('/p'))
    expect(document.body.querySelector('[aria-label="Slash commands"]')?.textContent).toContain('/permission')

    await act(async () => {
      setDraft('/permission')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    const permissionMenu = document.body.querySelector('[aria-label="Permission modes"]')
    expect(textarea.value).toBe('')
    expect(permissionMenu?.textContent).toContain('Default')
    expect(permissionMenu?.textContent).not.toContain('Plan')
    expect(permissionMenu?.textContent).toContain('YOLO')
    expect(permissionMenu?.textContent).toContain('Auto Decide')
    expect(permissionMenu?.querySelector('svg')).not.toBeNull()
    expect(textarea.parentElement?.querySelector('[role="status"]')).toBeNull()
    expect(setSessionApprovalMode).not.toHaveBeenCalled()

    const slashAutoDecide = Array.from(permissionMenu?.querySelectorAll('[role="option"]') ?? []).find((option) => option.textContent?.includes('Auto Decide')) as HTMLButtonElement
    await act(async () => { slashAutoDecide.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })) })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'medium')
    expect(host.textContent).not.toContain('Permission mode changed')

    await act(async () => {
      setDraft('/permission yolo')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')
    expect(host.textContent).not.toContain('Permission mode changed')

    await act(async () => {
      setDraft('/permission unrestricted')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    expect(setSessionApprovalMode).not.toHaveBeenCalled()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Unsupported permission mode "unrestricted"')

    act(() => root.unmount())
    host.remove()
  })

  it('always offers /skills and exposes only favorite collision-safe skill commands', async () => {
    const attachMarkdownStoreEntry = vi.fn()
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'one', title: 'Review', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, sourceProvider: 'codex', commandName: 'review', filePath: '/tmp/one.uam' },
        { id: 'two', title: 'Review', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, sourceProvider: 'gemini-cli', commandName: 'review-2', filePath: '/tmp/two.uam' },
        { id: 'three', title: 'Hidden', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: false, sourceProvider: 'claude-code', commandName: 'hidden', filePath: '/tmp/three.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      attachMarkdownStoreEntry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const palette = document.body.querySelector('[aria-label="Slash commands"]') as HTMLElement | null
    expect(palette?.style.position).toBe('absolute')
    expect(palette?.style.left).toBe('12px')
    expect(palette?.style.right).toBe('12px')
    expect(palette?.parentElement).toBe(textarea.previousElementSibling)
    expect(palette?.textContent).toContain('/skills')
    expect(palette?.textContent).toContain('/review')
    expect(palette?.textContent).toContain('/review-2')
    expect(palette?.textContent).toContain('codex')
    expect(palette?.textContent).toContain('gemini-cli')
    expect(palette?.textContent).not.toContain('/hidden')

    const reviewTwo = Array.from(palette?.querySelectorAll('[role="option"]') ?? []).find((option) => option.textContent?.includes('/review-2')) as HTMLElement
    act(() => reviewTwo.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'two' }))

    act(() => root.unmount())
    host.remove()
  })

  it('groups favorite Markdown Store skills in a submenu', async () => {
    const attachMarkdownStoreEntry = vi.fn()
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'one', title: 'Review', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'review', group: 'Workflows', filePath: '/tmp/one.uam' },
        { id: 'two', title: 'Release', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'release', group: 'Workflows', filePath: '/tmp/two.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      attachMarkdownStoreEntry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const group = Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).find((option) => option.textContent?.includes('/workflows')) as HTMLElement
    expect(group.textContent).toContain('/workflows')
    expect(group.textContent).not.toContain('/review')
    act(() => group.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    const submenu = document.body.querySelector('[aria-label="Workflows skills"]') as HTMLElement
    expect(submenu.textContent).toContain('/review')
    expect(submenu.textContent).toContain('/release')
    const review = Array.from(submenu.querySelectorAll('[role="menuitem"]')).find((item) => item.textContent?.includes('/review')) as HTMLElement
    act(() => review.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'one' }))
    act(() => root.unmount())
    host.remove()
  })

  it('keeps Codex reasoning and speed in the + menu and always shows the reasoning effort', async () => {
    const setSessionCodexOptions = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4', reasoningEffort: 'low', serviceTier: 'flex' }
        : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [{
            id: 'gpt-5.4',
            name: 'gpt-5.4',
            description: 'Latest Codex model.',
            supportedReasoningEfforts: ['low', 'high', 'xhigh'],
            additionalSpeedTiers: ['fast', 'flex'],
          }],
          currentModelId: 'gpt-5.4',
        },
      },
      setSessionCodexOptions,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('button[title="Select Codex reasoning"]')).toBeNull()
    expect(host.querySelector('button[title="Select Codex speed"]')).toBeNull()
    expect(host.querySelector('[data-mode-chip="Reasoning: Low"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Reasoning: Low"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Disable Reasoning: Low"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Disable Speed: Flex"]')).toBeTruthy()
    openComposerOptions(host)

    const reasoning = document.body.querySelector('button[aria-label="Reasoning"]') as HTMLButtonElement
    const speed = document.body.querySelector('button[aria-label="Speed"]') as HTMLButtonElement
    expect(reasoning.textContent).toContain('Low')
    expect(speed.textContent).toContain('Flex')

    act(() => reasoning.click())
    const high = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('High')) as HTMLButtonElement
    await act(async () => { high.click(); await Promise.resolve() })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(1, 'chat-1', { reasoningEffort: 'high' })

    act(() => speed.click())
    const fast = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('Fast')) as HTMLButtonElement
    await act(async () => { fast.click(); await Promise.resolve() })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(2, 'chat-1', { serviceTier: 'fast' })

    openComposerOptions(host)
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    await act(async () => {
      setDraft('/reasoning xhigh')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(3, 'chat-1', { reasoningEffort: 'xhigh' })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Reasoning changed to XHigh.')

    await act(async () => {
      setDraft('/speed flex')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(4, 'chat-1', { serviceTier: 'flex' })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Speed changed to Flex.')

    act(() => root.unmount())
    host.remove()
  })

  it('switches persisted edit and resend branches inside one chat', () => {
    const setActiveSession = vi.fn()
    useAppStore.setState((state) => ({
      sessions: [
        { ...state.sessions[0], id: 'chat-root', branchRootChatId: 'chat-root' },
        {
          ...state.sessions[0],
          id: 'chat-branch',
          name: 'Edited branch',
          parentChatId: 'chat-root',
          branchRootChatId: 'chat-root',
          branchFromMessageIndex: 0,
          branchMessageEdited: true,
          createdAt: new Date('2026-01-01T00:01:00.000Z'),
        },
        {
          ...state.sessions[0],
          id: 'chat-branch-2',
          name: 'Resent branch',
          parentChatId: 'chat-root',
          branchRootChatId: 'chat-root',
          branchFromMessageIndex: 0,
          branchMessageEdited: false,
          createdAt: new Date('2026-01-01T00:02:00.000Z'),
        },
      ],
      messages: {
        'chat-root': state.messages['chat-1'],
        'chat-branch': state.messages['chat-1'],
        'chat-branch-2': state.messages['chat-1'],
      },
      activeSessionId: 'chat-branch',
      setActiveSession,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[1]} />))

    expect(host.querySelector('[aria-label="Chat branches"]')).toBeNull()
    const branchPoint = host.querySelector('[data-message-kind="user"]')
    const navigation = branchPoint?.querySelector('[aria-label="Message branches"]')
    expect(navigation?.textContent).toContain('2 / 3')
    expect(navigation?.classList.contains('uam-message-frame__actions')).toBe(true)
    expect(branchPoint?.querySelector('button[aria-label="Edit message in new branch"]')).toBeTruthy()
    expect(branchPoint?.querySelector('button[aria-label="Revert to message in new branch"]')).toBeTruthy()
    act(() => (navigation?.querySelector('button[aria-label="Previous message branch"]') as HTMLButtonElement).click())
    expect(setActiveSession).toHaveBeenCalledWith('chat-root')
    act(() => (navigation?.querySelector('button[aria-label="Next message branch"]') as HTMLButtonElement).click())
    expect(setActiveSession).toHaveBeenCalledWith('chat-branch-2')

    act(() => root.unmount())
    host.remove()
  })

  it('renders Codex runtime model options without Gemini fallback labels', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'gpt-5.4', name: 'gpt-5.4', description: 'Latest frontier agentic coding model.' },
            { id: 'gpt-5.4-mini', name: 'GPT-5.4-Mini', description: 'Smaller frontier agentic coding model.' },
          ],
          currentModelId: 'gpt-5.4',
          agentInfo: { name: 'codex', title: 'Codex', version: '1.0.0' },
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('gpt-5.4')
    expect(host.querySelector('.uam-provider-logo--codex')).toBeTruthy()

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(document.body.textContent).not.toContain('CLI default')
    expect(document.body.textContent).toContain('gpt-5.4')
    expect(document.body.textContent).toContain('GPT-5.4-Mini')
    expect(document.body.textContent).not.toContain('Auto 3')
    expect(document.body.textContent).not.toContain('Flash Lite')

    const miniButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('GPT-5.4-Mini')
    )
    expect(miniButton).toBeTruthy()
    act(() => {
      miniButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'gpt-5.4-mini')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps a new provider-default model visible until ACP resolves it', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {},
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const defaultSession = useAppStore.getState().sessions[0]
    act(() => root.render(<ChatView session={defaultSession} />))

    const defaultModelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(defaultModelButton.textContent).toContain('Default')
    act(() => defaultModelButton.click())
    const defaultMenu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(Array.from(defaultMenu.querySelectorAll('[data-testid="model-options"] [role="menuitemradio"]')).some((option) => option.textContent?.includes('Default'))).toBe(true)
    act(() => defaultMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))

    const explicitSession = { ...defaultSession, modelId: 'gpt-5.4' }
    act(() => root.render(<ChatView session={explicitSession} />))
    const explicitModelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(explicitModelButton.textContent).toContain('GPT-5.4')
    act(() => explicitModelButton.click())
    const explicitMenu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(Array.from(explicitMenu.querySelectorAll('[data-testid="model-options"] [role="menuitemradio"]')).some((option) => option.textContent?.includes('Default'))).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('ignores a stale ACP model catalog after switching provider', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Stale Gemini catalog' },
          ],
          currentModelId: 'auto-gemini-3',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector.textContent).toContain('Default')
    expect(selector.textContent).not.toContain('Auto 3')

    act(() => root.unmount())
    host.remove()
  })

  it('shows the selected Codex model while ACP still reports the previous idle model', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4-mini' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'gpt-5.4', name: 'GPT-5.4', description: '' },
            { id: 'gpt-5.4-mini', name: 'GPT-5.4 Mini', description: '' },
          ],
          currentModelId: 'gpt-5.4',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('button[aria-label="Select provider and model"]')?.textContent).toContain('GPT-5.4 Mini')

    act(() => root.unmount())
    host.remove()
  })

  it('navigates long model lists with the keyboard and keeps them scrollable', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    const availableModels = Array.from({ length: 12 }, (_, index) => ({
      id: `model-${index}`,
      name: `Model ${index}`,
      description: `Model ${index} detail`,
    }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'model-0' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          lifecycleState: 'ready',
          processing: false,
          availableModels,
          currentModelId: 'model-0',
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const trigger = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(trigger.getAttribute('aria-haspopup')).toBe('menu')
    expect(trigger.style.borderRadius).toBe('7px')
    act(() => trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLDivElement
    const modelOptions = menu.querySelector('[data-testid="model-options"]') as HTMLDivElement
    const options = Array.from(modelOptions.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]'))
    expect(options).toHaveLength(12)
    expect(modelOptions.style.maxHeight).toBe('520px')
    expect(modelOptions.style.overflowY).toBe('auto')
    expect(document.activeElement?.textContent).toContain('Model 0')

    act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 1')
    expect((document.activeElement as HTMLElement).style.boxShadow).toContain('inset')
    act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'model-1')
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
  })

  it('hides only repeated Copilot model details', () => {
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#fff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'copilot-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'copilot-cli',
          lifecycleState: 'ready',
          processing: false,
          availableModels: [
            { id: 'gpt-5.4', name: 'gpt-5.4', description: '' },
            { id: 'claude-opus', name: 'Claude Opus', description: 'Best for complex tasks' },
          ],
          currentModelId: '',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    act(() => (host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).click())

    const repeated = Array.from(document.body.querySelectorAll<HTMLElement>('[data-testid="model-options"] [role="menuitemradio"]')).find((option) => option.textContent?.includes('gpt-5.4'))
    expect(repeated?.textContent?.match(/gpt-5\.4/g)).toHaveLength(1)
    expect(document.body.textContent).toContain('Best for complex tasks')

    act(() => root.unmount())
    host.remove()
  })

  it('renders OpenCode model options without Gemini fallback labels', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli', modelId: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'opencode-cli',
          protocolKind: 'opencode-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M', name: 'Qwen3.6 35B A3B Q4', description: '' },
            { id: 'ollama-r9700/qwen3-coder:30b', name: 'Qwen3 Coder 30B', description: '' },
          ],
          currentModelId: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M',
          agentInfo: { name: 'opencode', title: 'OpenCode', version: '0.6.6' },
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(document.body.textContent).not.toContain('CLI default')
    expect(document.body.textContent).toContain('Qwen3.6 35B A3B Q4')
    expect(document.body.textContent).toContain('Qwen3 Coder 30B')
    expect(document.body.textContent).not.toContain('Auto 3')
    expect(document.body.textContent).not.toContain('Flash Lite')

    const coderButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Qwen3 Coder 30B')
    )
    expect(coderButton).toBeTruthy()
    act(() => {
      coderButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'ollama-r9700/qwen3-coder:30b')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders OpenCode Zen free model options from ACP state', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'opencode-cli',
          protocolKind: 'opencode-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'opencode/deepseek-v4-flash-free', name: 'DeepSeek V4 Flash Free', description: 'OpenCode Zen free model.' },
            { id: 'opencode/big-pickle', name: 'Big Pickle', description: 'OpenCode Zen limited-time stealth free model.' },
          ],
          currentModelId: '',
          agentInfo: { name: 'opencode', title: 'OpenCode', version: '0.6.6' },
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(document.body.textContent).toContain('DeepSeek V4 Flash Free')
    expect(document.body.textContent).toContain('OpenCode Zen free model.')
    expect(document.body.textContent).toContain('Big Pickle')
    expect(host.textContent).not.toContain('Auto 3')

    const freeButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('DeepSeek V4 Flash Free')
    )
    expect(freeButton).toBeTruthy()
    act(() => {
      freeButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'opencode/deepseek-v4-flash-free')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders dynamic ACP model options and applies the selected model id', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, modelId: 'auto-gemini-3' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          currentModelId: 'auto-gemini-3',
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Gemini 3 routing' },
            { id: 'gemini-3-flash-preview', name: 'Gemini 3 Flash', description: 'Preview model' },
          ],
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('Auto 3')

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Auto 3')
    expect(document.body.textContent).toContain('Gemini 3 Flash')

    const flashButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Gemini 3 Flash')
    )
    expect(flashButton).toBeTruthy()
    act(() => {
      flashButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'gemini-3-flash-preview')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('disables the model chip while ACP is processing', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ setSessionModel })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement | null
    expect(modelButton).toBeTruthy()
    expect(modelButton?.disabled).toBe(true)
    openComposerOptions(host)
    expect((document.body.querySelector('button[aria-label="Agent mode"]') as HTMLButtonElement | null)?.disabled).toBe(false)
    expect((document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement | null)?.disabled).toBe(false)

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).not.toContain('Gemini 3 Flash')
    expect(setSessionModel).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not surface a runtime-only Plan mode as a user-selected chip', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], currentModeId: 'plan', processing: false },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('button[aria-label="Disable Plan"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('selects Plan only from Agent mode and clears only the Plan chip', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          currentModeId: 'default',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const agentButton = document.body.querySelector('button[aria-label="Agent mode"]') as HTMLButtonElement
    expect(agentButton.textContent).toContain('Default')
    act(() => agentButton.click())
    const planOption = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('Plan')) as HTMLButtonElement
    act(() => planOption.click())

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'plan')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, approvalMode: 'plan' } : session
        ),
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            currentModeId: 'plan',
          },
        },
      }))
    })

    expect(host.querySelector('button[aria-label="Disable Plan"]')).toBeTruthy()

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session
        ),
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            currentModeId: 'plan',
          },
        },
      }))
    })

    expect(host.querySelector('button[aria-label="Disable Plan"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('changes Permissions without changing Plan, reasoning, speed, or memory', () => {
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, approvalMode: 'plan', commandSafetyTier: 'medium', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          currentModeId: 'plan',
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          pendingPermission: null,
        },
      },
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const permissionButton = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    act(() => permissionButton.click())
    const yoloOption = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('YOLO')) as HTMLButtonElement
    act(() => yoloOption.click())
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, commandSafetyTier: 'yolo' } : session
        ),
      }))
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const yoloChip = host.querySelector('button[aria-label="Disable YOLO"]') as HTMLButtonElement
    expect(yoloChip).toBeTruthy()
    act(() => yoloChip.click())
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'off')
    expect(useAppStore.getState().sessions[0]).toMatchObject({ approvalMode: 'plan', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches Claude from plan mode back to default', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Claude structured mode cannot surface interactive permission')
    expect(host.textContent).toContain('model discovery is limited to the active model')

    const planButton = host.querySelector('button[aria-label="Disable Plan"]') as HTMLButtonElement | null
    expect(planButton).toBeTruthy()

    act(() => {
      planButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'default')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps Claude Accept Edits in Permissions and independent from Agent mode', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1'
          ? { ...session, providerId: 'claude-cli', approvalMode: 'plan', commandSafetyTier: 'off', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' }
          : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const agentButton = document.body.querySelector('button[aria-label="Agent mode"]') as HTMLButtonElement
    act(() => agentButton.click())
    expect(Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).some((option) => option.textContent?.startsWith('Accept Edits'))).toBe(false)
    act(() => agentButton.click())

    const permissionButton = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    act(() => permissionButton.click())
    const acceptEditsButton = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('Accept Edits')) as HTMLButtonElement
    expect(acceptEditsButton).toBeTruthy()
    act(() => acceptEditsButton.click())

    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'acceptEdits')
    expect(setSessionApprovalMode).not.toHaveBeenCalled()

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, commandSafetyTier: 'acceptEdits' } : session
        ),
      }))
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const acceptEditsChip = host.querySelector('button[aria-label="Disable Accept Edits"]') as HTMLButtonElement
    expect(acceptEditsChip).toBeTruthy()
    act(() => acceptEditsChip.click())
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'off')
    expect(setSessionApprovalMode).not.toHaveBeenCalled()
    expect(useAppStore.getState().sessions[0]).toMatchObject({ approvalMode: 'plan', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('asks how to proceed before sending a prompt from Claude plan mode', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve(true))
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement | null
    expect(textarea).toBeTruthy()
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, 'proceed')
      textarea!.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement | null
    await act(async () => {
      form?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

    expect(sendAcpPrompt).not.toHaveBeenCalled()
    expect(host.textContent).toContain('Claude Plan mode is read-only')

    const acceptAndProceed = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Accept edits and proceed') as HTMLButtonElement | undefined
    expect(acceptAndProceed).toBeTruthy()
    await act(async () => {
      acceptAndProceed?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'default')
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'acceptEdits')
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'proceed')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('arms the next message as a goal and sends the same text with the default budget', async () => {
    const setGoal = vi.fn(() => Promise.resolve('goal-1'))
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      setGoal,
      sendAcpPrompt,
      defaultGoalTokenBudgetByChatId: { ...state.defaultGoalTokenBudgetByChatId, 'chat-1': 1234 },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const goalButton = document.body.querySelector('button[title="Use the next message as a goal"]') as HTMLButtonElement | null
    expect(goalButton).toBeTruthy()
    act(() => {
      goalButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.textContent).toContain('Goal: next message')

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement | null
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, 'Ship the goal loop')
      textarea!.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement | null
    await act(async () => {
      form?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

	expect(setGoal).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', 1234, 'uam')
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', [])

    act(() => {
      root.unmount()
    })
    host.remove()
  })

	it('delegates a configured provider goal command exactly once', async () => {
	  const setGoal = vi.fn(() => Promise.resolve('goal-native'))
	  const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
	  useAppStore.setState((state) => ({
		setGoal,
		sendAcpPrompt,
		defaultGoalTokenBudgetByChatId: { ...state.defaultGoalTokenBudgetByChatId, 'chat-1': 0 },
		providers: state.providers.map((provider) => provider.id === 'codex-cli' ? { ...provider, nativeGoalCommand: '/ralph' } : provider),
		sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session),
	  }))

	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
	  const textarea = host.querySelector('textarea')!
	  act(() => {
		const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
		valueSetter?.call(textarea, '/goal Keep the objective literal')
		textarea.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  await act(async () => {
		host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
	  })

	  expect(setGoal).toHaveBeenCalledWith('chat-1', 'Keep the objective literal', 0, 'provider')
	  expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
	  expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', '/ralph Keep the objective literal', [])
	  act(() => root.unmount())
	  host.remove()
	})

  it('renders active goal pause/delete controls and paused goal resume control', async () => {
    const updateGoalStatus = vi.fn(() => Promise.resolve(true))
    const removeGoal = vi.fn(() => Promise.resolve(true))
    let finishResume: ((ok: boolean) => void) | undefined
    const resumeGoal = vi.fn(() => new Promise<boolean>((resolve) => { finishResume = resolve }))
    const goal = {
      id: 'goal-1',
      chatId: 'chat-1',
      objective: 'Review the Ralph loop',
      status: 'active' as const,
      tokenBudget: 0,
      tokensUsed: 0,
      blockedTurnCount: 0,
      completedItems: ['Audit messages'],
      remainingItems: ['Implement editing', 'Run tests'],
      currentStep: 'Implement editing',
      createdAt: new Date('2026-01-01T00:00:00.000Z'),
      updatedAt: new Date('2026-01-01T00:00:00.000Z'),
    }
    useAppStore.setState((state) => ({
      updateGoalStatus,
      removeGoal,
      resumeGoal,
      goalsByChatId: { ...state.goalsByChatId, 'chat-1': [goal] },
      activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': 'goal-1' },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const progress = host.querySelector('progress[aria-label="Goal progress"]') as HTMLProgressElement | null
    expect(progress?.max).toBe(3)
    expect(progress?.value).toBe(1)
    expect(host.textContent).toContain('Implement editing')

    // Goal actions are consolidated into an overflow menu; open it to reach them.
    const openGoalMenu = () => act(() => {
      (host.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
      ;(host.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    const menuItem = (text: string) =>
      Array.from(document.body.querySelectorAll('button')).find((b) => b.textContent === text) as HTMLButtonElement | undefined

    openGoalMenu()
    expect(menuItem('Mark complete')).toBeTruthy()
    const pauseButton = menuItem('Pause goal')
    expect(pauseButton).toBeTruthy()
    expect(menuItem('Delete goal')).toBeTruthy()

    act(() => {
      pauseButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(updateGoalStatus).toHaveBeenCalledWith('goal-1', 'paused')

    act(() => {
      useAppStore.setState((state) => ({
        goalsByChatId: { ...state.goalsByChatId, 'chat-1': [{ ...goal, status: 'paused' as const }] },
        activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': null },
      }))
    })

    openGoalMenu()
    const resumeButton = menuItem('Resume goal')
    expect(resumeButton).toBeTruthy()
    act(() => {
      resumeButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(resumeGoal).toHaveBeenCalledWith('chat-1', 'goal-1')
    openGoalMenu()
    const pendingResume = menuItem('Resuming…')
    expect(pendingResume?.disabled).toBe(true)
    await act(async () => {
      finishResume?.(false)
      await Promise.resolve()
    })
    expect(host.textContent).toContain('Failed to resume goal.')

    act(() => {
      menuItem('Delete goal')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(removeGoal).toHaveBeenCalledWith('goal-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('selects the per-chat memory level from options and /memory', async () => {
    const setSessionMemoryLevel = vi.fn((_id: string, level: 'off' | 'strict' | 'balanced' | 'open') => {
      useAppStore.setState((state) => ({ sessions: state.sessions.map((candidate) => candidate.id === 'chat-1' ? { ...candidate, memoryEnabled: level !== 'off', memoryLevel: level } : candidate) }))
      return Promise.resolve(true)
    })
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, memoryEnabled: true, memoryLevel: 'strict' as const } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
        },
      },
      setSessionMemoryLevel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const memoryButton = document.body.querySelector('button[aria-label="Memory"]') as HTMLButtonElement | null
    expect(memoryButton).toBeTruthy()
    expect(memoryButton?.disabled).toBe(false)

    act(() => {
      memoryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const balanced = Array.from(document.body.querySelectorAll('button[role="option"]')).find((button) => button.textContent?.includes('Balanced'))
    act(() => {
      balanced?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionMemoryLevel).toHaveBeenCalledWith('chat-1', 'balanced')

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('button[aria-label="Disable Memory Balanced"]')).toBeTruthy()

    act(() => { (document.body.querySelector('button[aria-label="Memory"]') as HTMLButtonElement).click() })
    const strict = Array.from(document.body.querySelectorAll('button[role="option"]')).find((button) => button.textContent?.includes('Strict')) as HTMLButtonElement
    act(() => strict.click())
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('button[aria-label="Disable Memory Strict"]')).toBeTruthy()

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/memory open')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionMemoryLevel).toHaveBeenLastCalledWith('chat-1', 'open')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  // Markdown rendering (tables, code fences, inline links) is covered directly in
  // components/markdown/Markdown.test.tsx and markdownParsing.test.ts (FE-3 extraction).

  it('renders persisted assistant thoughts when no active ACP timeline is available', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2' ? { ...message, thoughts: 'Persisted reasoning\nwith detail.' } : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Thinking')
    expect(host.textContent).toContain('Persisted reasoning')
    expect(host.textContent).toContain('with detail.')
    expect(host.textContent).toContain('Before tool. After tool.')
    const thinkingBlock = host.querySelector('[data-testid="thinking-block"]') as HTMLDetailsElement | null
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)
    expect(thinkingBlock?.tagName).toBe('DETAILS')
    expect(thinkingBlock?.textContent).toContain('Persisted reasoning')
    expect(thinkingBlock?.textContent).toContain('with detail.')
    expect(thinkingBlock?.hasAttribute('open')).toBe(false)
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders persisted assistant tool calls after ACP turn state is gone', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  thoughts: '',
                  toolCalls: [
                    {
                      id: 'persisted-tool-1',
                      title: 'Read saved file',
                      kind: 'read',
                      status: 'completed',
                      content: 'Saved tool output',
                    },
                  ],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Tool call:')
    expect(host.textContent).toContain('Read saved file')
    expect(host.querySelector('[data-tool-status="completed"][aria-label="Tool status: Completed"]')).toBeTruthy()
    expect(host.querySelector('[data-tool-status="completed"] svg')).toBeTruthy()

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Read saved file')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Saved tool output')
    const dialog = document.body.querySelector('[role="dialog"]')
    expect(dialog).toBeTruthy()
    expect(dialog?.querySelector('[data-tool-status="completed"][aria-label="Tool status: Completed"]')).toBeTruthy()
    expect(dialog?.textContent).not.toContain('status: completed')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders persisted ordered message blocks instead of regrouping assistant fields', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  content: 'Grouped content should not render.',
                  thoughts: 'Grouped thought should not render.',
                  planSummary: 'Ordered plan summary.',
                  planEntries: [{ content: 'Ordered plan step', priority: '', status: 'pending' }],
                  toolCalls: [
                    {
                      id: 'persisted-tool-1',
                      title: 'Ordered saved tool',
                      kind: 'read',
                      status: 'completed',
                      content: 'Saved tool output',
                    },
                  ],
                  blocks: [
                    { type: 'thought', text: 'First thought marker.' },
                    { type: 'assistant_text', text: 'First visible marker.' },
                    { type: 'tool_call', toolCallId: 'persisted-tool-1' },
                    { type: 'thought', text: 'Second thought marker.' },
                    { type: 'assistant_text', text: 'Final visible marker.' },
                    { type: 'plan' },
                  ],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const text = host.textContent ?? ''
    const firstThought = text.indexOf('First thought marker.')
    const firstVisible = text.indexOf('First visible marker.')
    const tool = text.indexOf('Ordered saved tool')
    const secondThought = text.indexOf('Second thought marker.')
    const finalVisible = text.indexOf('Final visible marker.')
    const plan = text.indexOf('Ordered plan summary.')
    expect(firstThought).toBeGreaterThan(-1)
    expect(firstVisible).toBeGreaterThan(firstThought)
    expect(tool).toBeGreaterThan(firstVisible)
    expect(secondThought).toBeGreaterThan(tool)
    expect(finalVisible).toBeGreaterThan(secondThought)
    expect(plan).toBe(-1)
    expect(text).not.toContain('Grouped content should not render.')
    expect(text).not.toContain('Grouped thought should not render.')
    expect(text.match(/Ordered saved tool/g) ?? []).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ordered goal review JSON as a distinct block without raw JSON', () => {
    const reviewJson = '{"decision":"continue","reason":"More work remains.","nextPrompt":"Continue with the next page.","evidence":["Reviewed the current diff."],"progressUpdate":{"currentStep":"Finish the branch UI.","lastVerification":"Focused tests passed."}}'
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  content: 'Grouped fallback content should not render.',
                  thoughts: '',
                  toolCalls: [],
                  planSummary: '',
                  planEntries: [],
                  blocks: [{ type: 'assistant_text' as const, text: reviewJson }],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const goalReviewFrame = host.querySelector('[data-message-kind="goal-review"]')
    expect(goalReviewFrame).not.toBeNull()
    expect(goalReviewFrame?.textContent).toContain('Goal Reviewer')
    expect(goalReviewFrame?.querySelector('[data-testid="goal-review-block"]')).not.toBeNull()
    expect(goalReviewFrame?.textContent).not.toContain('{"decision"')
    expect(goalReviewFrame?.textContent).not.toContain('Grouped fallback content should not render.')
    expect(host.textContent).toContain('Goal Review')
    expect(host.textContent).toContain('Continue')
    expect(host.textContent).toContain('More work remains.')
    expect(host.textContent).toContain('Next:')
    expect(host.textContent).toContain('Reviewed the current diff.')
    expect(host.textContent).toContain('Finish the branch UI.')
    expect(host.textContent).toContain('Focused tests passed.')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders Codex thinking and persisted plan actions', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sendAcpPrompt,
      setSessionApprovalMode,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'plan' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Please make a plan',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            thoughts: '### Reasoning\nInspecting files.\n\n### Summary\nNeed to patch Codex handling.',
            planSummary: 'Update Codex support.',
            planEntries: [
              { content: 'Update Codex support.', priority: 'duplicate', status: 'pending' },
              { content: 'Inspect protocol events', priority: '', status: 'completed' },
              { content: 'Patch rendering', priority: '', status: 'pending' },
            ],
            blocks: [
              { type: 'thought', text: '### Reasoning\nInspecting files.\n\n### Summary\nNeed to patch Codex handling.' },
              { type: 'plan' },
              { type: 'plan' },
            ],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Thinking')
    expect(host.textContent).toContain('Reasoning')
    expect(host.textContent).toContain('Inspecting files.')
    expect(host.textContent).toContain('Summary')
    expect(host.textContent).toContain('Need to patch Codex handling.')
    expect(host.textContent).toContain('Plan')
    expect(host.textContent).toContain('Update Codex support.')
    expect((host.textContent?.match(/Update Codex support\./g) ?? [])).toHaveLength(1)
    expect(host.textContent).toContain('Inspect protocol events')
    expect(host.textContent).toContain('completed')
    expect(host.textContent).toContain('Patch rendering')
    expect(host.textContent).toContain('pending')
    expect(host.querySelectorAll('[data-testid="plan-block"]')).toHaveLength(1)

    const approveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Approve')
    const denyButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Deny')
    expect(approveButton).toBeTruthy()
    expect(denyButton).toBeTruthy()

    await act(async () => {
      approveButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionApprovalMode).toHaveBeenNthCalledWith(1, 'chat-1', 'default')
    expect(sendAcpPrompt).toHaveBeenNthCalledWith(1, 'chat-1', 'Proceed with the plan.')
    expect(setSessionApprovalMode.mock.invocationCallOrder[0]).toBeLessThan(sendAcpPrompt.mock.invocationCallOrder[0])

    await act(async () => {
      denyButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionApprovalMode).toHaveBeenNthCalledWith(2, 'chat-1', 'plan')
    expect(sendAcpPrompt).toHaveBeenNthCalledWith(
      2,
      'chat-1',
      'Do not proceed with this plan. Please revise it before making changes.'
    )

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not promote ordinary Codex task progress into an actionable Plan card', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'default' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [{
          id: 'm-1',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Implemented the requested change.',
          planSummary: 'Internal task progress.',
          planEntries: [{ content: 'Run tests', priority: '', status: 'completed' }],
          createdAt: new Date('2026-01-01T00:00:01.000Z'),
        }],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          currentModeId: 'default',
          processing: false,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent).toContain('Implemented the requested change.')
    expect(host.querySelector('[data-testid="plan-block"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('hides historical Codex plan cards after a later user message', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Plan this',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            planSummary: 'Historical plan.',
            planEntries: [{ content: 'Old step', priority: '', status: 'pending' }],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
          {
            id: 'm-3',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Actually revise it',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Historical plan.')
    expect(host.textContent).not.toContain('Old step')
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === 'Approve')).toBe(false)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === 'Deny')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('disables Codex plan actions while the active ACP plan is still processing', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'plan' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Plan this',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            planSummary: 'Active plan.',
            planEntries: [{ content: 'Working step', priority: '', status: 'inProgress' }],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'processing',
          processing: true,
          planSummary: 'Active plan.',
          planEntries: [{ content: 'Working step', priority: '', status: 'inProgress' }],
          turnEvents: [{ type: 'plan' }],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Active plan.')
    expect(host.textContent).toContain('Working step')
    expect(host.textContent).toContain('in progress')
    const approveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Approve') as HTMLButtonElement | undefined
    const denyButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Deny') as HTMLButtonElement | undefined
    expect(approveButton).toBeTruthy()
    expect(denyButton).toBeTruthy()
    expect(approveButton?.disabled).toBe(true)
    expect(denyButton?.disabled).toBe(true)
    expect(approveButton?.getAttribute('title')).toBe('Codex is still working.')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ACP errors in the composer area', async () => {
    const writeText = vi.fn((_text: string) => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })

    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'error',
            processing: false,
            processingStartedAtMs: null,
            lastError: 'Internal ACP failure',
            recentStderr: 'stderr stack trace',
            lastExitCode: 137,
            diagnostics: [
              {
                time: '2026-01-01T00:00:02.000Z',
                event: 'response',
                reason: 'jsonrpc_error',
                method: 'session/prompt',
                requestId: '42',
                code: -32603,
                message: 'Internal error',
                detail: 'error.data={"cause":"boom"}',
                lifecycleState: 'processing',
              },
            ],
          },
        },
      }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={{ ...useAppStore.getState().sessions[0], providerId: 'opencode-cli' }} />)
    })

    const form = host.querySelector('form')
      expect(form?.textContent).toContain('Gemini ACP error')
      expect(form?.textContent).not.toContain('OpenCode ACP error')
      expect(form?.textContent).toContain('Internal ACP failure')
      expect(form?.textContent).toContain('Diagnostics')
      expect(form?.textContent).toContain('Exit code: 137')
      expect(form?.textContent).toContain('jsonrpc_error')
      expect(form?.textContent).toContain('stderr stack trace')
      const copyErrorButton = Array.from(form?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Copy error')
      expect(copyErrorButton).toBeTruthy()
      await act(async () => {
        copyErrorButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
        await Promise.resolve()
      })
      expect(writeText).toHaveBeenCalled()
      expect(writeText.mock.calls[0][0]).toContain('Internal ACP failure')
      expect(writeText.mock.calls[0][0]).toContain('stderr stack trace')
      const text = host.textContent ?? ''
      expect(text.indexOf('After tool.')).toBeLessThan(text.indexOf('Gemini ACP error'))
      const dismissErrorButton = form?.querySelector<HTMLButtonElement>('button[aria-label="Dismiss composer error"]')
      expect(dismissErrorButton).toBeTruthy()
      act(() => dismissErrorButton?.click())
      expect(form?.textContent).not.toContain('Internal ACP failure')

      act(() => useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': { ...state.acpBindingBySessionId['chat-1'], lastError: '' },
        },
      })))
      act(() => useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': { ...state.acpBindingBySessionId['chat-1'], lastError: 'Internal ACP failure' },
        },
      })))
      expect(form?.textContent).toContain('Internal ACP failure')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('uses turn serials to isolate the active ACP turn and close stale tool details', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Search symbols')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Searching workspace symbols')

    act(() => {
      useAppStore.setState((state) => ({
        messages: {
          ...state.messages,
          'chat-1': [
            ...(state.messages['chat-1'] ?? []),
            {
              id: 'm-3',
              sessionId: 'chat-1',
              role: 'user',
              content: 'Now summarize it',
              createdAt: new Date('2026-01-01T00:00:02.000Z'),
            },
            {
              id: 'm-4',
              sessionId: 'chat-1',
              role: 'assistant',
              content: 'This placeholder should be replaced.',
              createdAt: new Date('2026-01-01T00:00:03.000Z'),
            },
          ],
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            turnEvents: [{ type: 'assistant_text', text: 'Second answer only.' }],
            turnUserMessageIndex: 2,
            turnAssistantMessageIndex: 3,
            turnSerial: 2,
            toolCalls: [
              {
                id: 'tool-1',
                title: 'Second tool',
                kind: 'summary',
                status: 'completed',
                content: 'Second turn details',
              },
            ],
            pendingPermission: null,
          },
        },
      }))
    })

    expect(host.textContent).toContain('Second answer only.')
    expect(host.textContent).not.toContain('This placeholder should be replaced.')
    expect(host.textContent).not.toContain('Searching workspace symbols')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('expands multiple sub-agent chats inline without replacing the current chat', async () => {
    vi.useFakeTimers()
    const originalOpenSubAgentSession = useAppStore.getState().openSubAgentSession
    const openSubAgentSession = vi.fn((_sourceChatId: string, nativeSessionId: string) => Promise.resolve(nativeSessionId === 'agent-session-2' ? 'agent-chat-2' : 'agent-chat'))
    useAppStore.setState((state) => ({
      openSubAgentSession,
      sessions: [
        ...state.sessions,
        { ...state.sessions[0], id: 'agent-chat', name: 'Planner history' },
        { ...state.sessions[0], id: 'agent-chat-2', name: 'Reviewer history' },
      ],
      messages: {
        ...state.messages,
        'agent-chat': [
          {
            id: 'agent-message-1',
            sessionId: 'agent-chat',
            role: 'assistant',
            content: 'Sub-agent inspected the provider runtime.',
            toolCalls: [{ id: 'nested-tool-1', title: 'Inspect runtime', kind: 'read', status: 'completed', content: 'Nested tool output' }],
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
        ],
        'agent-chat-2': [
          {
            id: 'agent-message-2',
            sessionId: 'agent-chat-2',
            role: 'assistant',
            content: 'Sub-agent reviewed the provider runtime.',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          toolCalls: [
            {
              id: 'agent-tool-1',
              title: 'Planner agent',
              kind: 'sub-agent',
              status: 'running',
              content: 'Inspecting with a sub-agent',
              isSubAgent: true,
              subAgentId: 'agent-session-1',
              subAgentTitle: 'Planner',
            },
            {
              id: 'agent-tool-2',
              title: 'Reviewer agent',
              kind: 'sub-agent',
              status: 'completed',
              content: 'Reviewing with a sub-agent',
              isSubAgent: true,
              subAgentId: 'agent-session-2',
              subAgentTitle: 'Reviewer',
            },
          ],
          turnEvents: [
            { type: 'assistant_text', text: 'I am delegating to a sub-agent.' },
            { type: 'tool_call', toolCallId: 'agent-tool-1' },
            { type: 'tool_call', toolCallId: 'agent-tool-2' },
          ],
          turnSerial: 2,
          pendingPermission: null,
          pendingUserInput: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const summaries = Array.from(host.querySelectorAll('summary')).filter((candidate) => candidate.textContent?.includes('Sub-agent:'))
    expect(summaries).toHaveLength(2)
    expect(summaries[0].querySelector('[data-tool-status="running"][aria-label="Tool status: Running"]')).toBeTruthy()
    expect(summaries[1].querySelector('[data-tool-status="completed"][aria-label="Tool status: Completed"]')).toBeTruthy()

    await act(async () => {
      for (const summary of summaries) {
        const details = summary.closest('details') as HTMLDetailsElement
        details.open = true
        details.dispatchEvent(new Event('toggle', { bubbles: true }))
      }
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'agent-session-1', 'Planner', false)
    expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'agent-session-2', 'Reviewer', false)
    expect(host.textContent).toContain('Planner history')
    expect(host.textContent).toContain('Sub-agent inspected the provider runtime.')
    expect(host.textContent).toContain('Reviewer history')
    expect(host.textContent).toContain('Sub-agent reviewed the provider runtime.')
    expect(host.textContent?.match(/Inspect runtime/g)).toHaveLength(1)
    const nestedToolButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Inspect runtime'))
    act(() => nestedToolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(document.body.querySelector('[role="dialog"]')?.textContent).toContain('Nested tool output')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    await act(async () => {
      vi.advanceTimersByTime(1000)
      await Promise.resolve()
    })
    expect(openSubAgentSession.mock.calls.filter(([, nativeSessionId]) => nativeSessionId === 'agent-session-1')).toHaveLength(2)
    expect(openSubAgentSession.mock.calls.filter(([, nativeSessionId]) => nativeSessionId === 'agent-session-2')).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ openSubAgentSession: originalOpenSubAgentSession })
    vi.useRealTimers()
  })

  it('opens the current workspace from the composer workspace row', async () => {
    const originalOpenSessionWorkspace = useAppStore.getState().openSessionWorkspace
    const openSessionWorkspace = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ openSessionWorkspace })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openWorkspaceActions(host)
    const openButton = document.body.querySelector('button[role="menuitem"]') as HTMLButtonElement | null
    expect(openButton).toBeTruthy()
    expect(openButton?.textContent).toContain('Open workspace')

    await act(async () => {
      openButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openSessionWorkspace).toHaveBeenCalledWith('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ openSessionWorkspace: originalOpenSessionWorkspace })
  })

  it('expires successful workspace feedback but keeps errors until dismissed', async () => {
    vi.useFakeTimers()
    const originalEditor = useAppStore.getState().openSessionWorkspaceEditor
    const originalTerminal = useAppStore.getState().openSessionTerminal
    useAppStore.setState({
      openSessionWorkspaceEditor: vi.fn(() => Promise.resolve(true)),
      openSessionTerminal: vi.fn(() => Promise.resolve(false)),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    openWorkspaceActions(host)
    await act(async () => {
      ;(Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]')).find((button) => button.textContent?.includes('Open in editor')) as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Opened workspace editor.')
    expect(host.querySelector('button[aria-label="Dismiss workspace action message"]')).toBeTruthy()
    act(() => vi.advanceTimersByTime(5000))
    expect(host.textContent).not.toContain('Opened workspace editor.')

    openWorkspaceActions(host)
    await act(async () => {
      ;(Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]')).find((button) => button.textContent?.includes('Open terminal')) as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(Array.from(host.querySelectorAll('[role="alert"]')).some((alert) => alert.textContent?.includes('Failed to open terminal.'))).toBe(true)
    act(() => vi.advanceTimersByTime(6000))
    expect(host.textContent).toContain('Failed to open terminal.')
    act(() => (host.querySelector('button[aria-label="Dismiss workspace action message"]') as HTMLButtonElement).click())
    expect(host.textContent).not.toContain('Failed to open terminal.')

    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ openSessionWorkspaceEditor: originalEditor, openSessionTerminal: originalTerminal })
    vi.useRealTimers()
  })

  it('matches worktree action availability to the native runtime rules', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({
        ...session,
        workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
        workspaceSourceDirectory: '/tmp/project',
        workspaceIsolationKind: 'gitWorktree',
      })),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: true,
          processing: false,
          pendingPermission: null,
          pendingUserInput: null,
          queuedPrompts: [],
        },
      },
      cliBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    openWorkspaceActions(host)
    const discard = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]'))
      .find((button) => button.textContent === 'Discard & return') as HTMLButtonElement
    expect(discard.disabled).toBe(false)

    act(() => {
      useAppStore.setState({
        cliBindingBySessionId: {
          'chat-1': {
            terminalId: 'term-1',
            boundChatId: 'chat-1',
            running: true,
            lifecycleState: 'idle',
            turnState: 'idle',
            processing: false,
            readySinceLastSelect: true,
            active: true,
            lastError: '',
          },
        },
      })
    })
    expect(discard.disabled).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('clears transient worktree action messages when the active chat changes', async () => {
    const originalDiscardChatWorktreeChanges = useAppStore.getState().discardChatWorktreeChanges
    const discardChatWorktreeChanges = vi.fn(() => Promise.resolve({ ok: true, message: 'Discarded changes in the chat worktree.', patchPath: '' }))
    useAppStore.setState((state) => ({
      discardChatWorktreeChanges,
      sessions: [
        {
          ...state.sessions[0],
          workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
          workspaceSourceDirectory: '/tmp/project',
          workspaceIsolationKind: 'gitWorktree',
        },
        {
          id: 'chat-2',
          name: 'Second Chat',
          viewMode: 'chat',
          folderId: 'default',
          workspaceDirectory: '/tmp/project',
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
          updatedAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      messages: {
        ...state.messages,
        'chat-2': [],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
        'chat-2': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'gemini-cli',
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceMenu = openWorkspaceActions(host)
    const discardButton = Array.from(workspaceMenu?.querySelectorAll<HTMLButtonElement>('button') ?? []).find((button) => button.textContent === 'Discard & return')
    expect(discardButton).toBeTruthy()

    await act(async () => {
      discardButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(discardChatWorktreeChanges).toHaveBeenCalledWith('chat-1')
    expect(host.textContent).toContain('Discarded changes in the chat worktree.')

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[1]} />)
    })

    expect(host.textContent).not.toContain('Discarded changes in the chat worktree.')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ discardChatWorktreeChanges: originalDiscardChatWorktreeChanges })
  })

  it('clears transient worktree action messages when a chat returns to its source workspace', async () => {
    const originalDiscardChatWorktreeChanges = useAppStore.getState().discardChatWorktreeChanges
    const discardChatWorktreeChanges = vi.fn(() => Promise.resolve({ ok: true, message: 'Discarded changes in the chat worktree.', patchPath: '' }))
    useAppStore.setState((state) => ({
      discardChatWorktreeChanges,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1'
          ? {
              ...session,
              workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
              workspaceSourceDirectory: '/tmp/project',
              workspaceIsolationKind: 'gitWorktree',
            }
          : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceMenu = openWorkspaceActions(host)
    const discardButton = Array.from(workspaceMenu?.querySelectorAll<HTMLButtonElement>('button') ?? []).find((button) => button.textContent === 'Discard & return')
    expect(discardButton).toBeTruthy()

    await act(async () => {
      discardButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(host.textContent).toContain('Discarded changes in the chat worktree.')

    const returnedSession = {
      ...useAppStore.getState().sessions[0],
      workspaceDirectory: '/tmp/project',
      workspaceSourceDirectory: '',
      workspaceIsolationKind: '',
    }

    act(() => {
      root.render(<ChatView session={returnedSession} />)
    })

    expect(host.textContent).not.toContain('Discarded changes in the chat worktree.')
    expect(host.textContent).not.toContain('Git worktree')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ discardChatWorktreeChanges: originalDiscardChatWorktreeChanges })
  })

  it('stages selected files and sends them with the prompt', async () => {
    let stagedAttachment = {
      id: 'file-1',
      name: 'diagram.png',
      type: 'image',
      size: 4,
      path: '.UAM/attachments/chat-1/diagram.png',
    }
    const stageChatAttachments = vi.fn((_sessionId, items) => {
      stagedAttachment = {
        ...stagedAttachment,
        id: items[0].id,
      }
      return Promise.resolve([stagedAttachment])
    })
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      stageChatAttachments,
      sendAcpPrompt,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const input = host.querySelector('input[type="file"]') as HTMLInputElement
    const file = new File(['data'], 'diagram.png', { type: 'image/png' })
    Object.defineProperty(file, 'path', { value: '/tmp/diagram.png', configurable: true })
    Object.defineProperty(input, 'files', { value: [file], configurable: true })
    await act(async () => {
      input.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(stageChatAttachments).toHaveBeenCalledWith('chat-1', [
      expect.objectContaining({ name: 'diagram.png', kind: 'image', mimeType: 'image/png' }),
    ])
    expect(host.textContent).toContain('diagram.png')

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, 'Use this image')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement
    await act(async () => {
      form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Use this image', [stagedAttachment])

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not block normal text paste in the composer', async () => {
    const stageChatAttachments = vi.fn((_sessionId, items) => Promise.resolve([
      {
        id: items[0].id,
        name: 'pasted.png',
        type: 'image',
        size: 4,
        path: '.UAM/attachments/chat-1/pasted.png',
      },
    ]))
    useAppStore.setState((state) => ({
      stageChatAttachments,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const textPaste = new Event('paste', { bubbles: true, cancelable: true })
    Object.defineProperty(textPaste, 'clipboardData', {
      value: { files: [], getData: () => 'plain text' },
    })
    await act(async () => {
      textarea.dispatchEvent(textPaste)
    })
    expect(textPaste.defaultPrevented).toBe(false)

    expect(stageChatAttachments).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('hides workspace actions when no workspace is selected', () => {
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, directory: '' })),
      sessions: state.sessions.map((session) => ({ ...session, workspaceDirectory: '' })),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceCue = host.querySelector('button[aria-label="Workspace not selected"]') as HTMLButtonElement
    expect(workspaceCue).toBeTruthy()
    expect(workspaceCue.disabled).toBe(true)
    expect(host.querySelector('button[aria-label="Workspace actions"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('normalizes legacy provider chats to the Gemini-only provider list without showing Codex UI', () => {
    useAppStore.getState().loadFromCef({
      folders: [
        {
          id: 'default',
          title: 'Project',
          directory: '/tmp/project',
          collapsed: false,
        },
      ],
      resourceCollections: [],
      chats: [
        {
          id: 'chat-1',
          title: 'Legacy Codex Chat',
          folderId: 'default',
          providerId: 'codex-cli',
          createdAt: new Date('2026-01-01T00:00:00.000Z').toISOString(),
          updatedAt: new Date('2026-01-01T00:00:00.000Z').toISOString(),
          messages: [],
          acpSession: {
            providerId: 'codex-cli',
            protocolKind: 'codex-app-server',
            running: false,
            processing: false,
            lifecycleState: 'ready',
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
          },
        },
      ],
      selectedChatId: 'chat-1',
      providers: [
        {
          id: 'gemini-cli',
          name: 'Gemini CLI',
          shortName: 'Gemini',
          outputMode: 'cli',
          supportsCli: true,
          supportsStructured: true,
          structuredProtocol: 'gemini-acp',
        },
      ],
      settings: {
        activeProviderId: 'gemini-cli',
        theme: 'dark',
        memoryEnabledDefault: true,
        memoryIdleDelaySeconds: 60,
        memoryRecallBudgetBytes: 2048,
        goalMaxLoopIterations: 200,
        updateChecksEnabled: true,
        updateLastCheckedAt: '',
        dismissedUpdateVersions: {},
        memoryLastStatus: '',
        memoryWorkerBindings: {
          'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: '' },
        },
      },
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Codex is not supported in this build')
    expect(host.querySelector('button[aria-label="Select provider and model"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('omits the provider group when Gemini is the only available provider for the chat', () => {
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
      ],
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector).toBeTruthy()
    act(() => selector.click())
    expect(document.body.querySelector('[role="group"][aria-label="Provider"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps stopped dictation as a draft and only submits when Send stops it', async () => {
    const requests: Array<{ action: string; payload?: { locale?: string } }> = []
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    let completeDictationStart: (() => void) | undefined
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: { locale?: string } }
      requests.push(parsed)
      if (parsed.action === 'startDictation') {
        completeDictationStart = () => onSuccess('{}')
        return
      }
      onSuccess('{}')
    })
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Please')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const startButton = host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement
    expect(startButton).toBeTruthy()
    expect(startButton.getAttribute('aria-pressed')).toBe('false')
    await act(async () => {
      startButton.click()
      await Promise.resolve()
    })

    expect(requests[0]?.action).toBe('startDictation')
    expect(typeof requests[0]?.payload?.locale).toBe('string')
    const startingButton = host.querySelector('button[aria-label="Starting dictation"]') as HTMLButtonElement
    expect(startingButton.dataset.dictationState).toBe('starting')
    expect(startingButton.getAttribute('aria-busy')).toBe('true')
    expect(host.querySelector('button[aria-label="Send prompt"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Options"]')).toBeTruthy()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Starting dictation')

    await act(async () => {
      completeDictationStart?.()
      await Promise.resolve()
    })

    const listeningButton = host.querySelector('button[aria-label="Stop dictation"]') as HTMLButtonElement
    expect(listeningButton.getAttribute('aria-pressed')).toBe('true')
    expect(listeningButton.dataset.dictationState).toBe('listening')
    expect(listeningButton.querySelector('.uam-dictation-listening-indicator')).toBeTruthy()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Listening')
    expect(textarea.disabled).toBe(true)

    act(() => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'interim', text: 'write' },
      }))
    })
    expect(textarea.value).toBe('Please write')

    act(() => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'final', text: 'write tests' },
      }))
    })
    expect(textarea.value).toBe('Please write tests')

    await act(async () => {
      ;(host.querySelector('button[aria-label="Stop dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(requests.some((request) => request.action === 'stopDictation')).toBe(true)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Finishing dictation')
    const stoppingButton = host.querySelector('button[aria-label="Finishing dictation"]') as HTMLButtonElement
    expect(stoppingButton.dataset.dictationState).toBe('stopping')
    expect(stoppingButton.getAttribute('aria-busy')).toBe('true')
    expect(stoppingButton.querySelector('.uam-dictation-listening-indicator')).toBeNull()

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    expect(textarea.value).toBe('Please write tests')

    await act(async () => {
      ;(host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
      completeDictationStart?.()
      await Promise.resolve()
    })
    expect(host.querySelector('button[aria-label="Stop dictation"]')).toBeTruthy()

    await act(async () => {
      ;(host.querySelector('button[aria-label="Send prompt"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(requests.filter((request) => request.action === 'stopDictation')).toHaveLength(2)
    expect(sendAcpPrompt).not.toHaveBeenCalled()

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Please write tests', [])

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('surfaces native dictation permission errors without submitting partial text', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    window.cefQuery = vi.fn(({ onSuccess }) => onSuccess('{}'))
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    act(() => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'error', message: 'Microphone permission was denied.' },
      }))
    })
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Microphone permission was denied.')
    const errorButton = host.querySelector('button[aria-label="Dictation error"]') as HTMLButtonElement
    expect(errorButton.dataset.dictationState).toBe('error')
    expect(errorButton.querySelector('.uam-dictation-listening-indicator')).toBeNull()

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    const retryButton = host.querySelector('button[aria-label="Retry dictation"]') as HTMLButtonElement
    expect(retryButton.dataset.dictationState).toBe('error')
    expect(retryButton.disabled).toBe(false)

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })
})
