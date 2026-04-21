import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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
    expect(host.textContent).toContain('ACP')
    expect(host.textContent).toContain('Workspace')
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
    expect(host.textContent).toContain('Provider')
    expect(host.textContent).toContain('Gemini')

    const settingsButton = host.querySelector('button[title="Settings"]')
    expect(settingsButton).toBeTruthy()
    act(() => {
      settingsButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.textContent).toContain('Chat settings')
    expect(host.textContent).toContain('Unavailable')

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Tool call:')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.textContent).toContain('Searching workspace symbols')
    expect(host.querySelector('[role="dialog"]')?.parentElement?.className).toContain('absolute')
    expect(host.querySelector('[role="dialog"]')?.parentElement?.className).not.toContain('fixed')

    const closeToolButton = host.querySelector('button[title="Close tool details"]')
    expect(closeToolButton).toBeTruthy()
    act(() => {
      closeToolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.textContent).not.toContain('Searching workspace symbols')

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
    expect(host.textContent).toContain('App Server')
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
    expect((host.querySelector('button[title="Toggle planning mode"]') as HTMLButtonElement | null)?.disabled).toBe(true)

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

    const planButton = host.querySelector('button[title="Toggle planning mode"]') as HTMLButtonElement | null
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

  it('renders pipe tables and leaves malformed table text alone', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'table-message',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '| Tool | Status |\n| --- | :---: |\n| `rg` | **ok** |\n\n| Bad | Row |\n| nope |',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
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
          turnEvents: [],
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

    expect(host.querySelectorAll('table')).toHaveLength(1)
    expect(host.querySelector('th')?.textContent).toBe('Tool')
    expect(host.querySelector('td code')?.textContent).toBe('rg')
    expect(host.querySelector('td strong')?.textContent).toBe('ok')
    expect(host.textContent).toContain('| Bad | Row |')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

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
    expect(host.textContent).toContain('Saved tool output')
    expect(host.querySelector('[role="dialog"]')).toBeTruthy()

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
    expect(host.textContent).toContain('Searching workspace symbols')

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
    expect(host.querySelector('[role="dialog"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
