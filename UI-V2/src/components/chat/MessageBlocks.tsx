// Message block renderers: thinking blocks, plan blocks, goal review,
// persisted message content, attachment list, and turn timeline.
// Extracted from ChatView.tsx (MO-3).

import { MarkdownContent } from '../markdown/Markdown'
import { useEffect, useRef, useState, type ReactNode } from 'react'
import { useAppStore } from '../../store/useAppStore'
import type {
  AcpPendingPermission,
  AcpPendingUserInput,
  AcpPlanEntry,
  AcpToolCall,
  AcpTurnEvent,
  AcpUserInputAnswers,
} from '../../store/useAppStore'
import type { Attachment, Message, MessageBlock } from '../../types/message'
import { Button, Notice, Tooltip } from '../ui'
import { BookOpen, Brain, ChevronRight } from 'lucide-react'
import { ConversationWork, ConversationWorkRow, useWorkTraceDisclosure, useThoughtDisclosure, type WorkTraceDisclosureState } from './ConversationWork'
import {
  PermissionInlineCard,
  ToolCallModal,
  ToolCallInlineRows,
  UserInputInlineCard,
} from './ToolCallViews'
import { DEFAULT_PROVIDER_ID, fallbackProviderForId, providerShortName } from '../../utils/providerMetadata'

export function attachmentLabel(attachment: Attachment) {
  const path = attachment.path?.trim()
  return path || attachment.name
}

function SubAgentHistory({ sourceChatId, tool }: { sourceChatId: string; tool: AcpToolCall }) {
  const [chatId, setChatId] = useState('')
  const [error, setError] = useState('')
  const [refreshAttempt, setRefreshAttempt] = useState(0)
  const [selectedToolRef, setSelectedToolRef] = useState<{ messageId: string; toolId: string } | null>(null)
  const openSubAgentSession = useAppStore((state) => state.openSubAgentSession)
  const loadSessionMessages = useAppStore((state) => state.loadSessionMessages)
  const workingDisplayMode = useAppStore((state) => state.workingDisplayMode)
  const providerId = useAppStore((state) =>
    state.sessions.find((candidate) => candidate.id === sourceChatId)?.providerId ||
    state.acpBindingBySessionId[sourceChatId]?.providerId || DEFAULT_PROVIDER_ID
  )
  const providers = useAppStore((state) => state.providers)
  const session = useAppStore((state) => state.sessions.find((candidate) => candidate.id === chatId))
  const messages = useAppStore((state) => state.messages[chatId]) ?? []
  const selectedTool = messages.find((message) => message.id === selectedToolRef?.messageId)
    ?.toolCalls?.find((candidate) => candidate.id === selectedToolRef?.toolId)
  const isActive = tool.status === 'running' || tool.status === 'in_progress' || tool.status === 'pending'
  const finalRefreshPending = useRef(isActive)
  const providerName = providerShortName(
    providers.find((candidate) => candidate.id === providerId) ?? fallbackProviderForId(providerId),
    providerId
  )

  useEffect(() => {
    if (!tool.subAgentId) {
      setChatId('')
      setError('')
      return
    }
    setChatId('')
    setError('')
    let mounted = true
    void (async () => {
      try {
        const openedChatId = await openSubAgentSession(sourceChatId, tool.subAgentId!, tool.subAgentTitle, false)
        if (!mounted) return
        if (openedChatId) setChatId(openedChatId)
        else setError('Sub-agent chat history is unavailable.')
      } catch {
        if (mounted) setError('Sub-agent chat history is unavailable.')
      }
    })()
    return () => {
      mounted = false
    }
  }, [openSubAgentSession, sourceChatId, tool.subAgentId, tool.subAgentTitle, refreshAttempt])

  useEffect(() => {
    if (isActive) finalRefreshPending.current = true
    else if (chatId && tool.subAgentId && finalRefreshPending.current) {
      finalRefreshPending.current = false
      // Re-import the provider snapshot before loading the final child messages.
      let canceled = false
      void openSubAgentSession(sourceChatId, tool.subAgentId, tool.subAgentTitle, false)
        .then((openedChatId) => {
          if (canceled) return
          setError(openedChatId ? '' : 'Could not refresh the sub-agent transcript.')
          if (openedChatId) setChatId(openedChatId)
        })
        .catch(() => {
          if (!canceled) setError('Could not refresh the sub-agent transcript.')
        })
      return () => { canceled = true }
    }
  }, [chatId, isActive, openSubAgentSession, sourceChatId, tool.subAgentId, tool.subAgentTitle])

  useEffect(() => {
    if (chatId || !error || !tool.subAgentId) return
    // A child rollout may not exist yet. Retry only after the previous lookup settles.
    if (!isActive) {
      if (finalRefreshPending.current) {
        finalRefreshPending.current = false
        setRefreshAttempt((attempt) => attempt + 1)
      }
      return
    }
    const retryTimer = window.setTimeout(() => setRefreshAttempt((attempt) => attempt + 1), 5000)
    return () => window.clearTimeout(retryTimer)
  }, [chatId, error, isActive, tool.subAgentId])

  const sessionId = session?.id ?? ''
  const childSessionWasMissing = useRef(false)
  const hydratedSessionId = useRef('')
  useEffect(() => {
    if (!chatId || isActive) return
    if (!sessionId) {
      childSessionWasMissing.current = true
      return
    }
    if (!childSessionWasMissing.current || hydratedSessionId.current === sessionId) return
    childSessionWasMissing.current = false
    hydratedSessionId.current = sessionId
    let canceled = false
    void Promise.resolve(loadSessionMessages(sessionId, false))
      .then((result) => {
        if (!canceled && result === false) setError('Could not load the sub-agent transcript.')
      })
      .catch(() => {
        if (!canceled) setError('Could not load the sub-agent transcript.')
      })
    return () => { canceled = true }
  }, [isActive, loadSessionMessages, sessionId, chatId])

  useEffect(() => {
    if (!isActive || !chatId) return
    let refreshing = false
    let canceled = false
    // Native refresh exports a full transcript and may start remote provider processes.
    const refreshTimer = window.setInterval(() => {
      if (document.visibilityState === 'hidden') return
      if (refreshing) return
      refreshing = true
      void Promise.resolve()
        .then(() => loadSessionMessages(chatId, false, true))
        .then((result) => {
          if (!canceled) setError(result === false ? 'Could not refresh the sub-agent transcript.' : '')
        })
        .catch(() => {
          if (!canceled) setError('Could not refresh the sub-agent transcript.')
        })
        .finally(() => { if (!canceled) refreshing = false })
    }, 5000)
    return () => {
      canceled = true
      window.clearInterval(refreshTimer)
    }
  }, [chatId, isActive, loadSessionMessages])

  if (!tool.subAgentId) {
    return (
      <section aria-label="Provider sub-agent event" className="space-y-1 text-xs">
        <div className="font-semibold" style={{ color: 'var(--text-2)' }}>No separate transcript</div>
        <div style={{ color: 'var(--text-3)' }}>{providerName} did not expose a child session ID, so UAM cannot show an internal conversation for this event.</div>
      </section>
    )
  }
  const errorNotice = error ? (
    <Notice key={`${refreshAttempt}:${error}`} tone="error" title="Transcript unavailable" dismissLabel="Dismiss transcript error"
      actions={<Button size="sm" onClick={() => setRefreshAttempt((attempt) => attempt + 1)}>Retry</Button>}>
      {error}
    </Notice>
  ) : null
  if (!chatId || !session) return errorNotice ?? <div role="status" className="text-xs" style={{ color: 'var(--text-3)' }}>Loading subtask transcript from {providerName}…</div>

  return (
    <section className="space-y-3" aria-label={`Subtask transcript: ${session.name}`}>
      {errorNotice}
      {selectedTool && <ToolCallModal tool={selectedTool} chatId={chatId} messageIndex={messages.findIndex((message) => message.id === selectedToolRef?.messageId)} onClose={() => setSelectedToolRef(null)} />}
      <div>
        <div className="text-xs font-semibold" style={{ color: 'var(--blue)' }}>{session.name}</div>
        <div className="text-[10px]" style={{ color: 'var(--text-3)' }}>{providerName} · Transcript available</div>
      </div>
      {messages.length === 0 && <div className="text-xs" style={{ color: 'var(--text-3)' }}>No messages recorded.</div>}
      {messages.map((message) => (
        <article key={message.id} className="space-y-2" style={{ borderLeft: '2px solid var(--border-bright)', paddingLeft: 10 }}>
          <div className="text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>{message.role}</div>
          <PersistedMessageContent
            message={message}
            workingMode={workingDisplayMode}
            sourceChatId={chatId}
            onSelectTool={(_, toolId) => setSelectedToolRef({ messageId: message.id, toolId })}
          />
        </article>
      ))}
    </section>
  )
}

export function ThinkingBlock({
  text,
  defaultOpen = false,
  active = false,
  disclosureState,
  thoughtIndex,
}: {
  text: string
  defaultOpen?: boolean
  disclosureState?: WorkTraceDisclosureState
  thoughtIndex?: number
  active?: boolean
}) {
  const { open, toggle } = useThoughtDisclosure(disclosureState, thoughtIndex, defaultOpen)
  if (!text.trim()) return null

  return (
    <details
      aria-label="Thinking"
      data-testid="thinking-block"
      data-active={active}
      className="uam-thinking-block uam-thinking-row"
      open={open}
    >
      <summary className="uam-thinking-row__summary" onClick={(event) => { event.preventDefault(); toggle() }}>
        <Brain className="uam-thinking-row__icon" size={13} aria-hidden />
        <span className="uam-thinking-row__kind">Thoughts</span>
        <span className="uam-thinking-row__preview">{text.split('\n').find((line) => line.trim())}</span>
        <ChevronRight className="uam-thinking-row__chevron" size={13} aria-hidden />
      </summary>
      <div className="uam-thinking-row__content">
        <MarkdownContent content={text} />
      </div>
    </details>
  )
}

export type WorkingDisplayMode = 'compact' | 'verbose'

export function formatWorkedDuration(seconds = 0) {
  const wholeSeconds = Math.max(0, Math.round(seconds))
  const minutes = Math.floor(wholeSeconds / 60)
  const remainder = wholeSeconds % 60
  if (minutes === 0) return `${remainder}s`
  if (remainder === 0) return `${minutes}m`
  return `${minutes}m ${remainder}s`
}

function CompactWorkingSummary({
  events,
  tools,
  active,
  workedSeconds,
  onSelectTool,
  renderSubAgentHistory,
  headerOnly = false,
  startedAt,
  expanded,
  disclosureState,
  onToggle,
}: {
  expanded?: boolean
  disclosureState?: WorkTraceDisclosureState
  onToggle?: () => void
  startedAt?: number
  headerOnly?: boolean
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  active: boolean
  workedSeconds?: number
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  return (
    <ConversationWork
      disclosureState={disclosureState}
      startedAt={startedAt}
      headerOnly={headerOnly}
      expanded={expanded}
      onToggle={onToggle}
      events={events}
      tools={tools}
      active={active}
      duration={formatWorkedDuration(workedSeconds)}
      onSelectTool={onSelectTool}
      renderSubAgentHistory={renderSubAgentHistory}

    />
  )
}

export function planStatusLabel(status: string) {
  if (status === 'inProgress') return 'in progress'
  if (status === 'completed') return 'completed'
  if (status === 'pending') return 'pending'
  return status || 'pending'
}

export function planStatusColor(status: string) {
  if (status === 'completed') return 'var(--green)'
  if (status === 'inProgress') return 'var(--blue)'
  return 'var(--text-3)'
}

export function PlanBlock({
  summary,
  entries,
  showActions = false,
  actionsDisabled = false,
  disabledTitle = 'Codex is still working.',
  onApprove,
  onDeny,
}: {
  summary?: string
  entries?: AcpPlanEntry[]
  showActions?: boolean
  actionsDisabled?: boolean
  disabledTitle?: string
  onApprove?: () => void
  onDeny?: () => void
}) {
  const planSummary = summary?.trim() ?? ''
  const planEntries = entries?.filter((entry) => {
    const content = entry.content.trim()
    return content && content !== planSummary
  }) ?? []
  if (!showActions || (!planSummary && planEntries.length === 0)) return null

  return (
    <section
      data-testid="plan-block"
      className="space-y-3"
      style={{
        border: '1px solid color-mix(in srgb, var(--blue) 42%, var(--border))',
        borderLeft: '4px solid var(--blue)',
        borderRadius: 6,
        background: 'color-mix(in srgb, var(--blue) 9%, var(--surface))',
        color: 'var(--text)',
        padding: 10,
      }}
    >
      <div className="flex items-center gap-2 text-[11px] font-semibold" style={{ color: 'var(--text)' }}>
        <span style={{ color: 'var(--blue)', fontSize: 9 }}>●</span>
        <span>Plan</span>
      </div>
      {planSummary && <MarkdownContent content={planSummary} />}
      {planEntries.length > 0 && (
        <ol className="space-y-2">
          {planEntries.map((entry, index) => (
            <li key={`${entry.content}-${index}`} className="flex gap-2 text-xs" style={{ color: 'var(--text-2)' }}>
              <span style={{ color: planStatusColor(entry.status), fontSize: 9, lineHeight: '20px' }}>●</span>
              <div className="min-w-0 flex-1">
                <div style={{ color: 'var(--text)' }}>{entry.content}</div>
                <div className="text-[10px] uppercase" style={{ color: planStatusColor(entry.status) }}>
                  {planStatusLabel(entry.status)}
                </div>
              </div>
            </li>
          ))}
        </ol>
      )}
      {showActions && (
        <div className="flex flex-wrap gap-2 pt-1">
          <Tooltip label={actionsDisabled ? disabledTitle : 'Approve plan'}>
            <button
              type="button"
              className="px-3 h-7 text-[11px] font-medium"
              disabled={actionsDisabled}
              // Radix tooltips don't fire on disabled buttons; native title carries the reason.
              title={actionsDisabled ? disabledTitle : undefined}
              style={{
                borderRadius: 6,
                border: '1px solid color-mix(in srgb, var(--green) 52%, var(--border-bright))',
                background: actionsDisabled ? 'var(--surface-up)' : 'color-mix(in srgb, var(--green) 16%, var(--surface-up))',
                color: actionsDisabled ? 'var(--text-3)' : 'var(--text)',
                opacity: actionsDisabled ? 0.65 : 1,
              }}
              onClick={() => {
                if (!actionsDisabled) onApprove?.()
              }}
            >
              Approve
            </button>
          </Tooltip>
          <Tooltip label={actionsDisabled ? disabledTitle : 'Deny plan'}>
            <button
              type="button"
              className="px-3 h-7 text-[11px] font-medium"
              disabled={actionsDisabled}
              title={actionsDisabled ? disabledTitle : undefined}
              style={{
                borderRadius: 6,
                border: '1px solid color-mix(in srgb, var(--red) 48%, var(--border-bright))',
                background: actionsDisabled ? 'var(--surface-up)' : 'color-mix(in srgb, var(--red) 12%, var(--surface-up))',
                color: actionsDisabled ? 'var(--text-3)' : 'var(--text)',
                opacity: actionsDisabled ? 0.65 : 1,
              }}
              onClick={() => {
                if (!actionsDisabled) onDeny?.()
              }}
            >
              Deny
            </button>
          </Tooltip>
        </div>
      )}
    </section>
  )
}

type GoalReviewDecision = {
  decision: 'complete' | 'continue' | 'blocked'
  reason: string
  nextPrompt: string
  evidence: string[]
  currentStep: string
  lastVerification: string
}

export function parseGoalReviewDecision(text: string): GoalReviewDecision | null {
  const trimmed = text.trim()
  if (!trimmed.includes('"decision"')) return null

  const first = trimmed.indexOf('{')
  const last = trimmed.lastIndexOf('}')
  if (first < 0 || last <= first) return null

  try {
    const parsed = JSON.parse(trimmed.slice(first, last + 1)) as Partial<GoalReviewDecision> & {
      progressUpdate?: { currentStep?: unknown; lastVerification?: unknown }
    }
    if (parsed.decision !== 'complete' && parsed.decision !== 'continue' && parsed.decision !== 'blocked') {
      return null
    }
    return {
      decision: parsed.decision,
      reason: typeof parsed.reason === 'string' ? parsed.reason : '',
      nextPrompt: typeof parsed.nextPrompt === 'string' ? parsed.nextPrompt : '',
      evidence: Array.isArray(parsed.evidence) ? parsed.evidence.filter((item): item is string => typeof item === 'string' && Boolean(item.trim())) : [],
      currentStep: typeof parsed.progressUpdate?.currentStep === 'string' ? parsed.progressUpdate.currentStep : '',
      lastVerification: typeof parsed.progressUpdate?.lastVerification === 'string' ? parsed.progressUpdate.lastVerification : '',
    }
  } catch {
    return null
  }
}

export function goalReviewForMessage(message: Message): GoalReviewDecision | null {
  if (message.role !== 'assistant') return null

  const contentReview = parseGoalReviewDecision(message.content)
  if (contentReview) return contentReview

  for (const block of message.blocks ?? []) {
    if (block.type !== 'assistant_text') continue
    const blockReview = parseGoalReviewDecision(block.text)
    if (blockReview) return blockReview
  }

  return null
}

export function goalReviewDecisionStyle(decision: GoalReviewDecision['decision']) {
  if (decision === 'complete') return { color: 'var(--green)', label: 'Complete' }
  if (decision === 'blocked') return { color: 'var(--red)', label: 'Blocked' }
  return { color: 'var(--purple)', label: 'Continue' }
}

export function GoalReviewBlock({ review }: { review: GoalReviewDecision }) {
  const decision = goalReviewDecisionStyle(review.decision)

  return (
    <section
      data-testid="goal-review-block"
      aria-label="Goal review"
      className="space-y-2"
      style={{
        border: '1px solid color-mix(in srgb, var(--purple) 44%, var(--border))',
        borderLeft: '4px solid var(--purple)',
        borderRadius: 6,
        background: 'color-mix(in srgb, var(--purple) 10%, var(--surface))',
        color: 'var(--text)',
        padding: 10,
      }}
    >
      <div className="flex items-center gap-2 text-[11px] font-semibold">
        <span style={{ color: 'var(--purple)', fontSize: 9 }}>●</span>
        <span>Goal Review</span>
        <span
          className="ml-auto rounded px-1.5 py-0.5 text-[10px] uppercase"
          style={{
            border: `1px solid color-mix(in srgb, ${decision.color} 48%, var(--border))`,
            color: decision.color,
            background: 'color-mix(in srgb, var(--surface-up) 70%, transparent)',
          }}
        >
          {decision.label}
        </span>
      </div>
      {review.reason && (
        <div className="text-xs" style={{ color: 'var(--text-2)' }}>
          {review.reason}
        </div>
      )}
      {(review.currentStep || review.lastVerification || review.evidence.length > 0) && (
        <div className="space-y-1 text-xs" style={{ borderTop: '1px solid color-mix(in srgb, var(--purple) 24%, var(--border))', paddingTop: 8, color: 'var(--text-2)' }}>
          {review.currentStep && <div><span className="font-semibold">Current: </span>{review.currentStep}</div>}
          {review.lastVerification && <div><span className="font-semibold">Verified: </span>{review.lastVerification}</div>}
          {review.evidence.map((item) => <div key={item}><span className="font-semibold">Evidence: </span>{item}</div>)}
        </div>
      )}
      {review.nextPrompt && review.decision === 'continue' && (
        <div
          className="text-xs"
          style={{
            borderTop: '1px solid color-mix(in srgb, var(--purple) 24%, var(--border))',
            paddingTop: 8,
            color: 'var(--text-3)',
          }}
        >
          <span className="font-semibold" style={{ color: 'var(--text-2)' }}>Next: </span>
          {review.nextPrompt}
        </div>
      )}
    </section>
  )
}

export function PersistedMessageBlocksContent({
  message,
  disclosureState,
  blocks,
  onSelectTool,
  planActions,
  sourceChatId,
  workingMode = 'verbose',
  workedSeconds,
}: {
  disclosureState?: WorkTraceDisclosureState
  message: Message
  blocks: MessageBlock[]
  onSelectTool: (messageId: string, toolId: string) => void
  sourceChatId?: string
  workingMode?: WorkingDisplayMode
  workedSeconds?: number
  planActions?: {
    show: boolean
    disabled: boolean
    disabledTitle: string
    onApprove: () => void
    onDeny: () => void
  }
}) {
  const { expanded, toggle, state: traceDisclosure } = useWorkTraceDisclosure(disclosureState)
  const toolById = new Map((message.toolCalls ?? []).map((tool) => [tool.id, tool]))
  let thoughtIndex = 0
  const lastAssistantBlockIndex = blocks.reduce((latest, block, index) => block.type === 'assistant_text' && block.text.trim() ? index : latest, -1)
  const lastPlanBlockIndex = blocks.reduce((latest, block, index) => block.type === 'plan' ? index : latest, -1)

  return (
    <div className="space-y-2">
      {(
        <CompactWorkingSummary
          headerOnly
          expanded={expanded}
          disclosureState={traceDisclosure}
          onToggle={toggle}
          events={[]}
          tools={[]}
          active={false}
          workedSeconds={workedSeconds}
          onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
          renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
        />
      )}
      <div className={workingMode === 'compact' && expanded ? 'conversation-events conversation-trace' : 'conversation-events'}>
      {blocks.map((block, index) => {
        if (block.type === 'assistant_text') {
          if (!expanded && index !== lastAssistantBlockIndex) return null
          const review = parseGoalReviewDecision(block.text)
          if (review) return <div className="conversation-trace__text" key={`block-goal-review-${index}`}><GoalReviewBlock review={review} /></div>
          return <div className="conversation-trace__text" key={`block-text-${index}`}><MarkdownContent content={block.text} /></div>
        }

        if (block.type === 'thought') {
          if (!block.text.trim()) return null
          const ordinal = thoughtIndex++
          if (!expanded) return null
          return workingMode === 'compact'
            ? <div className="conversation-trace__internal" key={`block-thought-${index}`}><ConversationWorkRow text={block.text} disclosureState={traceDisclosure} thoughtIndex={ordinal} onSelectTool={(toolId) => onSelectTool(message.id, toolId)} /></div>
            : <ThinkingBlock key={`block-thought-${index}`} text={block.text} disclosureState={traceDisclosure} thoughtIndex={ordinal} />
        }

        if (block.type === 'tool_call') {
          if (!expanded) return null
          const tool = toolById.get(block.toolCallId) ?? {
            id: block.toolCallId,
            title: block.toolCallId,
            kind: 'tool',
            status: 'pending',
            content: '',
          }
          if (workingMode === 'compact') return <div className="conversation-trace__internal" key={`block-tool-${block.toolCallId}-${index}`}><ConversationWorkRow
            tool={tool}
            onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
            renderSubAgentHistory={sourceChatId ? (subAgentTool) => <SubAgentHistory sourceChatId={sourceChatId} tool={subAgentTool} /> : undefined} /></div>
          return (
            <ToolCallInlineRows
              key={`block-tool-${block.toolCallId}-${index}`}
              tools={[tool]}
              onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
              renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
            />
          )
        }

        if (block.type === 'plan') {
          return (
            <PlanBlock
              key={`block-plan-${index}`}
              summary={message.planSummary ?? ''}
              entries={message.planEntries ?? []}
              showActions={index === lastPlanBlockIndex && planActions?.show}
              actionsDisabled={planActions?.disabled}
              disabledTitle={planActions?.disabledTitle}
              onApprove={planActions?.onApprove}
              onDeny={planActions?.onDeny}
            />
          )
        }

        return null
      })}
      </div>
    </div>
  )
}

export function PersistedMessageContent({
  message,
  disclosureState,
  onSelectTool,
  planActions,
  sourceChatId,
  workingMode = 'verbose',
}: {
  disclosureState?: WorkTraceDisclosureState
  message: Message
  onSelectTool: (messageId: string, toolId: string) => void
  sourceChatId?: string
  workingMode?: WorkingDisplayMode
  planActions?: {
    show: boolean
    disabled: boolean
    disabledTitle: string
    onApprove: () => void
    onDeny: () => void
  }
}) {
  const { expanded, toggle, state: traceDisclosure } = useWorkTraceDisclosure(disclosureState)
  const thoughts = message.role === 'assistant' ? message.thoughts?.trim() ?? '' : ''
  const toolCalls = message.role === 'assistant' ? message.toolCalls ?? [] : []
  const planSummary = message.role === 'assistant' ? message.planSummary ?? '' : ''
  const planEntries = message.role === 'assistant' ? message.planEntries ?? [] : []
  const blocks = message.role === 'assistant' ? message.blocks ?? [] : []
  const attachments = message.attachments ?? []
  const goalReview = goalReviewForMessage(message)
  const workedSeconds = (message.processingTimeMs ?? 0) / 1000

  if (blocks.length > 0) {
    return (
      <div className="space-y-2">
        <PersistedMessageBlocksContent
          message={message}
          disclosureState={traceDisclosure}
          blocks={blocks}
          onSelectTool={onSelectTool}
          planActions={planActions}
          sourceChatId={sourceChatId}
          workingMode={workingMode}
          workedSeconds={workedSeconds}
        />
        <AttachmentList attachments={attachments} />
      </div>
    )
  }

  if (!thoughts && toolCalls.length === 0 && !planSummary.trim() && planEntries.length === 0) {
    return (
      <div className="space-y-2">
        {goalReview ? <GoalReviewBlock review={goalReview} /> : <MarkdownContent content={message.content} />}
        <AttachmentList attachments={attachments} />
      </div>
    )
  }

  return (
    <div className="space-y-2">
      {(thoughts || toolCalls.length > 0) ? (
        <CompactWorkingSummary
          headerOnly={workingMode !== 'compact'}
          expanded={expanded}
          disclosureState={traceDisclosure}
          onToggle={toggle}
          events={[
            ...(thoughts ? [{ type: 'thought' as const, text: thoughts }] : []),
            ...toolCalls.map((tool) => ({ type: 'tool_call' as const, toolCallId: tool.id })),
          ]}
          tools={toolCalls}
          active={false}
          workedSeconds={workedSeconds}
          onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
          renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
        />
      ) : null}
      {workingMode !== 'compact' && expanded && (
        <>
          <ThinkingBlock text={thoughts} disclosureState={traceDisclosure} />
          <ToolCallInlineRows
            tools={toolCalls}
            onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
            renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
          />
        </>
      )}
      {message.content.trim() && (goalReview ? <GoalReviewBlock review={goalReview} /> : <MarkdownContent content={message.content} />)}
      <PlanBlock
        summary={planSummary}
        entries={planEntries}
        showActions={planActions?.show}
        actionsDisabled={planActions?.disabled}
        disabledTitle={planActions?.disabledTitle}
        onApprove={planActions?.onApprove}
        onDeny={planActions?.onDeny}
      />
      <AttachmentList attachments={attachments} />
    </div>
  )
}

export function AttachmentList({ attachments }: { attachments: Attachment[] }) {
  if (attachments.length === 0) return null

  const markdownStoreAttachments = attachments.filter((attachment) => attachment.type === 'markdown-store')
  const fileAttachments = attachments.filter((attachment) => attachment.type !== 'markdown-store')

  return (
    <div className="space-y-2">
      {markdownStoreAttachments.length > 0 && (
        <div className="flex flex-wrap gap-2" aria-label="Skills context">
          {markdownStoreAttachments.map((attachment) => (
            <span
              key={attachment.id}
              className="inline-flex items-center gap-2 max-w-full text-[11px]"
              title={attachmentLabel(attachment)}
              style={{
                border: '1px solid color-mix(in srgb, var(--purple) 45%, var(--border))',
                borderRadius: 7,
                background: 'color-mix(in srgb, var(--purple) 10%, var(--surface-up))',
                color: 'var(--text-2)',
                padding: '5px 8px',
              }}
            >
              <BookOpen size={13} aria-hidden style={{ color: 'var(--purple)' }} />
              <span style={{ color: 'var(--purple)' }}>Skills</span>
              <span className="truncate max-w-[320px]">{attachment.name}</span>
            </span>
          ))}
        </div>
      )}
      {fileAttachments.length > 0 && (
        <div className="flex flex-wrap gap-2" aria-label="File attachments">
          {fileAttachments.map((attachment) => (
            <span
              key={attachment.id}
              className="inline-flex items-center gap-2 max-w-full text-[11px]"
              title={attachmentLabel(attachment)}
              style={{
                border: '1px solid var(--border)',
                borderRadius: 999,
                background: 'var(--surface-up)',
                color: 'var(--text-2)',
                padding: '3px 7px',
              }}
            >
              <span className="truncate max-w-[320px]">{attachment.path || attachment.name}</span>
              <span style={{ color: 'var(--text-3)' }}>{attachment.type}</span>
            </span>
          ))}
        </div>
      )}
    </div>
  )
}

export function TurnTimelineContent({
  disclosureState,
  interrupted = false,
  startedAt,
  events,
  tools,
  planSummary,
  planEntries,
  planActions,
  pendingPermission,
  pendingUserInput,
  waitIsStale,
  waitStaleReason,
  waitSeconds,
  onSelectTool,
  onResolvePermission,
  onResolveUserInput,
  onCancelTurn,
  onStopRuntime,
  sourceChatId,
  active = false,
  workingMode = 'verbose',
  workedSeconds,
}: {
  disclosureState?: WorkTraceDisclosureState
  startedAt?: number
  interrupted?: boolean
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  planSummary?: string
  planEntries?: AcpPlanEntry[]
  planActions?: {
    show: boolean
    disabled: boolean
    disabledTitle: string
    onApprove: () => void
    onDeny: () => void
  }
  pendingPermission: AcpPendingPermission | null
  pendingUserInput: AcpPendingUserInput | null
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  onSelectTool: (toolId: string) => void
  onResolvePermission: (requestId: string, optionId: string) => Promise<boolean>
  onResolveUserInput: (requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
  onCancelTurn: () => void
  onStopRuntime: () => void
  sourceChatId?: string
  active?: boolean
  workingMode?: WorkingDisplayMode
  workedSeconds?: number
}) {
  const { expanded, toggle, state: traceDisclosure } = useWorkTraceDisclosure(disclosureState)
  const traceExpanded = active ? true : expanded
  const lastAssistantEventIndex = events.reduce((latest, event, index) => event.type === 'assistant_text' && event.text.trim() ? index : latest, -1)
  let thoughtIndex = 0
  const activeEventIndex = active && !pendingPermission && !pendingUserInput ? events.reduce((latest, event, index) =>
    (event.type === 'assistant_text' || event.type === 'thought') && !event.text.trim() ? latest : index, -1) : -1
  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const hasPlanEvent = events.some((event) => event.type === 'plan')
  const lastPlanEventIndex = events.reduce((latest, event, index) => event.type === 'plan' ? index : latest, -1)
  const hasPendingPermissionEvent = Boolean(
    pendingPermission &&
      events.some((event) => event.type === 'permission_request' && event.requestId === pendingPermission.requestId)
  )
  const hasPendingPermissionToolEvent = Boolean(
    pendingPermission &&
      events.some((event) => event.type === 'tool_call' && event.toolCallId === pendingPermission.toolCallId)
  )
  const hasPendingUserInputEvent = Boolean(
    pendingUserInput &&
      events.some((event) => event.type === 'user_input_request' && event.requestId === pendingUserInput.requestId)
  )
  const hasPendingUserInputToolEvent = Boolean(
    pendingUserInput &&
      pendingUserInput.itemId &&
      events.some((event) => event.type === 'tool_call' && event.toolCallId === pendingUserInput.itemId)
  )

  return (
    <div className="space-y-2">
      {(
        <CompactWorkingSummary
          startedAt={startedAt}
          headerOnly
          expanded={traceExpanded}
          disclosureState={traceDisclosure}
          onToggle={toggle}
          events={[]}
          tools={[]}
          active={active}
          workedSeconds={workedSeconds}
          onSelectTool={onSelectTool}
          renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
        />
      )}
      <div className={workingMode === 'compact' && traceExpanded ? 'conversation-events conversation-trace' : 'conversation-events'}>
      {events.map((event, index) => {
        if (event.type === 'assistant_text') {
          if (!traceExpanded && index !== lastAssistantEventIndex) return null
          return <div className="conversation-trace__text" key={`text-${index}`}><MarkdownContent content={event.text} /></div>
        }

        if (event.type === 'thought') {
          if (!event.text.trim()) return null
          const ordinal = thoughtIndex++
          if (!traceExpanded) return null
          return workingMode === 'compact'
            ? <div className="conversation-trace__internal" key={`thought-${index}`} data-processing-step={index === activeEventIndex || undefined}><ConversationWorkRow text={event.text} disclosureState={traceDisclosure} thoughtIndex={ordinal} onSelectTool={onSelectTool} /></div>
            : <ThinkingBlock key={`thought-${index}`} text={event.text} active={index === activeEventIndex} disclosureState={traceDisclosure} thoughtIndex={ordinal} />
        }

        if (event.type === 'plan') {
          return (
            <PlanBlock
              key={`plan-${index}`}
              summary={planSummary}
              entries={planEntries}
              showActions={index === lastPlanEventIndex && planActions?.show}
              actionsDisabled={planActions?.disabled}
              disabledTitle={planActions?.disabledTitle}
              onApprove={planActions?.onApprove}
              onDeny={planActions?.onDeny}
            />
          )
        }

        if (event.type === 'tool_call') {
          const tool = toolById.get(event.toolCallId) ?? {
            id: event.toolCallId,
            title: event.toolCallId,
            kind: 'tool',
            status: 'pending',
            content: '',
          }
          const shouldRenderPendingPermission =
            pendingPermission &&
            !hasPendingPermissionEvent &&
            pendingPermission.toolCallId === event.toolCallId
          const shouldRenderPendingUserInput =
            pendingUserInput &&
            !hasPendingUserInputEvent &&
            pendingUserInput.itemId === event.toolCallId


          return (
            <div key={`tool-${event.toolCallId}-${index}`} className="space-y-2">
              {traceExpanded && <div className="conversation-trace__internal" data-processing-step={index === activeEventIndex && ['pending', 'in_progress', 'running'].includes(tool.status) || undefined}>
                {workingMode === 'compact' ? <ConversationWorkRow tool={tool} onSelectTool={onSelectTool}
                  renderSubAgentHistory={sourceChatId ? (subAgentTool) => <SubAgentHistory sourceChatId={sourceChatId} tool={subAgentTool} /> : undefined} /> : <ToolCallInlineRows
                  tools={[tool]}
                  onSelectTool={onSelectTool}
                  renderSubAgentHistory={sourceChatId ? (subAgentTool) => <SubAgentHistory sourceChatId={sourceChatId} tool={subAgentTool} /> : undefined}
                />}
              </div>}
              {shouldRenderPendingPermission && (
                <PermissionInlineCard
                  permission={pendingPermission}
                  onResolve={onResolvePermission}
                  waitIsStale={waitIsStale}
                  waitStaleReason={waitStaleReason}
                  waitSeconds={waitSeconds}
                  onCancelTurn={onCancelTurn}
                  onStopRuntime={onStopRuntime}
                />
              )}
              {shouldRenderPendingUserInput && (
                <UserInputInlineCard
                  input={pendingUserInput}
                  onResolve={onResolveUserInput}
                  waitIsStale={waitIsStale}
                  waitStaleReason={waitStaleReason}
                  waitSeconds={waitSeconds}
                  onCancelTurn={onCancelTurn}
                  onStopRuntime={onStopRuntime}
                />
              )}
            </div>
          )
        }

        if (event.type === 'permission_request' && pendingPermission?.requestId === event.requestId) {
          return (
            <PermissionInlineCard
              key={`permission-${event.requestId}-${index}`}
              permission={pendingPermission}
              onResolve={onResolvePermission}
              waitIsStale={waitIsStale}
              waitStaleReason={waitStaleReason}
              waitSeconds={waitSeconds}
              onCancelTurn={onCancelTurn}
              onStopRuntime={onStopRuntime}
            />
          )
        }

        if (event.type === 'user_input_request' && pendingUserInput?.requestId === event.requestId) {
          return (
            <UserInputInlineCard
              key={`user-input-${event.requestId}-${index}`}
              input={pendingUserInput}
              onResolve={onResolveUserInput}
              waitIsStale={waitIsStale}
              waitStaleReason={waitStaleReason}
              waitSeconds={waitSeconds}
              onCancelTurn={onCancelTurn}
              onStopRuntime={onStopRuntime}
            />
          )
        }

        return null
      })}
      </div>
      {!hasPlanEvent && ((planSummary?.trim() ?? '') || (planEntries?.length ?? 0) > 0) && (
        <PlanBlock
          summary={planSummary}
          entries={planEntries}
          showActions={planActions?.show}
          actionsDisabled={planActions?.disabled}
          disabledTitle={planActions?.disabledTitle}
          onApprove={planActions?.onApprove}
          onDeny={planActions?.onDeny}
        />
      )}
      {pendingPermission && !hasPendingPermissionEvent && !hasPendingPermissionToolEvent && (
        <PermissionInlineCard permission={pendingPermission} onResolve={onResolvePermission}
          waitIsStale={waitIsStale} waitStaleReason={waitStaleReason} waitSeconds={waitSeconds}
          onCancelTurn={onCancelTurn} onStopRuntime={onStopRuntime} />
      )}
      {pendingUserInput && !hasPendingUserInputEvent && !hasPendingUserInputToolEvent && (
        <UserInputInlineCard
          input={pendingUserInput}
          onResolve={onResolveUserInput}
          waitIsStale={waitIsStale}
          waitStaleReason={waitStaleReason}
          waitSeconds={waitSeconds}
          onCancelTurn={onCancelTurn}
          onStopRuntime={onStopRuntime}
        />
      )}
      {interrupted && <div className="conversation-interrupted">Response interrupted</div>}
    </div>
  )
}
