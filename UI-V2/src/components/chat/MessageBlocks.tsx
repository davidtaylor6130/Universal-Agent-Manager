// Message block renderers: thinking blocks, plan blocks, goal review,
// persisted message content, attachment list, and turn timeline.
// Extracted from ChatView.tsx (MO-3).

import { MarkdownContent } from '../markdown/Markdown'
import { useEffect, useState, type ReactNode } from 'react'
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
import { Tooltip } from '../ui'
import { BookOpen, Brain, ChevronRight, LoaderCircle } from 'lucide-react'
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
  const [selectedTool, setSelectedTool] = useState<AcpToolCall | null>(null)
  const openSubAgentSession = useAppStore((state) => state.openSubAgentSession)
  const loadSessionMessages = useAppStore((state) => state.loadSessionMessages)
  const workingDisplayMode = useAppStore((state) => state.workingDisplayMode)
  const sourceSession = useAppStore((state) => state.sessions.find((candidate) => candidate.id === sourceChatId))
  const sourceAcp = useAppStore((state) => state.acpBindingBySessionId[sourceChatId])
  const providers = useAppStore((state) => state.providers)
  const session = useAppStore((state) => state.sessions.find((candidate) => candidate.id === chatId))
  const messages = useAppStore((state) => state.messages[chatId] ?? [])
  const isActive = tool.status === 'running' || tool.status === 'in_progress' || tool.status === 'pending'
  const providerId = sourceSession?.providerId || sourceAcp?.providerId || DEFAULT_PROVIDER_ID
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
  }, [openSubAgentSession, sourceChatId, tool.subAgentId, tool.subAgentTitle])

  useEffect(() => {
    if (!isActive || !chatId) return
    let refreshing = false
    const refreshTimer = window.setInterval(() => {
      if (refreshing) return
      refreshing = true
      void Promise.resolve()
        .then(() => loadSessionMessages(chatId))
        .catch(() => {})
        .finally(() => { refreshing = false })
    }, 1000)
    return () => window.clearInterval(refreshTimer)
  }, [chatId, isActive, loadSessionMessages])

  if (!tool.subAgentId) {
    return (
      <section aria-label="Provider sub-agent event" className="space-y-1 text-xs">
        <div className="font-semibold" style={{ color: 'var(--text-2)' }}>No separate transcript</div>
        <div style={{ color: 'var(--text-3)' }}>{providerName} did not expose a child session ID, so UAM cannot show an internal conversation for this event.</div>
      </section>
    )
  }
  if (error) return <div role="alert" className="text-xs" style={{ color: 'var(--error)' }}>Transcript unavailable: {error}</div>
  if (!chatId || !session) return <div role="status" className="text-xs" style={{ color: 'var(--text-3)' }}>Loading subtask transcript from {providerName}…</div>

  return (
    <section className="space-y-3" aria-label={`Subtask transcript: ${session.name}`}>
      {selectedTool && <ToolCallModal tool={selectedTool} chatId={chatId} onClose={() => setSelectedTool(null)} />}
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
            onSelectTool={(_, toolId) => setSelectedTool(message.toolCalls?.find((candidate) => candidate.id === toolId) ?? null)}
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
}: {
  text: string
  defaultOpen?: boolean
  active?: boolean
}) {
  if (!text.trim()) return null

  return (
    <details
      aria-label="Thinking"
      data-testid="thinking-block"
      data-active={active}
      className="uam-thinking-block uam-thinking-row"
      open={defaultOpen}
    >
      <summary className="uam-thinking-row__summary">
        <Brain className="uam-thinking-row__icon" size={13} aria-hidden />
        <span className="uam-thinking-row__kind">Thinking</span>
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

function formatWorkedDuration(seconds = 0) {
  const wholeSeconds = Math.max(0, Math.round(seconds))
  const minutes = Math.floor(wholeSeconds / 60)
  const remainder = wholeSeconds % 60
  if (minutes === 0) return `${remainder}s`
  if (remainder === 0) return `${minutes}m`
  return `${minutes}m ${remainder}s`
}

function lastNonEmptyLine(text: string) {
  return text.split('\n').reverse().find((line) => line.trim())?.trim() ?? ''
}

function CompactWorkingSummary({
  events,
  tools,
  active,
  workedSeconds,
  onSelectTool,
  renderSubAgentHistory,
}: {
  events: AcpTurnEvent[]
  tools: AcpToolCall[]
  active: boolean
  workedSeconds?: number
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  const [open, setOpen] = useState(active)
  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const lastTextUpdate = events.slice().reverse().find(
    (event) => (event.type === 'thought' || event.type === 'assistant_text') && event.text.trim()
  )
  const lastUpdate = lastTextUpdate && (lastTextUpdate.type === 'thought' || lastTextUpdate.type === 'assistant_text')
    ? lastNonEmptyLine(lastTextUpdate.text) || 'Reasoning completed'
    : tools.length > 0
      ? `${active ? 'Using' : 'Used'} a tool`
      : 'Reasoning completed'

  useEffect(() => setOpen(active), [active])

  return (
    <details
      data-testid="working-summary"
      className="uam-working-summary"
      data-active={active}
      open={open}
    >
      <summary
        className="uam-working-summary__header"
        onClick={(event) => {
          event.preventDefault()
          setOpen(active || !open)
        }}
      >
        {active
          ? <LoaderCircle className="uam-working-summary__spinner" size={14} aria-hidden />
          : <ChevronRight className="uam-working-summary__chevron" size={14} aria-hidden />}
        <span className="uam-working-summary__label">
          {active ? 'Working' : `Worked for ${formatWorkedDuration(workedSeconds)}`}
        </span>
        <span className="uam-working-summary__last">{lastUpdate}</span>
      </summary>
      {open && (
        <div className="uam-working-summary__content">
          {events.map((event, index) => {
            if (event.type === 'assistant_text') {
              return <MarkdownContent key={`compact-text-${index}`} content={event.text} />
            }
            if (event.type === 'thought') {
              return <ThinkingBlock key={`compact-thought-${index}`} text={event.text} />
            }
            if (event.type === 'tool_call') {
              const tool = toolById.get(event.toolCallId)
              if (!tool) return null
              return (
                <ToolCallInlineRows
                  key={`compact-tool-${event.toolCallId}-${index}`}
                  tools={[tool]}
                  onSelectTool={onSelectTool}
                  renderSubAgentHistory={renderSubAgentHistory}
                />
              )
            }
            return null
          })}
        </div>
      )}
    </details>
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
  blocks,
  onSelectTool,
  planActions,
  sourceChatId,
  workingMode = 'verbose',
  workedSeconds,
}: {
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
  const toolById = new Map((message.toolCalls ?? []).map((tool) => [tool.id, tool]))
  const lastPlanBlockIndex = blocks.reduce((latest, block, index) => block.type === 'plan' ? index : latest, -1)
  const lastAssistantTextIndex = blocks.reduce(
    (latest, block, index) => block.type === 'assistant_text' && block.text.trim() ? index : latest,
    -1
  )
  const compactWorkingEvents: AcpTurnEvent[] = workingMode === 'compact'
    ? blocks.flatMap<AcpTurnEvent>((block, index) => {
        if (block.type === 'thought') return [{ type: 'thought' as const, text: block.text }]
        if (block.type === 'tool_call') return [{ type: 'tool_call' as const, toolCallId: block.toolCallId }]
        if (block.type === 'assistant_text' && index !== lastAssistantTextIndex) {
          return [{ type: 'assistant_text' as const, text: block.text }]
        }
        return []
      })
    : []

  return (
    <div className="space-y-2">
      {compactWorkingEvents.length > 0 && (
        <CompactWorkingSummary
          events={compactWorkingEvents}
          tools={message.toolCalls ?? []}
          active={false}
          workedSeconds={workedSeconds}
          onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
          renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
        />
      )}
      {blocks.map((block, index) => {
        if (block.type === 'assistant_text') {
          if (workingMode === 'compact' && index !== lastAssistantTextIndex) return null
          const review = parseGoalReviewDecision(block.text)
          if (review) return <GoalReviewBlock key={`block-goal-review-${index}`} review={review} />
          return <MarkdownContent key={`block-text-${index}`} content={block.text} />
        }

        if (block.type === 'thought') {
          if (workingMode === 'compact') return null
          return <ThinkingBlock key={`block-thought-${index}`} text={block.text} />
        }

        if (block.type === 'tool_call') {
          if (workingMode === 'compact') return null
          const tool = toolById.get(block.toolCallId) ?? {
            id: block.toolCallId,
            title: block.toolCallId,
            kind: 'tool',
            status: 'pending',
            content: '',
          }
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
  )
}

export function PersistedMessageContent({
  message,
  onSelectTool,
  planActions,
  sourceChatId,
  workingMode = 'verbose',
}: {
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
      {workingMode === 'compact' && (thoughts || toolCalls.length > 0) ? (
        <CompactWorkingSummary
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
      {message.content.trim() && (goalReview ? <GoalReviewBlock review={goalReview} /> : <MarkdownContent content={message.content} />)}
      {workingMode !== 'compact' && (
        <>
          <ThinkingBlock text={thoughts} />
          <ToolCallInlineRows
            tools={toolCalls}
            onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
            renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
          />
        </>
      )}
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
  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const hasPlanEvent = events.some((event) => event.type === 'plan')
  const lastPlanEventIndex = events.reduce((latest, event, index) => event.type === 'plan' ? index : latest, -1)
  const hasPendingPermissionEvent = Boolean(
    pendingPermission &&
      events.some((event) => event.type === 'permission_request' && event.requestId === pendingPermission.requestId)
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
  const compactWorkingEvents = workingMode === 'compact' && !active
    ? events.filter((event, index) => {
        if (event.type === 'thought' || event.type === 'tool_call') return true
        if (event.type !== 'assistant_text') return false
        return events.slice(index + 1).some((candidate) => candidate.type === 'assistant_text' && candidate.text.trim())
      })
    : []

  return (
    <div className="space-y-2">
      {compactWorkingEvents.length > 0 && (
        <CompactWorkingSummary
          events={compactWorkingEvents}
          tools={tools}
          active={active}
          workedSeconds={workedSeconds}
          onSelectTool={onSelectTool}
          renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
        />
      )}
      {events.map((event, index) => {
        if (event.type === 'assistant_text') {
          if (
            workingMode === 'compact' &&
            !active &&
            events.slice(index + 1).some((candidate) => candidate.type === 'assistant_text' && candidate.text.trim())
          ) return null
          return <MarkdownContent key={`text-${index}`} content={event.text} />
        }

        if (event.type === 'thought') {
          if (workingMode === 'compact' && !active) return null
          return <ThinkingBlock key={`thought-${index}`} text={event.text} active={active && index === events.length - 1} />
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

          if (workingMode === 'compact' && !active) {
            if (!shouldRenderPendingPermission && !shouldRenderPendingUserInput) return null
            return (
              <div key={`tool-attention-${event.toolCallId}-${index}`} className="space-y-2">
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

          return (
            <div key={`tool-${event.toolCallId}-${index}`} className="space-y-2">
              <ToolCallInlineRows
                tools={[tool]}
                onSelectTool={onSelectTool}
                renderSubAgentHistory={sourceChatId ? (subAgentTool) => <SubAgentHistory sourceChatId={sourceChatId} tool={subAgentTool} /> : undefined}
              />
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
    </div>
  )
}
