import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { AttachmentList, PersistedMessageContent, ThinkingBlock, TurnTimelineContent } from './MessageBlocks'
import { ToolCallModal } from './ToolCallViews'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('AttachmentList', () => {
  it('renders Skills context separately from ordinary files', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AttachmentList attachments={[
      { id: '/tmp/review.uam', name: 'review.uam', type: 'markdown-store', size: 0, path: '/tmp/review.uam' },
      { id: 'diagram', name: 'diagram.png', type: 'image', size: 10, path: '/tmp/diagram.png' },
    ]} />))

    const markdownContext = host.querySelector('[aria-label="Skills context"]')
    const files = host.querySelector('[aria-label="File attachments"]')
    expect(markdownContext?.textContent).toContain('Skillsreview.uam')
    expect(markdownContext?.textContent).not.toContain('/tmp/review.uam')
    expect(files?.textContent).toContain('/tmp/diagram.pngimage')

    act(() => root.unmount())
    host.remove()
  })
})

describe('working transcript', () => {
  const tools = [{
    id: 'tool-1',
    title: '/bin/zsh -lc "rg TODO src"',
    kind: 'shell',
    status: 'completed',
    content: 'No matches',
  }]

  const renderTimeline = (workingMode: 'compact' | 'verbose', active = false, prioritySteerText?: string) => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <TurnTimelineContent
        events={[
          { type: 'assistant_text', text: 'I will inspect the workspace first.' },
          { type: 'thought', text: 'Checking the code paths.' },
          { type: 'tool_call', toolCallId: 'tool-1' },
          { type: 'assistant_text', text: 'The workspace is clean.' },
        ]}
        tools={active ? [{ ...tools[0], status: 'in_progress' }] : tools}
        pendingPermission={null}
        pendingUserInput={null}
        onSelectTool={vi.fn()}
        onResolvePermission={vi.fn()}
        onResolveUserInput={vi.fn()}
        onCancelTurn={vi.fn()}
        onStopRuntime={vi.fn()}
        workingMode={workingMode}
        workedSeconds={83}
        active={active}
		prioritySteerText={prioritySteerText}
      />
    ))
    return { host, root }
  }

  it('collapses completed thinking and tools into a compact worked summary', () => {
    const { host, root } = renderTimeline('compact')
    const summary = host.querySelector('[data-testid="working-summary"]') as HTMLDetailsElement | null

    expect(summary?.open).toBe(false)
    expect(summary?.textContent).toContain('Worked for 1m 23s')
    expect(summary?.querySelector('.uam-working-summary__last')?.textContent).toBe('/bin/zsh -lc "rg TODO src" · completed')
    expect(summary?.textContent).not.toContain('I will inspect the workspace first.')
    expect(summary?.textContent).not.toContain('The workspace is clean.')
    expect(summary?.querySelector('[data-testid="thinking-block"]')).toBeNull()
    expect(summary?.querySelector('.uam-tool-row')).toBeNull()

    act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(summary?.open).toBe(true)
    expect(summary?.querySelector('[data-testid="thinking-block"]')?.textContent).toContain('Checking the code paths.')
    expect(summary?.querySelector('.uam-tool-row')?.textContent).toContain('/bin/zsh')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps only the latest assistant update outside compact working', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <PersistedMessageContent
        message={{
          id: 'message-1',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Finished.',
          createdAt: new Date(),
          processingTimeMs: 83_000,
          blocks: [
            { type: 'thought', text: 'First thought.\nLatest reasoning update.' },
            { type: 'assistant_text', text: 'Interim progress.' },
            { type: 'tool_call', toolCallId: 'tool-1' },
            { type: 'assistant_text', text: 'Finished.' },
          ],
          toolCalls: tools,
        }}
        workingMode="compact"
        onSelectTool={vi.fn()}
      />
    ))

    const summary = host.querySelector('[data-testid="working-summary"]') as HTMLDetailsElement | null
    expect(summary?.textContent).toContain('Worked for 1m 23s')
    expect(summary?.querySelector('.uam-working-summary__last')?.textContent).toBe('/bin/zsh -lc "rg TODO src" · completed')
    expect(host.textContent).toContain('Finished.')
    expect(summary?.querySelector('[data-testid="thinking-block"]')).toBeNull()
    expect(summary?.querySelector('.uam-tool-row')).toBeNull()

    act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(summary?.textContent).toContain('Latest reasoning update.')
    expect(summary?.textContent).toContain('Interim progress.')
    expect(summary?.textContent).toContain('/bin/zsh')

    act(() => root.unmount())
    host.remove()
  })

  it('places legacy persisted work before the final assistant response', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <PersistedMessageContent
        message={{
          id: 'message-legacy',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Final answer.',
          thoughts: 'Checked the state.',
          toolCalls: tools,
          createdAt: new Date(),
          processingTimeMs: 12_000,
        }}
        workingMode="compact"
        onSelectTool={vi.fn()}
      />
    ))

    const text = host.textContent ?? ''
    expect(text.indexOf('Worked for 12s')).toBeLessThan(text.indexOf('Final answer.'))
    expect(host.querySelector('[data-testid="working-summary"] [data-testid="thinking-block"]')).toBeNull()
    expect(host.querySelector('[data-testid="working-summary"] .uam-tool-row')).toBeNull()

    const summary = host.querySelector('[data-testid="working-summary"]') as HTMLDetailsElement | null
    act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(summary?.querySelector('[data-testid="thinking-block"]')).toBeTruthy()
    expect(summary?.querySelector('.uam-tool-row')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps compact work expanded while active', () => {
    const { host, root } = renderTimeline('compact', true, 'Change direction now.')
    const summary = host.querySelector('[data-testid="working-summary"]') as HTMLDetailsElement | null

	expect(summary?.open).toBe(true)
	expect(summary?.querySelector('[data-testid="thinking-block"]')).toBeTruthy()
	expect(summary?.querySelector('.uam-tool-row')?.textContent).toContain('/bin/zsh')
	expect(summary?.querySelector('[data-testid="priority-steer"]')?.textContent).toContain('Change direction now.')
	act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
	expect(summary?.open).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('preserves chronological thinking and tool rows in verbose mode', () => {
    const { host, root } = renderTimeline('verbose')

    expect(host.querySelector('[data-testid="working-summary"]')).toBeNull()
    expect(host.querySelector('[data-testid="thinking-block"]')).toBeTruthy()
    expect(host.querySelector('.uam-tool-row')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('renders thinking with the same row language as tools', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ThinkingBlock text="Inspecting state." />))

    expect(host.querySelector('.uam-thinking-row')).toBeTruthy()
    expect(host.querySelector('.uam-thinking-row__icon')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('animates only the current thinking event while a turn is active', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <TurnTimelineContent
        events={[
          { type: 'thought', text: 'Earlier thought.' },
          { type: 'tool_call', toolCallId: 'tool-1' },
          { type: 'thought', text: 'Current thought.' },
        ]}
        tools={tools}
        pendingPermission={null}
        pendingUserInput={null}
        onSelectTool={vi.fn()}
        onResolvePermission={vi.fn()}
        onResolveUserInput={vi.fn()}
        onCancelTurn={vi.fn()}
        onStopRuntime={vi.fn()}
        active
      />
    ))

    const thoughts = host.querySelectorAll('[data-testid="thinking-block"]')
    expect(thoughts).toHaveLength(2)
    expect((thoughts[0] as HTMLElement).dataset.active).toBe('false')
    expect((thoughts[1] as HTMLElement).dataset.active).toBe('true')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps tool details compact and closes them with Escape', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onClose = vi.fn()
    act(() => root.render(<ToolCallModal tool={{ ...tools[0], content: 'ok\\n\\u001b[31merror\\u001b[0m' }} onClose={onClose} />))

    expect(document.body.querySelector('.uam-tool-modal')).toBeTruthy()
    const output = document.body.querySelector('.uam-tool-modal__output')
    expect(output?.textContent).toContain('error')
    expect(output?.textContent).not.toContain('\\u001b')
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
    expect(onClose).toHaveBeenCalledOnce()

    act(() => root.unmount())
    host.remove()
  })

  it('loads deferred persisted tool output only when its modal opens', async () => {
    const previousCefQuery = window.cefQuery
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({
        action: 'getToolCallContent',
        payload: { chatId: 'chat-1', toolCallId: 'tool-1', offset: 0 },
      })
      onSuccess(JSON.stringify({
        content: 'Loaded on demand', offset: 0, nextOffset: 16, previousOffset: 0,
        lastOffset: 0, totalBytes: 16, hasPrevious: false, hasMore: false,
      }))
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(
        <ToolCallModal
          tool={{ ...tools[0], content: '', contentDeferred: true }}
          chatId="chat-1"
          onClose={vi.fn()}
        />
      )
      await Promise.resolve()
    })

    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('Loaded on demand')

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('keeps loaded deferred chunks in one continuous output', async () => {
    const previousCefQuery = window.cefQuery
    const offsets: number[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const offset = JSON.parse(request).payload.offset as number
      offsets.push(offset)
      if (offset === 0) {
        onSuccess(JSON.stringify({ content: 'FIRST_CHUNK', offset: 0, nextOffset: 131072, previousOffset: 0, lastOffset: 262144, totalBytes: 393216, hasPrevious: false, hasMore: true }))
      } else if (offset === 131072) {
        onSuccess(JSON.stringify({ content: 'SECOND_CHUNK', offset: 131072, nextOffset: 262144, previousOffset: 0, lastOffset: 262144, totalBytes: 393216, hasPrevious: true, hasMore: true }))
      } else {
        onSuccess(JSON.stringify({ content: 'LATEST_CHUNK', offset: 262144, nextOffset: 393216, previousOffset: 131072, lastOffset: 262144, totalBytes: 393216, hasPrevious: true, hasMore: false }))
      }
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<ToolCallModal tool={{ ...tools[0], content: '', contentDeferred: true }} chatId="chat-1" onClose={vi.fn()} />)
      await Promise.resolve()
    })

    const button = (label: string) => Array.from(document.body.querySelectorAll('button')).find((candidate) => candidate.textContent === label) as HTMLButtonElement
    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('FIRST_CHUNK')
    await act(async () => { button('Load later').click(); await Promise.resolve() })
    const output = document.body.querySelector('.uam-tool-modal__output')?.textContent ?? ''
    expect(output.indexOf('FIRST_CHUNK')).toBeLessThan(output.indexOf('SECOND_CHUNK'))
    await act(async () => { button('Load latest').click(); await Promise.resolve() })
    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('LATEST_CHUNK')
    expect(document.body.querySelectorAll('.uam-tool-modal__output')).toHaveLength(1)
    expect(document.body.textContent).toContain('Bytes 262145–393216 of 393216')
    expect(offsets).toEqual([0, 131072, Number.MAX_SAFE_INTEGER])

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('opens live deferred output at the latest chunk', async () => {
    const previousCefQuery = window.cefQuery
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request).payload.offset).toBe(Number.MAX_SAFE_INTEGER)
      onSuccess(JSON.stringify({ content: 'LIVE_TAIL', offset: 262144, nextOffset: 393216, previousOffset: 131072, lastOffset: 262144, totalBytes: 393216, hasPrevious: true, hasMore: false }))
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<ToolCallModal tool={{ ...tools[0], status: 'running', content: '', contentDeferred: true }} chatId="chat-1" onClose={vi.fn()} />)
      await Promise.resolve()
    })

    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('LIVE_TAIL')

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('retries a failed deferred chunk request', async () => {
    const previousCefQuery = window.cefQuery
    let requests = 0
    window.cefQuery = ({ onSuccess, onFailure }) => {
      ++requests
      if (requests === 1) {
        onFailure(500, 'Chunk unavailable.')
        return
      }
      onSuccess(JSON.stringify({ content: 'RECOVERED_CHUNK', offset: 0, nextOffset: 15, previousOffset: 0, lastOffset: 0, totalBytes: 15, hasPrevious: false, hasMore: false }))
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<ToolCallModal tool={{ ...tools[0], content: '', contentDeferred: true }} chatId="chat-1" onClose={vi.fn()} />)
      await Promise.resolve()
    })
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('Chunk unavailable.')
    const retry = Array.from(document.body.querySelectorAll('button')).find((candidate) => candidate.textContent === 'Retry') as HTMLButtonElement
    await act(async () => { retry.click(); await Promise.resolve() })
    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('RECOVERED_CHUNK')

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('opens a validated managed-agent transcript without adding it to the chat list', async () => {
    const previousCefQuery = window.cefQuery
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'resumeAgentRun') {
        expect(parsed.payload).toEqual({ runId: 'run-old' })
        onSuccess(JSON.stringify({ runId: 'run-fresh' }))
        return
      }
      expect(parsed).toMatchObject({ action: 'getManagedAgentTranscript', payload: { chatId: 'chat-1', transcriptChatId: 'managed-chat-1' } })
      onSuccess(JSON.stringify({
        runId: 'run-old', title: 'Reviewer run', status: 'interrupted',
        messages: [{ role: 'user', content: 'Review this change.' }, { role: 'assistant', content: 'The change is stable.' }],
      }))
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <ToolCallModal
        tool={{ ...tools[0], content: '{"ok":true,"result":{"transcriptChatId":"managed-chat-1"}}' }}
        chatId="chat-1"
        onClose={vi.fn()}
      />
    ))
    const open = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'View managed agent transcript') as HTMLButtonElement
    await act(async () => open.click())

    const transcript = document.body.querySelector('[aria-label="Managed agent transcript"]')
    expect(transcript?.textContent).toContain('Reviewer run')
    expect(transcript?.textContent).toContain('The change is stable.')
    const resume = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Resume as fresh run') as HTMLButtonElement
    await act(async () => resume.click())
    expect(transcript?.textContent).toContain('Fresh run queued: run-fresh')

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('resolves a running subtask once and refreshes only its messages afterwards', async () => {
    vi.useFakeTimers()
    const openSubAgentSession = vi.fn(async () => 'child-chat')
    const loadSessionMessages = vi.fn()
    useAppStore.setState({
      sessions: [
        { id: 'parent-chat', name: 'Parent', viewMode: 'chat', folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() },
        { id: 'child-chat', name: 'Child', viewMode: 'chat', folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() },
      ],
      messages: { 'child-chat': [] },
      providers: [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' }],
      openSubAgentSession,
      loadSessionMessages,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(
        <PersistedMessageContent
          message={{
            id: 'message-subtask',
            sessionId: 'parent-chat',
            role: 'assistant',
            content: '',
            createdAt: new Date(),
            blocks: [{ type: 'tool_call', toolCallId: 'subtask-1' }],
            toolCalls: [{ id: 'subtask-1', kind: 'sub-agent', title: 'Review', status: 'running', content: '', isSubAgent: true, subAgentId: 'native-child' }],
          }}
          workingMode="verbose"
          sourceChatId="parent-chat"
          onSelectTool={vi.fn()}
        />
      )
      await Promise.resolve()
    })
    const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
    await act(async () => {
      panel.open = true
      panel.dispatchEvent(new Event('toggle', { bubbles: true }))
      await Promise.resolve()
    })
    expect(openSubAgentSession).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(3_000)
    })
    expect(openSubAgentSession).toHaveBeenCalledTimes(1)
    expect(loadSessionMessages).toHaveBeenCalledWith('child-chat')

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })
})
