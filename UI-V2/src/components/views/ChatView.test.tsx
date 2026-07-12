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

describe('ChatView', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
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
            options: [{ id: 'allow-once', name: 'Allow once', kind: 'allow_once' }],
          },
          pendingUserInput: null,
          agentInfo: { name: 'gemini', title: 'Gemini CLI', version: '0.36.0' },
        },
      },
    })
  })

  it('uses the composer action as Stop while the runtime is processing', async () => {
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ stopAcpSession })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const stopButton = Array.from(host.querySelectorAll('button')).find((button) => button.title === 'Stop runtime') as HTMLButtonElement
    expect(stopButton).toBeTruthy()
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
    expect(host.textContent).toContain('Tool call:')
    expect(host.textContent).toContain('Search symbols')
    expect(host.textContent).toContain('Thinking')
    expect(host.textContent).toContain('Need to inspect the workspace first.')
    expect(host.textContent).not.toContain('Persisted thought should not duplicate while turn events are active.')
    expect(host.textContent).toContain('After tool.')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).not.toContain('ACP')
    expect(host.querySelector('button[aria-label="Select provider"]')).toBeTruthy()
    expect(host.textContent).toContain('/tmp/project')
    expect(host.textContent).not.toContain('Tools on')
    expect(host.textContent).toContain('Read file')

    const streamText = host.textContent ?? ''
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Tool call:'))
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Thinking'))
    expect(streamText.indexOf('Thinking')).toBeLessThan(streamText.indexOf('Tool call:'))
    expect(streamText.indexOf('Tool call:')).toBeLessThan(streamText.indexOf('Read file'))
    expect(streamText.indexOf('Read file')).toBeLessThan(streamText.indexOf('After tool.'))
    const thinkingBlock = host.querySelector('[data-testid="thinking-block"]') as HTMLDetailsElement | null
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)
    expect(thinkingBlock?.tagName).toBe('DETAILS')
    expect(thinkingBlock?.textContent).toContain('Thinking')
    expect(thinkingBlock?.textContent).toContain('Need to inspect the workspace first.')
    expect(thinkingBlock?.hasAttribute('open')).toBe(true)
    expect(host.querySelectorAll('details')).toHaveLength(1)

    const providerButton = host.querySelector('button[title="Select provider"]')
    expect(providerButton).toBeTruthy()
    act(() => {
      providerButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    const providerMenu = document.body.querySelector('[data-testid="provider-menu"]') as HTMLElement
    expect(providerMenu.textContent).toContain('Provider')
    expect(providerMenu.textContent).toContain('Gemini')
    expect(providerMenu.style.position).toBe('fixed')

    const settingsButton = host.querySelector('button[title="Settings"]')
    expect(settingsButton).toBeTruthy()
    act(() => {
      settingsButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.textContent).toContain('Chat settings')
    expect(host.textContent).not.toContain('Unavailable')

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Tool call:')
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
    expect(host.textContent).toContain('CLI default')
    expect(host.querySelector('textarea')?.getAttribute('placeholder')).toBe('Message Codex')

    act(() => {
      root.unmount()
    })
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

    const modelButton = host.querySelector('button[title="Select model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('Model')
    expect(modelButton?.textContent).toContain('gpt-5.4')

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('CLI default')
    expect(host.textContent).toContain('Use Codex CLI settings')
    expect(host.textContent).toContain('gpt-5.4')
    expect(host.textContent).toContain('GPT-5.4-Mini')
    expect(host.textContent).not.toContain('Auto 3')
    expect(host.textContent).not.toContain('Flash Lite')

    const miniButton = Array.from(host.querySelectorAll('button')).find((button) =>
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

    const modelButton = host.querySelector('button[title="Select model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('Model')

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('CLI default')
    expect(host.textContent).toContain('Use OpenCode CLI settings')
    expect(host.textContent).toContain('Qwen3.6 35B A3B Q4')
    expect(host.textContent).toContain('Qwen3 Coder 30B')
    expect(host.textContent).not.toContain('Auto 3')
    expect(host.textContent).not.toContain('Flash Lite')

    const coderButton = Array.from(host.querySelectorAll('button')).find((button) =>
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

    const modelButton = host.querySelector('button[title="Select model"]')
    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('DeepSeek V4 Flash Free')
    expect(host.textContent).toContain('OpenCode Zen free model.')
    expect(host.textContent).toContain('Big Pickle')
    expect(host.textContent).not.toContain('Auto 3')

    const freeButton = Array.from(host.querySelectorAll('button')).find((button) =>
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

    const modelButton = host.querySelector('button[title="Select model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('Model')
    expect(modelButton?.textContent).toContain('Auto 3')

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Auto 3')
    expect(host.textContent).toContain('Gemini 3 Flash')

    const flashButton = Array.from(host.querySelectorAll('button')).find((button) =>
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

    const modelButton = host.querySelector('button[title="Select model"]') as HTMLButtonElement | null
    expect(modelButton).toBeTruthy()
    expect(modelButton?.disabled).toBe(true)
    openComposerOptions(host)
    expect((host.querySelector('button[title^="Toggle planning mode"]') as HTMLButtonElement | null)?.disabled).toBe(true)
    expect((host.querySelector('button[title="Toggle Yolo mode"]') as HTMLButtonElement | null)?.disabled).toBe(false)

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

  it('toggles the planning chip and reflects runtime plan state', () => {
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
    const planButton = host.querySelector('button[title^="Toggle planning mode"]') as HTMLButtonElement | null
    expect(planButton).toBeTruthy()
    expect(planButton?.disabled).toBe(false)
    expect(planButton?.getAttribute('aria-pressed')).toBe('false')

    act(() => {
      planButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

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

    expect(planButton?.getAttribute('aria-pressed')).toBe('true')

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

    act(() => {
      planButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenLastCalledWith('chat-1', 'default')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('toggles the Yolo chip without changing Plan mode', () => {
    const setSessionAutoApproveCommands = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, approvalMode: 'plan', autoApproveCommands: false } : session
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
      setSessionAutoApproveCommands,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const yoloButton = host.querySelector('button[title="Toggle Yolo mode"]') as HTMLButtonElement | null
    expect(yoloButton).toBeTruthy()
    expect(yoloButton?.disabled).toBe(false)
    expect(yoloButton?.getAttribute('aria-pressed')).toBe('false')

    act(() => {
      yoloButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionAutoApproveCommands).toHaveBeenCalledWith('chat-1', true)

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, autoApproveCommands: true } : session
        ),
      }))
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(yoloButton?.getAttribute('aria-pressed')).toBe('true')

    act(() => {
      yoloButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionAutoApproveCommands).toHaveBeenLastCalledWith('chat-1', false)

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

    openComposerOptions(host)
    const planButton = host.querySelector('button[title^="Toggle planning mode"]') as HTMLButtonElement | null
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

  it('toggles Claude Accept Edits mode', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'default' } : session
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
    const acceptEditsButton = host.querySelector('button[title="Toggle Accept Edits mode. Claude can edit workspace files without prompting."]') as HTMLButtonElement | null
    const autoButton = host.querySelector('button[title="Toggle Auto mode"]') as HTMLButtonElement | null
    expect(acceptEditsButton).toBeTruthy()
    expect(acceptEditsButton?.disabled).toBe(false)
    expect(acceptEditsButton?.textContent).toContain('Accept Edits')
    expect(autoButton?.textContent).toContain('Auto')

    act(() => {
      acceptEditsButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'acceptEdits')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, approvalMode: 'acceptEdits' } : session
        ),
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            currentModeId: 'acceptEdits',
          },
        },
      }))
    })

    act(() => {
      acceptEditsButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenLastCalledWith('chat-1', 'default')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('asks how to proceed before sending a prompt from Claude plan mode', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
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

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'acceptEdits')
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
    const goalButton = host.querySelector('button[title="Use the next message as a goal"]') as HTMLButtonElement | null
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

    expect(setGoal).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', 1234)
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', [])

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders active goal pause/delete controls and paused goal resume control', async () => {
    const updateGoalStatus = vi.fn(() => Promise.resolve(true))
    const removeGoal = vi.fn(() => Promise.resolve(true))
    const resumeGoal = vi.fn(() => Promise.resolve(true))
    const goal = {
      id: 'goal-1',
      chatId: 'chat-1',
      objective: 'Review the Ralph loop',
      status: 'active' as const,
      tokenBudget: 0,
      tokensUsed: 0,
      blockedTurnCount: 0,
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

    // Goal actions are consolidated into an overflow menu; open it to reach them.
    const openGoalMenu = () => act(() => {
      (host.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
      ;(host.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    const menuItem = (text: string) =>
      Array.from(host.querySelectorAll('button')).find((b) => b.textContent === text) as HTMLButtonElement | undefined

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
    act(() => {
      menuItem('Delete goal')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(removeGoal).toHaveBeenCalledWith('goal-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('toggles the memory chip', () => {
    const setSessionMemoryEnabled = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, memoryEnabled: true } : session
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
      setSessionMemoryEnabled,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const memoryButton = host.querySelector('button[title="Toggle memory"]') as HTMLButtonElement | null
    expect(memoryButton).toBeTruthy()
    expect(memoryButton?.disabled).toBe(false)
    expect(memoryButton?.getAttribute('aria-pressed')).toBe('true')

    act(() => {
      memoryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionMemoryEnabled).toHaveBeenCalledWith('chat-1', false)

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
    expect(host.querySelectorAll('details')).toHaveLength(1)

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

    expect(host.textContent).toContain('Tool call:')
    expect(host.textContent).toContain('Read saved file')
    expect(host.textContent).toContain('completed')

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Read saved file')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Saved tool output')
    expect(document.body.querySelector('[role="dialog"]')).toBeTruthy()

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
    expect(plan).toBeGreaterThan(finalVisible)
    expect(text).not.toContain('Grouped content should not render.')
    expect(text).not.toContain('Grouped thought should not render.')
    expect(text.match(/Tool call:/g) ?? []).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders goal review JSON as a goal review block', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  content: '{"decision":"continue","reason":"More work remains.","nextPrompt":"Continue with the next page."}',
                  thoughts: '',
                  toolCalls: [],
                  planSummary: '',
                  planEntries: [],
                  blocks: [],
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

    expect(host.querySelector('[data-testid="goal-review-block"]')).not.toBeNull()
    expect(host.textContent).toContain('Goal Review')
    expect(host.textContent).toContain('Continue')
    expect(host.textContent).toContain('More work remains.')
    expect(host.textContent).toContain('Next:')

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
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
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

  it('hides plan actions for historical Codex plans after a later user message', () => {
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

    expect(host.textContent).toContain('Historical plan.')
    expect(host.textContent).toContain('Old step')
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
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const form = host.querySelector('form')
      expect(form?.textContent).toContain('Gemini ACP error')
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
      button.textContent?.includes('Tool call:')
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

  it('expands a sub-agent chat inline without replacing the current chat', async () => {
    const originalOpenSubAgentSession = useAppStore.getState().openSubAgentSession
    const openSubAgentSession = vi.fn(() => Promise.resolve('agent-chat'))
    useAppStore.setState((state) => ({
      openSubAgentSession,
      sessions: [
        ...state.sessions,
        { ...state.sessions[0], id: 'agent-chat', name: 'Planner history' },
      ],
      messages: {
        ...state.messages,
        'agent-chat': [
          {
            id: 'agent-message-1',
            sessionId: 'agent-chat',
            role: 'assistant',
            content: 'Sub-agent inspected the provider runtime.',
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
          ],
          turnEvents: [
            { type: 'assistant_text', text: 'I am delegating to a sub-agent.' },
            { type: 'tool_call', toolCallId: 'agent-tool-1' },
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

    const summary = Array.from(host.querySelectorAll('summary')).find((candidate) => candidate.textContent?.includes('Sub-agent:'))
    expect(summary).toBeTruthy()

    await act(async () => {
      const details = summary?.closest('details') as HTMLDetailsElement
      details.open = true
      details.dispatchEvent(new Event('toggle', { bubbles: true }))
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'agent-session-1', 'Planner', false)
    expect(host.textContent).toContain('Planner history')
    expect(host.textContent).toContain('Sub-agent inspected the provider runtime.')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ openSubAgentSession: originalOpenSubAgentSession })
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

    const openButton = host.querySelector('button[aria-label="Open workspace in Finder or File Explorer"]') as HTMLButtonElement | null
    expect(openButton).toBeTruthy()
    expect(openButton?.disabled).toBe(false)

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

    const discardButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Discard & return') as HTMLButtonElement | undefined
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

    expect(host.textContent).toContain('/tmp/project')
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

    const discardButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Discard & return') as HTMLButtonElement | undefined
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

  it('disables the workspace open button when no workspace is selected', () => {
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

    const openButton = host.querySelector('button[aria-label="Open workspace in Finder or File Explorer"]') as HTMLButtonElement | null
    expect(openButton).toBeTruthy()
    expect(openButton?.disabled).toBe(true)

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
    expect(host.querySelector('button[title="Select provider"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('hides the provider selector when Gemini is the only available provider for the chat', () => {
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

    expect(host.querySelector('button[title="Select provider"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
