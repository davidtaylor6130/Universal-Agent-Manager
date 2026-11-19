import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { CppAppState } from '../../store/cpp/types'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

type CppChat = CppAppState['chats'][number]
const stamp = (seconds: number) => new Date(seconds * 1000).toISOString()

function chat(id: string, extra: Partial<CppChat> = {}): CppChat {
  return { id, title: id, folderId: 'default', providerId: 'gemini-cli', createdAt: stamp(0), updatedAt: stamp(1), ...extra }
}

function state(revision: number): CppAppState {
  return {
    stateRevision: revision,
    folders: [{ id: 'default', title: 'General', directory: '/tmp/project', collapsed: false }],
    resourceCollections: [],
    chats: [
      chat('focused', { messages: [] }),
      chat('structured', { acpSession: { sessionId: 'native-structured', running: true, processing: true, lifecycleState: 'processing', protocolKind: 'gemini-acp', turnUserMessageIndex: 0, turnAssistantMessageIndex: 1, turnSerial: 1, turnEvents: [] } }),
      chat('cli', { cliTerminal: { terminalId: 'term-cli', sourceChatId: 'cli', running: true, lifecycleState: 'busy', turnState: 'busy', processing: true, readySinceLastSelect: false, active: false, lastError: '' } }),
    ],
    selectedChatId: 'focused',
    providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'cli' }],
    settings: { activeProviderId: 'gemini-cli', theme: 'dark', memoryEnabledDefault: true, memoryIdleDelaySeconds: 60,
      memoryRecallBudgetBytes: 2048, goalMaxLoopIterations: 200, updateChecksEnabled: true } as CppAppState['settings'],
    statusLine: '',
  } as CppAppState
}

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
  delete (window as { cefQuery?: unknown }).cefQuery
})

describe('concurrent streaming keeps another chat composer idle', () => {
  it('does not re-render the focused ChatView for background CLI output, stream tokens or status patches', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    ;(window as { cefQuery?: unknown }).cefQuery = ({ onSuccess }: { onSuccess: (response: string) => void }) => onSuccess('{}')
    vi.resetModules()
    const { useAppStore } = await import('../../store/useAppStore')
    const { ChatView } = await import('./ChatView')
    const { AppShell } = await import('../layout/AppShell')
    useAppStore.getState().loadFromCef(state(1))
    useAppStore.setState({ refreshUamAgents: vi.fn(async () => true), loadSessionMessages: vi.fn(async () => true) })

    let renders = 0
    function Pane() {
      const session = useAppStore((s) => s.sessions.find((candidate) => candidate.id === 'focused'))!
      return <Profiler id="focused" onRender={() => { renders += 1 }}><ChatView session={session} /></Profiler>
    }
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    let shellCommits = 0
    let shellMs = 0
    await act(async () => root.render(<><Profiler id="shell" onRender={(_id, _phase, duration) => { shellCommits += 1; shellMs += duration }}><AppShell /></Profiler><Pane /></>))
    expect(host.querySelector('.uam-composer-textarea')).not.toBeNull()

    vi.useFakeTimers()
    const push = (window as { uamPush?: (payload: unknown) => void }).uamPush!
    const baseline = renders
    shellCommits = 0
    shellMs = 0
    for (let index = 0; index < 100; index += 1) {
      act(() => {
        push({ type: 'cliOutput', sessionId: 'cli', sourceChatId: 'cli', terminalId: 'term-cli', data: btoa(`line ${index}\r\n`) })
        push({ type: 'streamToken', chatId: 'structured', token: `token ${index} `, messageIndex: 1 })
        if (index % 5 === 0) {
          push({ type: 'statePatch', data: { stateRevision: 2 + index, chats: [{ ...state(1).chats[1], updatedAt: stamp(2 + index) }] } })
        }
        vi.advanceTimersByTime(50)
      })
    }
    const backgroundRenders = renders - baseline
    process.stderr.write(`[perf] focused ChatView renders during 100 background bursts: ${backgroundRenders}; shell commits ${shellCommits}, shell render ${shellMs.toFixed(1)}ms`)
    expect(useAppStore.getState().cliTranscriptBySessionId.cli?.content).toContain('line 99')
    expect(useAppStore.getState().messages.structured?.at(-1)?.content).toContain('token 99')
    expect(backgroundRenders).toBe(0)
    act(() => root.unmount())
    host.remove()
  })
})
