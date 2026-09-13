import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { AttachmentList, PersistedMessageContent, ThinkingBlock, TurnTimelineContent } from './MessageBlocks'
import { ToolCallModal } from './ToolCallViews'
import { ConversationWork } from './ConversationWork'
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

  const renderTimeline = (workingMode: 'compact' | 'verbose', active = false) => {
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
      />
    ))
    return { host, root }
  }

  it('keeps commentary before the following tool while streaming and after persistence', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const events = [{ type: 'assistant_text' as const, text: 'Before the command.' }, { type: 'tool_call' as const, toolCallId: 'tool-1' }]
    act(() => root.render(<TurnTimelineContent events={events} tools={tools} interrupted workingMode="compact"
      pendingPermission={null} pendingUserInput={null} onSelectTool={vi.fn()} onResolvePermission={vi.fn()}
      onResolveUserInput={vi.fn()} onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
    expect(host.textContent!.indexOf('Before the command.')).toBeLessThan(host.textContent!.indexOf('/bin/zsh'))
    expect(host.textContent!.indexOf('/bin/zsh')).toBeLessThan(host.textContent!.indexOf('Response interrupted'))
    act(() => root.render(<PersistedMessageContent message={{ id: 'ordered', sessionId: 'chat-1', role: 'assistant',
      content: 'Before the command.', createdAt: new Date(), blocks: events, toolCalls: tools }} workingMode="compact" onSelectTool={vi.fn()} />))
    expect(host.textContent).toContain('/bin/zsh')
    expect(host.textContent!.indexOf('Before the command.')).toBeLessThan(host.textContent!.indexOf('/bin/zsh'))
    act(() => root.unmount())
  })

  it('keeps completed compact work visible in chronological order', () => {
    const { host, root } = renderTimeline('compact')
    expect(host.textContent).toContain('Worked for 1m 23s')
    const text = host.textContent!
    expect(text.indexOf('I will inspect')).toBeLessThan(text.indexOf('Thoughts'))
    expect(text.indexOf('Thoughts')).toBeLessThan(text.indexOf('/bin/zsh'))
    expect(text.indexOf('/bin/zsh')).toBeLessThan(text.indexOf('The workspace is clean.'))
    expect(host.querySelector('[data-processing-step="true"]')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it.each(['compact', 'verbose'] as const)('collapses %s work while retaining only the final commentary and pending permission', (workingMode) => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const render = (active: boolean) => act(() => root.render(<TurnTimelineContent
      events={[{ type: 'assistant_text', text: 'Before the command.' }, { type: 'assistant_text', text: 'Intermediate update.' },
        { type: 'thought', text: 'Check the command.' }, { type: 'tool_call', toolCallId: 'tool-1' },
        { type: 'assistant_text', text: 'After the command.' }, { type: 'assistant_text', text: '   ' }]}
      tools={tools} active={active} workingMode={workingMode}
      pendingPermission={{ requestId: 'permission-1', toolCallId: 'tool-1', title: 'Allow this command?', kind: 'shell', status: 'pending', content: '', options: [{ id: 'allow', name: 'Allow once', kind: 'allow_once' }] }}
      pendingUserInput={null} onSelectTool={vi.fn()} onResolvePermission={vi.fn()} onResolveUserInput={vi.fn()}
      onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
    render(true)
    expect(host.textContent).toContain('/bin/zsh')
    expect(host.querySelector('button[aria-label="Collapse work trace"]')).toBeNull()
    expect(host.textContent).toContain('Check the command.')
    render(false)
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Collapse work trace"]')!.click())
    expect(host.textContent).not.toContain('/bin/zsh')
    expect(host.textContent).not.toContain('Check the command.')
    expect(host.textContent).toContain('Allow once')
    expect(host.textContent).not.toContain('Before the command.')
    expect(host.textContent).not.toContain('Intermediate update.')
    expect(host.textContent).toContain('After the command.')
    render(false)
    expect(host.querySelector('button[aria-label="Expand work trace"]')?.getAttribute('aria-expanded')).toBe('false')
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand work trace"]')!.click())
    expect(host.textContent!.indexOf('Before the command.')).toBeLessThan(host.textContent!.indexOf('/bin/zsh'))
    expect(host.textContent!.indexOf('/bin/zsh')).toBeLessThan(host.textContent!.indexOf('After the command.'))
    act(() => root.unmount())
  })

  it('uses the saved trace default for persisted blocks until the turn is toggled', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const defaultExpanded = useAppStore.getState().expandWorkTraces
    act(() => useAppStore.setState({ expandWorkTraces: false }))
    act(() => root.render(<PersistedMessageContent message={{ id: 'saved', sessionId: 'chat-1', role: 'assistant',
      content: 'Final answer.', createdAt: new Date(), blocks: [{ type: 'thought', text: 'Saved thought.' },
        { type: 'tool_call', toolCallId: 'tool-1' }, { type: 'assistant_text', text: 'Final answer.' }], toolCalls: tools }}
      workingMode="compact" onSelectTool={vi.fn()} />))
    expect(host.textContent).toContain('Final answer.')
    expect(host.querySelector('.conversation-work__tool')).toBeNull()
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Expand work trace"]')!.click())
    expect(host.querySelector('.conversation-work__tool')).toBeTruthy()
    act(() => useAppStore.setState({ expandWorkTraces: true }))
    act(() => useAppStore.setState({ expandWorkTraces: false }))
    expect(host.querySelector('.conversation-work__tool')).toBeTruthy()
    expect(host.querySelector('.conversation-work__thought > summary')?.textContent).toContain('Saved thought.')
    expect(host.querySelectorAll('.conversation-trace__internal')).toHaveLength(2)
    expect(host.querySelector('.conversation-trace__text')?.textContent).toContain('Final answer.')
    act(() => root.unmount())
    act(() => useAppStore.setState({ expandWorkTraces: defaultExpanded }))
  })

  it.each(['compact', 'verbose'] as const)('collapsed persisted %s work keeps only the last nonempty assistant text', (workingMode) => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const defaultExpanded = useAppStore.getState().expandWorkTraces
    act(() => useAppStore.setState({ expandWorkTraces: false }))
    act(() => root.render(<PersistedMessageContent message={{
      id: `saved-collapse-${workingMode}`, sessionId: 'chat-1', role: 'assistant', content: 'Final answer.', createdAt: new Date(),
      blocks: [
        { type: 'assistant_text', text: 'Earlier answer.' },
        { type: 'thought', text: 'Saved thought.' },
        { type: 'tool_call', toolCallId: 'tool-1' },
        { type: 'assistant_text', text: 'Intermediate answer.' },
        { type: 'assistant_text', text: 'Final answer.' },
        { type: 'assistant_text', text: '   ' },
      ], toolCalls: tools,
    }} workingMode={workingMode} onSelectTool={vi.fn()} />))
    expect(host.textContent).toContain('Final answer.')
    expect(host.textContent).not.toContain('Earlier answer.')
    expect(host.textContent).not.toContain('Intermediate answer.')
    expect(host.querySelector('.conversation-work__tool')).toBeNull()
    act(() => root.unmount())
    host.remove()
    act(() => useAppStore.setState({ expandWorkTraces: defaultExpanded }))
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
    expect(host.querySelector('[data-testid="working-summary"] .conversation-work__reasoning')).toBeNull()
    expect(host.querySelector('[data-testid="working-summary"] .conversation-work__tool')).toBeTruthy()

    const summary = host.querySelector('[data-testid="working-summary"]') as HTMLDetailsElement | null
    act(() => (summary?.querySelector('.conversation-work__thought > summary') as HTMLElement).click())
    expect(summary?.querySelector('.conversation-work__reasoning')).toBeTruthy()
    act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(summary?.querySelector('.conversation-work__tool')).toBeNull()
    expect(host.textContent).toContain('Final answer.')
    act(() => summary?.querySelector('summary')?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(summary?.querySelector('.conversation-work__tool')).toBeTruthy()
    expect(summary?.querySelector<HTMLDetailsElement>('.conversation-work__thought')?.open).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps compact rows visible without animating assistant text', () => {
    const { host, root } = renderTimeline('compact', true)
    expect(host.textContent).toContain('Working 1m 23s')
    expect(host.querySelector('[data-processing-step="true"]')).toBeNull()
    expect(host.querySelector('[aria-label="Processing"]')).toBeNull()
    expect(host.textContent).toContain('I will inspect')
    act(() => root.unmount())
    host.remove()
  })

  it('pulses only the current compact thought or running tool and preserves the plain row callbacks', () => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const onSelectTool = vi.fn()
    const render = (stage: 'thought' | 'tool' | 'waiting' | 'done') => act(() => root.render(<TurnTimelineContent
      events={[{ type: 'assistant_text', text: 'Starting review.' }, { type: 'thought', text: '### Reasoning\nInspecting files.' },
        ...(stage === 'thought' ? [] : [{ type: 'tool_call' as const, toolCallId: 'tool-1' }])]}
      tools={[{ ...tools[0], status: stage === 'done' ? 'completed' : 'in_progress' }]}
      active={stage !== 'done'} workingMode="compact" pendingPermission={stage === 'waiting' ? {
        requestId: 'permission-1', toolCallId: 'tool-1', title: 'Allow this command?', kind: 'shell', status: 'pending', content: '', options: [],
      } : null} pendingUserInput={null}
      onSelectTool={onSelectTool} onResolvePermission={vi.fn()} onResolveUserInput={vi.fn()} onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
    render('thought')
    expect(host.querySelectorAll('[data-processing-step="true"]')).toHaveLength(1)
    expect(host.querySelector('[data-processing-step="true"]')?.textContent).toContain('Thoughts')
    expect(host.querySelector('.conversation-work__reasoning')).toBeNull()
    expect(host.querySelector('.conversation-work__thought > summary')?.textContent).toBe('ThoughtsInspecting files.')
    act(() => (host.querySelector('.conversation-work__thought > summary') as HTMLElement).click())
    expect(host.querySelector('.conversation-work__reasoning h3')?.textContent).toBe('Reasoning')
    expect(host.textContent).not.toContain('###')
    render('tool')
    expect(host.querySelectorAll('[data-processing-step="true"]')).toHaveLength(1)
    const button = host.querySelector<HTMLButtonElement>('.conversation-work__tool')!
    expect(host.querySelector('[data-processing-step="true"]')?.contains(button)).toBe(true)
    act(() => button.click())
    expect(onSelectTool).toHaveBeenCalledExactlyOnceWith('tool-1')
    render('waiting')
    expect(host.querySelector('[data-processing-step="true"]')).toBeNull()
    render('done')
    expect(host.querySelector('[data-processing-step="true"]')).toBeNull()
    expect(host.textContent!.indexOf('Starting review.')).toBeLessThan(host.textContent!.indexOf('Thoughts'))
    expect(host.textContent!.indexOf('Thoughts')).toBeLessThan(host.textContent!.indexOf('/bin/zsh'))
    act(() => root.unmount())
  })

  it('keeps disclosure hooks stable from empty to populated and retains missing, extra, and sub-agent tools', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onSelectTool = vi.fn()
    const subAgent = { ...tools[0], id: 'child', title: 'Review child', isSubAgent: true, subAgentId: 'native-child' }
    const renderSubAgentHistory = vi.fn(() => <p>Child conversation.</p>)
    act(() => root.render(<ConversationWork events={[]} tools={[]} active={false} duration="0s" onSelectTool={onSelectTool} />))
    expect(host.childElementCount).toBe(0)
    act(() => root.render(<ConversationWork
      events={[{ type: 'tool_call', toolCallId: 'missing' }, { type: 'tool_call', toolCallId: 'child' }]}
      tools={[subAgent, ...tools]}
      active
      duration="1s"
      onSelectTool={onSelectTool}
      renderSubAgentHistory={renderSubAgentHistory}
    />))
    const buttons = host.querySelectorAll<HTMLButtonElement>('.conversation-work__tool')
    expect(Array.from(buttons).map((button) => button.textContent)).toEqual([
      'missingpending', 'Review childcompleted', '/bin/zsh -lc "rg TODO src"completed',
    ])
    act(() => buttons[1].click())
    expect(onSelectTool).toHaveBeenCalledExactlyOnceWith('child')
    expect(renderSubAgentHistory).not.toHaveBeenCalled()
    act(() => (host.querySelector('.conversation-work__subagent > summary') as HTMLElement).click())
    expect(renderSubAgentHistory).toHaveBeenCalledWith(subAgent)
    expect(host.querySelector('.conversation-work__history')?.textContent).toBe('Child conversation.')
    act(() => root.render(<ConversationWork events={[]} tools={[]} active={false} duration="2s" onSelectTool={onSelectTool} />))
    expect(host.childElementCount).toBe(0)

    act(() => root.unmount())
    host.remove()
  })

  it('preserves chronological thinking and tool rows in verbose mode', () => {
    const { host, root } = renderTimeline('verbose')

    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toContain('Worked for')
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

  it('refreshes revised deferred results and rejects an older in-flight page', async () => {
    const previousCefQuery = window.cefQuery
    const requests: Array<{ offset: number; finish: (content: string) => void }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      const offset = JSON.parse(request).payload.offset as number
      requests.push({ offset, finish: (content) => onSuccess(JSON.stringify({
        content, offset, nextOffset: offset + 131072, previousOffset: 0,
        lastOffset: 131072, totalBytes: 262144, hasPrevious: offset > 0, hasMore: offset === 0,
      })) })
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const render = (digest: string) => root.render(<ToolCallModal
      tool={{ ...tools[0], status: 'completed', content: '', contentDeferred: true, contentDigest: digest }}
      chatId="chat-1" onClose={vi.fn()} />)
    try {
      await act(async () => { render('first') })
      await act(async () => { requests[0].finish('OLD_FIRST') })
      await act(async () => { render('first') })
      expect(requests).toHaveLength(1)
      await act(async () => {
        const later = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Load later')!
        later.click()
      })
      await act(async () => { render('changed') })
      expect(requests.map((request) => request.offset)).toEqual([0, 131072, 131072])
      await act(async () => { requests[2].finish('NEW_SECOND') })
      await act(async () => { requests[1].finish('OLD_SECOND') })
      const output = document.body.querySelector('.uam-tool-modal__output')?.textContent
      expect(output).toContain('NEW_SECOND')
      expect(output).not.toContain('OLD_')
    } finally {
      act(() => root.unmount())
      host.remove()
      window.cefQuery = previousCefQuery
    }
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
    const tab = Array.from(document.body.querySelectorAll('[role="tab"]')).find((button) => button.textContent === 'Transcript') as HTMLButtonElement
    act(() => tab.click())
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

  it.each(['running', 'completed'])('retries unavailable child history after a slow lookup while %s', async (status) => {
    vi.useFakeTimers()
    let finishLookup!: (value: string | null) => void
    const openSubAgentSession = vi.fn(async (): Promise<string | null> => 'child-chat')
      .mockImplementationOnce(() => new Promise((resolve) => { finishLookup = resolve }))
    const loadSessionMessages = vi.fn(async () => {})
    useAppStore.setState({
      sessions: [{ id: 'child-chat', name: 'Child', viewMode: 'chat', folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() }],
      messages: { 'child-chat': [] }, openSubAgentSession, loadSessionMessages,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const renderChild = (status: string) => act(async () => {
      root.render(<PersistedMessageContent message={{
        id: 'parent-message', sessionId: 'parent-chat', role: 'assistant', content: '', createdAt: new Date(),
        blocks: [{ type: 'tool_call', toolCallId: 'child-tool' }],
        toolCalls: [{ id: 'child-tool', kind: 'sub-agent', title: 'Review', status, content: '', isSubAgent: true, subAgentId: 'native-child' }],
      }} workingMode="verbose" sourceChatId="parent-chat" onSelectTool={vi.fn()} />)
    })
    try {
      await renderChild('running')
      await act(async () => {
        const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
        panel.open = true
        panel.dispatchEvent(new Event('toggle', { bubbles: true }))
      })
      await act(async () => { await vi.advanceTimersByTimeAsync(15_000) })
      expect(openSubAgentSession).toHaveBeenCalledTimes(1)
      await renderChild(status)
      expect(openSubAgentSession).toHaveBeenCalledTimes(1)
      await act(async () => { finishLookup(null) })
      if (status === 'running') {
        await act(async () => { await vi.advanceTimersByTimeAsync(4999) })
        expect(openSubAgentSession).toHaveBeenCalledTimes(1)
        await act(async () => { await vi.advanceTimersByTimeAsync(1) })
      }
      expect(openSubAgentSession).toHaveBeenCalledTimes(2)
      expect(host.querySelector('[aria-label="Subtask transcript: Child"]')).not.toBeNull()
      expect(host.querySelector('[role="alert"]')).toBeNull()
      act(() => root.unmount())
      await act(async () => { await vi.advanceTimersByTimeAsync(15_000) })
      expect(openSubAgentSession).toHaveBeenCalledTimes(2)
      expect(loadSessionMessages).not.toHaveBeenCalled()
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.useRealTimers()
    }
  })

  it('shows active transcript refresh failures and clears them after recovery', async () => {
    vi.useFakeTimers()
    const openSubAgentSession = vi.fn(async (): Promise<string | null> => 'child-chat')
    const loadSessionMessages = vi.fn(async () => true).mockResolvedValueOnce(false)
    useAppStore.setState({
      sessions: [
        { id: 'child-chat', name: 'Child', viewMode: 'chat', folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() },
      ],
      messages: { 'child-chat': [] }, openSubAgentSession, loadSessionMessages,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<PersistedMessageContent message={{
        id: 'parent-message', sessionId: 'parent-chat', role: 'assistant', content: '', createdAt: new Date(),
        blocks: [{ type: 'tool_call', toolCallId: 'child-tool' }],
        toolCalls: [{ id: 'child-tool', kind: 'sub-agent', title: 'Review', status: 'running', content: '', isSubAgent: true, subAgentId: 'native-child' }],
      }} workingMode="verbose" sourceChatId="parent-chat" onSelectTool={vi.fn()} />))
      await act(async () => {
        const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
        panel.open = true
        panel.dispatchEvent(new Event('toggle', { bubbles: true }))
        await Promise.resolve()
      })
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(loadSessionMessages).toHaveBeenCalledWith('child-chat', false, true)
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not refresh the sub-agent transcript.')
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(host.querySelector('[role="alert"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
      vi.useRealTimers()
    }
  })

  it('pauses active transcript refresh while hidden and resumes when visible', async () => {
    vi.useFakeTimers()
    const originalVisibility = document.visibilityState
    const openSubAgentSession = vi.fn(async (): Promise<string | null> => 'child-chat')
    const loadSessionMessages = vi.fn(async () => true)
    useAppStore.setState({
      sessions: [{ id: 'child-chat', name: 'Child', viewMode: 'chat', folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() }],
      messages: { 'child-chat': [] }, openSubAgentSession, loadSessionMessages,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'hidden' })
      await act(async () => root.render(<PersistedMessageContent message={{
        id: 'parent-message', sessionId: 'parent-chat', role: 'assistant', content: '', createdAt: new Date(),
        blocks: [{ type: 'tool_call', toolCallId: 'child-tool' }],
        toolCalls: [{ id: 'child-tool', kind: 'sub-agent', title: 'Review', status: 'running', content: '', isSubAgent: true, subAgentId: 'native-child' }],
      }} workingMode="verbose" sourceChatId="parent-chat" onSelectTool={vi.fn()} />))
      await act(async () => {
        const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
        panel.open = true
        panel.dispatchEvent(new Event('toggle', { bubbles: true }))
        await Promise.resolve()
      })
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(loadSessionMessages).not.toHaveBeenCalled()
      Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' })
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(loadSessionMessages).toHaveBeenCalledWith('child-chat', false, true)
    } finally {
      Object.defineProperty(document, 'visibilityState', { configurable: true, value: originalVisibility })
      act(() => root.unmount())
      host.remove()
      vi.useRealTimers()
    }
  })

  it('hydrates a completed child when its session arrives in a later snapshot', async () => {
    const child = { id: 'child-chat', name: 'Child', viewMode: 'chat' as const, folderId: 'folder', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() }
    let resolveFirst!: (value: string) => void
    const openSubAgentSession = vi.fn(async () => await new Promise<string>((resolve) => { resolveFirst = resolve }))
    const loadSessionMessages = vi.fn(async () => useAppStore.setState({ messages: { 'child-chat': [{ id: 'child-message', sessionId: 'child-chat', role: 'assistant', content: 'Child transcript', createdAt: new Date() }] } }))
    useAppStore.setState({ sessions: [], messages: {}, openSubAgentSession, loadSessionMessages })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<PersistedMessageContent message={{
        id: 'parent-message', sessionId: 'parent-chat', role: 'assistant', content: '', createdAt: new Date(),
        blocks: [{ type: 'tool_call', toolCallId: 'child-tool' }],
        toolCalls: [{ id: 'child-tool', kind: 'sub-agent', title: 'Review', status: 'completed', content: '', isSubAgent: true, subAgentId: 'native-child' }],
      }} workingMode="verbose" sourceChatId="parent-chat" onSelectTool={vi.fn()} />))
      await act(async () => {
        const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
        panel.open = true
        panel.dispatchEvent(new Event('toggle', { bubbles: true }))
      })
      await act(async () => { resolveFirst('child-chat') })
      expect(openSubAgentSession).toHaveBeenCalledTimes(1)
      expect(loadSessionMessages).not.toHaveBeenCalled()
      await act(async () => { useAppStore.setState({ sessions: [child] }) })
      expect(loadSessionMessages).toHaveBeenCalledWith('child-chat', false)
      expect(host.textContent).toContain('Child transcript')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it.each(['completed', 'failed'])('waits for subtask history and fetches its final %s snapshot', async (status) => {
    vi.useFakeTimers()
    const openSubAgentSession = vi.fn(async (): Promise<string | null> => 'child-chat')
    const replies: Array<() => void> = []
    const loadSessionMessages = vi.fn(() => new Promise<void>((resolve) => replies.push(resolve)))
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
    const onRender = vi.fn()
    const renderSubtask = async (status: string) => act(async () => {
      root.render(
        <Profiler id="child-history" onRender={onRender}><PersistedMessageContent
          message={{
            id: 'message-subtask',
            sessionId: 'parent-chat',
            role: 'assistant',
            content: '',
            createdAt: new Date(),
            blocks: [{ type: 'tool_call', toolCallId: 'subtask-1' }],
            toolCalls: [{ id: 'subtask-1', kind: 'sub-agent', title: 'Review', status, content: '', isSubAgent: true, subAgentId: 'native-child' }],
          }}
          workingMode="verbose"
          sourceChatId="parent-chat"
          onSelectTool={vi.fn()}
        /></Profiler>
      )
      await Promise.resolve()
    })
    await renderSubtask('running')
    const panel = host.querySelector('details.uam-subagent-panel') as HTMLDetailsElement
    await act(async () => {
      panel.open = true
      panel.dispatchEvent(new Event('toggle', { bubbles: true }))
      await Promise.resolve()
    })
    expect(openSubAgentSession).toHaveBeenCalledTimes(1)
    onRender.mockClear()
    for (let index = 0; index < 20; index++) {
      act(() => useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) => session.id === 'parent-chat' ? { ...session, updatedAt: new Date(index) } : session),
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'parent-chat': {
          ...state.acpBindingBySessionId['parent-chat'], providerId: 'codex-cli', recentStderr: String(index),
        } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] },
      })))
    }
    expect(onRender).not.toHaveBeenCalled()
    act(() => useAppStore.setState({ messages: { 'child-chat': [{
      id: 'child-answer', sessionId: 'child-chat', role: 'assistant', content: 'Child transcript update', createdAt: new Date(),
    }] } }))
    expect(host.textContent).toContain('Child transcript update')


    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
    expect(loadSessionMessages).not.toHaveBeenCalled()
    await act(async () => { await vi.advanceTimersByTimeAsync(14_000) })
    expect(openSubAgentSession).toHaveBeenCalledTimes(1)
    expect(loadSessionMessages).toHaveBeenCalledWith('child-chat', false, true)
    expect(loadSessionMessages).toHaveBeenCalledTimes(1)
    await act(async () => { replies[0]() })
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(loadSessionMessages).toHaveBeenCalledTimes(2)
    openSubAgentSession.mockResolvedValueOnce(null)
    await renderSubtask(status)
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not refresh')
    expect(host.querySelector('[aria-label="Subtask transcript: Child"]')).not.toBeNull()
    expect(openSubAgentSession).toHaveBeenCalledTimes(2)
    await act(async () => {
      replies[1]()
      await vi.advanceTimersByTimeAsync(3000)
    })
    expect(loadSessionMessages).toHaveBeenCalledTimes(2)
    expect(openSubAgentSession).toHaveBeenCalledTimes(2)

    await act(async () => {
      const action = status === 'completed'
        ? Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Retry')
        : host.querySelector<HTMLButtonElement>('[aria-label="Dismiss transcript error"]')
      expect(action).toBeTruthy()
      action!.click()
    })
    expect(openSubAgentSession).toHaveBeenCalledTimes(status === 'completed' ? 3 : 2)
    expect(host.querySelector('[role="alert"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })
})
