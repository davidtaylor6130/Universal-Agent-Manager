import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { AttachmentList, PersistedMessageContent, ThinkingBlock, TurnTimelineContent, stoppedResponseLabel } from './MessageBlocks'
import { ToolCallModal } from './ToolCallViews'
import { ConversationWork } from './ConversationWork'
import { useAppStore } from '../../store/useAppStore'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import type { CppMessage } from '../../store/cpp/types'
import type { MessageBlock } from '../../types/message'
import { FileInspectionContext } from '../shared/fileInspectionContext'
import { InlineSubagentProvider } from './InlineSubagentGroup'

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
    expect(markdownContext?.textContent).toContain('review.uamskill')
    expect(markdownContext?.textContent).not.toContain('/tmp/review.uam')
    expect(files?.textContent).toContain('diagram.pngimage')
    expect(files?.querySelector('[data-detail="/tmp/diagram.png"]')).not.toBeNull()

    act(() => root.unmount())
    host.remove()
  })
})

it.each(['plain', 'blocks', 'legacy'] as const)('keeps images above text and ordinary files below in %s messages', (kind) => {
  const host = document.createElement('div')
  const root = createRoot(host)
  try {
    act(() => root.render(<PersistedMessageContent message={{ id: 'order', sessionId: 'chat', role: 'assistant', content: 'Message text', createdAt: new Date(0),
      ...(kind === 'blocks' ? { blocks: [{ type: 'assistant_text' as const, text: 'Message text' }] } : {}),
      ...(kind === 'legacy' ? { thoughts: 'Reasoning' } : {}),
      attachments: [{ id: 'image', name: 'photo.webp', type: 'file', mimeType: 'image/webp', size: 4 }, { id: 'doc', name: 'notes.txt', type: 'file', size: 4 }],
    }} onSelectTool={() => undefined} />))
    const order = host.textContent!
    expect(order.indexOf('photo.webp')).toBeLessThan(order.indexOf('Message text'))
    expect(order.indexOf('notes.txt')).toBeGreaterThan(order.indexOf('Message text'))
    expect(host.querySelectorAll('[data-detail="photo.webp"]')).toHaveLength(1)
  } finally { act(() => root.unmount()); host.remove() }
})

describe('working transcript', () => {
  it('keeps two provider sub-agent transcripts independently expanded', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ConversationWork
      events={[]} active={false} duration="1s" expanded
      tools={[
        { id: 'child-a', title: 'Review A', kind: 'sub-agent', status: 'completed', content: '', isSubAgent: true, subAgentId: 'provider-a' },
        { id: 'child-b', title: 'Review B', kind: 'sub-agent', status: 'completed', content: '', isSubAgent: true, subAgentId: 'provider-b' },
      ]}
      onSelectTool={vi.fn()}
      renderSubAgentHistory={(tool) => <article>{tool.subAgentId} tool response</article>}
    />))
    const panels = Array.from(host.querySelectorAll<HTMLDetailsElement>('details.conversation-work__subagent'))
    expect(panels).toHaveLength(2)
    act(() => panels[0].querySelector('summary')!.click())
    act(() => panels[1].querySelector('summary')!.click())
    expect(panels.every((panel) => panel.open)).toBe(true)
    expect(host.textContent).toContain('provider-a tool response')
    expect(host.textContent).toContain('provider-b tool response')
    act(() => panels[0].querySelector('summary')!.click())
    expect(panels[0].open).toBe(false)
    expect(panels[1].open).toBe(true)
    expect(host.textContent).not.toContain('provider-a tool response')
    expect(host.textContent).toContain('provider-b tool response')
    act(() => root.unmount())
    host.remove()
  })

  it('distinguishes a timeout stop from an interrupted response', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const render = (interrupted: boolean) => act(() => root.render(<TurnTimelineContent events={[]} tools={[]}
      interrupted={interrupted} stopReason="timeout" pendingPermission={null} pendingUserInput={null}
      onSelectTool={() => undefined} onResolvePermission={() => Promise.resolve(true)}
      onResolveUserInput={() => Promise.resolve(true)} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />))
    render(false)
    expect(host.textContent).toContain('Stopped after timeout')
    expect(host.textContent).not.toContain('Response interrupted')
    render(true)
    expect(host.textContent).toContain('Response interrupted')
    expect(host.textContent).not.toContain('Stopped after timeout')
    expect(stoppedResponseLabel('forced', true)).toBe('Forced stop')
    expect(stoppedResponseLabel('failed', true)).toBe('Provider failed')
    expect(stoppedResponseLabel('unknown', true)).toBe('Provider stopped')
    act(() => root.unmount())
    host.remove()
  })

  it('shows a pending permission even before the provider emits a matching event', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<TurnTimelineContent events={[]} tools={[]}
      pendingPermission={{ requestId: 'permission-1', toolCallId: 'missing-tool', title: 'Run command',
        kind: 'commandExecution', status: 'pending', content: 'npm test',
        options: [{ id: 'allow', name: 'Allow', kind: 'decision' }] }}
      pendingUserInput={null} onSelectTool={() => undefined}
      onResolvePermission={() => Promise.resolve(true)}
      onCancelTurn={() => undefined} onStopRuntime={() => undefined} />))
    expect(host.textContent).toContain('Run command')
    expect(host.textContent).toContain('Allow')
    act(() => root.unmount())
    host.remove()
  })

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
      onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
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
    expect(host.textContent).toContain('Worked 1m 23s')
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
      pendingUserInput={null} onSelectTool={vi.fn()} onResolvePermission={vi.fn()}
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
        { type: 'assistant_text', text: 'Intermediate answer.' },
        { type: 'tool_call', toolCallId: 'tool-1' },
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
    expect(text.indexOf('Worked 12s')).toBeLessThan(text.indexOf('Final answer.'))
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
      onSelectTool={onSelectTool} onResolvePermission={vi.fn()} onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
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

    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toContain('Worked')
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
    act(() => root.render(<ToolCallModal tool={{ ...tools[0], content: 'ok\n\u001b[31merror\u001b[0m' }} onClose={onClose} />))

    expect(document.body.querySelector('.uam-tool-modal')).toBeTruthy()
    const output = document.body.querySelector('.uam-tool-modal__output')
    expect(output?.textContent).toContain('error')
    expect(output?.textContent).not.toContain(String.fromCharCode(27))
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
      await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview' && button.getAttribute('aria-expanded') !== 'true') button.click() })
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
      await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview' && button.getAttribute('aria-expanded') !== 'true') button.click() })
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
      await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview' && button.getAttribute('aria-expanded') !== 'true') button.click() })
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
      await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview' && button.getAttribute('aria-expanded') !== 'true') button.click() })
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
    await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview' && button.getAttribute('aria-expanded') !== 'true') button.click() })
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


describe('persisted context compaction', () => {
  it.each(['saved', 'retained'] as const)('hides streaming compaction text in %s messages through completion without hiding ordinary output', async (source) => {
    const host = document.createElement('div')
    const root = createRoot(host)
    const partial = 'Private native summary'
    const ordinary = 'Continuing the requested work.'
    const render = async (blocks: MessageBlock[], active = true) => {
      const message = buildMessageFromCpp('chat', {
        role: 'assistant', content: partial, createdAt: '2026-10-15T22:55:48Z', blocks,
      } as CppMessage, 0)
      expect(message.blocks).toEqual(blocks)
      await act(async () => root.render(source === 'saved'
        ? <PersistedMessageContent message={message} onSelectTool={vi.fn()} />
        : <TurnTimelineContent events={message.blocks!} active={active} tools={[]} pendingPermission={null} pendingUserInput={null}
            onSelectTool={vi.fn()} onResolvePermission={async () => true} onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} />))
    }
    try {
      for (const text of [partial, `${partial} continues`, `${partial} complete`]) {
        await render([{ type: 'context_compaction_summary', text, requestId: 'opencode-message:summary' }])
        expect(host.textContent).not.toContain(partial)
        expect(host.querySelector('.context-compaction')).toBeNull()
      }
      const hidden = { type: 'context_compaction_summary' as const, text: `${partial} complete`, requestId: 'opencode-message:summary' }
      await render([hidden, { type: 'assistant_text', text: ordinary, requestId: 'opencode-message:ordinary' }], false)
      expect(host.textContent).not.toContain(partial)
      expect(host.textContent).toContain(ordinary)
      await render([{ ...hidden, type: 'context_compaction', requestId: 'acp-compaction:summary' },
        { type: 'assistant_text', text: ordinary, requestId: 'opencode-message:ordinary' }], false)
      const details = host.querySelector<HTMLDetailsElement>('details.context-compaction')
      expect(details?.open).toBe(false)
      expect(details?.querySelector('summary')?.textContent).toBe('Context compacted')
      expect(details?.textContent).toContain(partial)
      expect(host.querySelector('.conversation-trace__text')?.textContent).toBe(ordinary)
    } finally { await act(async () => root.unmount()); host.remove() }
  })

  it.each(['saved', 'retained'] as const)('collapses a proven interrupted compaction in %s history without claiming success', async (source) => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const partial = 'The provider received only this part of its compaction summary.'
    const block = { type: 'context_compaction_interrupted' as const, text: partial, requestId: 'acp-compaction:interrupted-native', tokensBefore: 5000, tokensAfter: 1000 }
    try {
      if (source === 'saved') {
        const message = buildMessageFromCpp('chat', {
          role: 'assistant', content: partial, createdAt: '2026-10-15T18:08:28Z',
          blocks: [block], interrupted: true,
        } as CppMessage, 0)
        expect(message.blocks?.[0]).toMatchObject(block)
        await act(async () => root.render(<PersistedMessageContent message={message} workingMode="compact" onSelectTool={vi.fn()} />))
      } else {
        await act(async () => root.render(<TurnTimelineContent events={[block]} tools={[]} pendingPermission={null} pendingUserInput={null} onSelectTool={vi.fn()} onResolvePermission={async () => true} onCancelTurn={vi.fn()} onStopRuntime={vi.fn()} workingMode="compact" />))
      }
      const details = host.querySelector<HTMLDetailsElement>('details.context-compaction')
      expect(details).not.toBeNull()
      expect(details!.open).toBe(false)
      expect(details!.querySelector('summary')?.textContent).toBe('Context compaction interrupted')
      expect(details!.textContent).toContain(partial)
      expect(details!.textContent).not.toContain('reduced')
      expect(host.querySelector('.conversation-trace__text')).toBeNull()
    } finally {
      await act(async () => root.unmount())
      host.remove()
    }
  })

  it('hydrates and renders a native compaction marker without summary text', async () => {
    // Native history omits the text field when a compaction has no summary.
    const message = buildMessageFromCpp('chat', {
      role: 'assistant', content: '', createdAt: '2026-10-03T00:00:00Z',
      blocks: [{ type: 'context_compaction', requestId: 'compact-no-summary' }],
    } as CppMessage, 0)
    expect(message.blocks?.[0]).toMatchObject({ type: 'context_compaction', text: '', requestId: 'compact-no-summary' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<PersistedMessageContent message={message} workingMode="compact" onSelectTool={vi.fn()} />))
      expect(host.querySelector('details.context-compaction')?.textContent).toContain('The provider did not include a summary.')
    } finally {
      await act(async () => root.unmount())
      host.remove()
    }
  })

  it('renders a collapsed clickable separator with the provider summary', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<PersistedMessageContent message={{
      id: 'compaction', sessionId: 'chat', role: 'assistant', content: '', createdAt: new Date(),
      blocks: [{ type: 'context_compaction', text: 'Keep the selected workspace.', requestId: 'compact-1', tokensBefore: 27445, tokensAfter: 1192 }],
    }} workingMode="compact" onSelectTool={vi.fn()} />))
    const details = host.querySelector('details.context-compaction') as HTMLDetailsElement
    expect(details).not.toBeNull()
    expect(details.open).toBe(false)
    expect(details.querySelector('summary')?.textContent).toBe('Context compacted · 27,445 → 1,192 tokens (96% reduced)')
    details.open = true
    expect(details.textContent).toContain('Keep the selected workspace.')
    await act(async () => root.unmount())
    host.remove()
  })
})

it('renders adjacent streamed and saved text as one markdown run without empty rows or crossing tool boundaries', () => {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  const events = [
    { type: 'assistant_text' as const, text: '' },
    { type: 'assistant_text' as const, text: '**Complete' },
    { type: 'assistant_text' as const, text: ' sentence**\n\n```ts\nconst value = ' },
    { type: 'assistant_text' as const, text: '1\n```' },
    { type: 'tool_call' as const, toolCallId: 'tool-boundary' },
    { type: 'assistant_text' as const, text: ' \n ' },
    { type: 'thought' as const, text: 'Boundary thought' },
    { type: 'assistant_text' as const, text: 'Final paragraph.' },
  ]
  const tools = [{ id: 'tool-boundary', title: 'Inspect', kind: 'read', status: 'completed', content: 'OK' }]
  const inspect = () => {
    expect(Array.from(host.querySelectorAll('.conversation-trace__text'), node => node.textContent?.replace('Copy code block', ''))).toEqual(['Complete sentencetsconst value = 1', 'Final paragraph.'])
    expect(host.querySelectorAll('strong')).toHaveLength(1)
    expect(host.querySelector('pre code')?.textContent).toBe('const value = 1')
    expect(host.querySelectorAll('.conversation-trace__text p')).toHaveLength(2)
  }
  try {
    act(() => root.render(<TurnTimelineContent events={events} tools={tools} active disclosureState={{ expanded: true }} pendingPermission={null} pendingUserInput={null} onSelectTool={() => {}} onResolvePermission={async () => true} onCancelTurn={() => {}} onStopRuntime={() => {}} />))
    inspect()
    act(() => root.render(<PersistedMessageContent message={{ id: 'saved', sessionId: 'chat', role: 'assistant', content: '**Complete sentence**\n\n```ts\nconst value = 1\n```Final paragraph.', blocks: events, toolCalls: tools, createdAt: new Date(0) }} disclosureState={{ expanded: true }} onSelectTool={() => {}} />))
    inspect()
    // Rendering must not rewrite persisted/provider-owned fragments.
    expect(events[1].text).toBe('**Complete')
    expect(events).toHaveLength(8)
  } finally { act(() => root.unmount()); host.remove() }
})

it('does not rescan a live timeline for a composer-only render and still accepts streamed updates', () => {
  const host = document.createElement('div')
  const root = createRoot(host)
  let reads = 0
  const events = [{ type: 'assistant_text' as const, get text() { reads++; return 'Before stream' } }]
  const props = { events, tools: [], active: true, pendingPermission: null, pendingUserInput: null,
    onSelectTool: vi.fn(), onResolvePermission: vi.fn(), onCancelTurn: vi.fn(), onStopRuntime: vi.fn() }
  try {
    act(() => root.render(<TurnTimelineContent {...props} />))
    expect(reads).toBeGreaterThan(0)
    reads = 0
    act(() => root.render(<TurnTimelineContent {...props} />))
    expect(reads).toBe(0)
    act(() => root.render(<TurnTimelineContent {...props} events={[{ type: 'assistant_text', text: 'After stream' }]} />))
    expect(host.textContent).toContain('After stream')
  } finally { act(() => root.unmount()); host.remove() }
})

it.each([
  { toolStatus: 'completed', childStatus: 'running', shouldPoll: true },
  { toolStatus: 'running', childStatus: 'completed', shouldPoll: false },
  { toolStatus: 'completed', childStatus: 'pendingInit', shouldPoll: true },
])('refreshes the actual $childStatus child independently of its $toolStatus parent tool', async ({ toolStatus, childStatus, shouldPoll }) => {
  vi.useFakeTimers()
  const original = useAppStore.getState()
  const openSubAgentSession = vi.fn().mockResolvedValue('status-child-chat')
  const loadSessionMessages = vi.fn().mockResolvedValue(true)
  const child = { id: 'status-child-chat', name: 'Status child', folderId: null, viewMode: 'chat' as const, providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() }
  useAppStore.setState({ sessions: [child], messages: { [child.id]: [] }, openSubAgentSession, loadSessionMessages })
  const host = document.createElement('div'); const root = createRoot(host)
  const render = (status: string) => act(async () => root.render(<PersistedMessageContent sourceChatId="status-parent" onSelectTool={vi.fn()} message={{
    id: 'status-message', sessionId: 'status-parent', role: 'assistant', content: '', createdAt: new Date(), blocks: [{ type: 'tool_call', toolCallId: 'status-tool' }],
    toolCalls: [{ id: 'status-tool', title: 'Worker', kind: 'sub-agent', status: toolStatus, content: '', isSubAgent: true, subAgentId: 'status-native-child', subAgentChildren: [{ id: 'status-native-child', title: 'Worker', status }] }],
  }} />))
  try {
    await render(childStatus)
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    await act(async () => (host.querySelector('[aria-label="Preview Worker"]') as HTMLButtonElement).click())
    expect(openSubAgentSession).toHaveBeenCalledTimes(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    if (shouldPoll) {
      expect(loadSessionMessages).toHaveBeenCalledExactlyOnceWith(child.id, false, true)
      await render('completed')
      expect(openSubAgentSession).toHaveBeenCalledTimes(2)
    } else expect(loadSessionMessages).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(original, true); vi.useRealTimers() }
})

it.each([undefined, 'legacy_path'] as const)('NM-R2-03 makes a mixed URI attachment unavailable instead of inspecting its path (source=%s)', async source => {
  const host = document.createElement('div')
  const root = createRoot(host)
  const inspect = vi.fn()
  const sourceUri = 'file:///opaque/reference%20image.png'
  const fetch = vi.fn()
  vi.stubGlobal('fetch', fetch)
  try {
    await act(async () => root.render(<FileInspectionContext.Provider value={inspect}>
      <AttachmentList chatId="stored-chat" nativeMessageIndex={11} attachments={[
        { id: 'mixed-reference', name: 'Recorded image', type: 'file', mimeType: 'image/png', size: 0,
          source, sourceUri, path: '/must-not-inspect/image.png', previewKey: 'a'.repeat(64) },
      ]} />
    </FileInspectionContext.Provider>))
    const chip = host.querySelector<HTMLElement>('.uam-context-chip__body')!
    expect(chip).not.toBeNull()
    act(() => chip.click())
    expect(inspect).not.toHaveBeenCalled()
    expect(host.querySelector('button.uam-context-chip__body, a, img')).toBeNull()
    expect(host.textContent).toContain('External reference')
    expect(host.textContent).toContain('Bytes unavailable')
    expect(host.querySelector('[data-detail]')?.getAttribute('data-detail')).toBe(sourceUri)
    expect(fetch).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove(); vi.unstubAllGlobals() }
})


describe('shared stored history content preservation', () => {
  it.each([false, true])('keeps partial-inventory content, recorded requests and missing-tool evidence without authority (passive=%s)', (passive) => {
    const oldExpanded = useAppStore.getState().expandWorkTraces
    useAppStore.setState({ expandWorkTraces: true })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    const onSelectTool = vi.fn()
    try {
      const message = buildMessageFromCpp('fixture', { role: 'assistant', content: 'Retained final answer', thoughts: 'Retained reasoning',
        toolCalls: [{ id: 'orphan', title: 'Retained unreferenced tool', kind: 'read', status: 'completed', content: 'Stored output' }],
        blocks: [{ type: 'permission_request', requestId: 'permission', text: 'Recorded permission text' },
          { type: 'user_input_request', requestId: 'question', text: 'Recorded question text' },
          { type: 'tool_call', toolCallId: 'missing' }], createdAt: '2026-01-01T00:00:00Z' }, 0)
      act(() => root.render(<PersistedMessageContent message={message} onSelectTool={onSelectTool} passive={passive} workingMode="verbose" />))
      expect(host.textContent).toContain('Retained final answer')
      expect(host.textContent).toContain('Retained reasoning')
      expect(host.textContent).toContain('Retained unreferenced tool')
      expect(host.querySelectorAll('[role="note"]')).toHaveLength(2)
      expect(host.textContent).toContain('Recorded permission text')
      expect(host.textContent).toContain('Recorded question text')
      expect(host.textContent).toContain('Tool activity is unavailable in this stored transcript.')
      expect(host.textContent).not.toContain('pending')
      expect([...host.querySelectorAll('button')].some(button => /approve|allow|answer|resume|send/i.test(button.textContent ?? ''))).toBe(false)
      expect(onSelectTool).not.toHaveBeenCalled()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ expandWorkTraces: oldExpanded }) }
  })

  it('keeps separately retained final content when only reasoning is in the block inventory', () => {
    const host = document.createElement('div'); const root = createRoot(host)
    try {
      act(() => root.render(<PersistedMessageContent message={{ id: 'partial', sessionId: 'fixture', role: 'assistant',
        content: 'Final answer survives.', createdAt: new Date(0), blocks: [{ type: 'thought', text: 'Reasoning only.' }] }} onSelectTool={() => undefined} />))
      expect(host.textContent?.split('Final answer survives.')).toHaveLength(2)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('explains unavailable stored deferred output without borrowing any chat read or resume capability', () => {
    const requests: unknown[] = []; const previous = window.cefQuery
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ToolCallModal tool={{ id: 'deferred', title: 'Stored deferred tool', kind: 'read', status: 'completed', content: '', contentDeferred: true }} onClose={() => undefined} />))
      expect(document.body.textContent).toContain('Tool output was not included in this stored transcript.')
      expect(document.body.textContent).not.toContain('No tool output yet.')
      expect(requests).toEqual([])
      expect([...document.body.querySelectorAll('button')].some(button => /load latest|resume|open chat/i.test(button.textContent ?? ''))).toBe(false)
    } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous }
  })
})


it('retains passive collapsed recorded launches without borrowing an enclosing parent feed or native child identity', async () => {
  const previous = useAppStore.getState()
  const oldCef = window.cefQuery
  const requests: Array<{ action: string; payload?: unknown }> = []
  const open = vi.fn().mockResolvedValue(null)
  const select = vi.fn()
  const inspect = vi.fn()
  const fetch = vi.fn()
  vi.stubGlobal('fetch', fetch)
  window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
  useAppStore.setState({ expandWorkTraces: false, openSubAgentSession: open,
    sessions: [{ id: 'borrowed-parent', providerId: 'codex-cli' } as never, { id: 'recorded-native', providerId: 'copilot-cli' } as never] })
  const message = buildMessageFromCpp('stored-child', { role: 'assistant', content: 'Final recorded text', createdAt: '2026-01-01T00:00:00Z',
    blocks: [{ type: 'tool_call', toolCallId: 'native-launch' }, { type: 'assistant_text', text: '   ' }, { type: 'tool_call', toolCallId: 'managed-launch' }],
    toolCalls: [{ id: 'native-launch', title: 'spawn_agent', kind: 'sub-agent', status: 'completed', content: 'Stored native result', isSubAgent: true,
      subAgentChildren: [{ id: 'recorded-native', title: 'Recorded native task', status: 'completed', durationMs: 2000 }] },
      { id: 'managed-launch', title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', content: '{"runId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","status":"queued"}' }],
  }, 1)
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    await act(async () => root.render(<FileInspectionContext.Provider value={inspect}><InlineSubagentProvider sourceChatId="borrowed-parent">
      <PersistedMessageContent passive sourceChatId="borrowed-parent" message={message} launchesOnly workingMode="verbose" onSelectTool={select} />
    </InlineSubagentProvider></FileInspectionContext.Provider>))
    expect(host.querySelectorAll('.uam-inline-subagents')).toHaveLength(2)
    expect(host.querySelector('[data-source-chat]')).toBeNull()
    expect(host.querySelector('.uam-subagent-group')?.getAttribute('open')).toBeNull()
    await act(async () => { const details = host.querySelector<HTMLDetailsElement>('.uam-subagent-group')!; details.open = true; details.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector('[data-subagent-row="recorded-native"] .uam-subagent-card__title')?.textContent).toBe('Recorded native task')
    expect(host.querySelector('[data-subagent-row="recorded-native"] .uam-subagent-card__provider')?.getAttribute('data-provider-id')).toBe('unknown')
    const native = host.querySelector<HTMLButtonElement>('[data-subagent-row="recorded-native"]')!
    expect(native.disabled).toBe(true)
    const managed = host.querySelector<HTMLButtonElement>('[data-managed-open]')!
    expect(managed.disabled).toBe(true)
    await act(async () => { native.click(); managed.click() })
    expect(host.querySelector('[aria-label^="Preview "]')).toBeNull()
    expect(host.querySelector('[aria-label^="Stop "]')).toBeNull()
    expect(requests).toEqual([]); expect(open).not.toHaveBeenCalled(); expect(fetch).not.toHaveBeenCalled(); expect(inspect).not.toHaveBeenCalled()
    expect(select).not.toHaveBeenCalled()
    expect(host.textContent).not.toContain('Final recorded text')
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = oldCef; useAppStore.setState(previous, true); vi.unstubAllGlobals() }
})
