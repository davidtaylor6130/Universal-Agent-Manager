// Message block renderers: thinking blocks, plan blocks, goal review,
// persisted message content, attachment list, and turn timeline.
// Extracted from ChatView.tsx (MO-3).

import { MarkdownContent } from '../markdown/Markdown'
import { useEffect, useState } from 'react'
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
import {
  PermissionInlineCard,
  ToolCallModal,
  ToolCallInlineRows,
  UserInputInlineCard,
} from './ToolCallViews'

export function attachmentLabel(attachment: Attachment) {
  const path = attachment.path?.trim()
  return path || attachment.name
}

function SubAgentHistory({ sourceChatId, tool }: { sourceChatId: string; tool: AcpToolCall }) {
  const [chatId, setChatId] = useState('')
  const [error, setError] = useState('')
  const [selectedTool, setSelectedTool] = useState<AcpToolCall | null>(null)
  const openSubAgentSession = useAppStore((state) => state.openSubAgentSession)
  const session = useAppStore((state) => state.sessions.find((candidate) => candidate.id === chatId))
  const messages = useAppStore((state) => state.messages[chatId] ?? [])
  const isActive = tool.status === 'running' || tool.status === 'in_progress' || tool.status === 'pending'

  useEffect(() => {
    if (!tool.subAgentId) {
      setError('The provider did not expose a sub-agent session ID.')
      return
    }
    const subAgentId = tool.subAgentId
    let mounted = true
    const refresh = () => void openSubAgentSession(sourceChatId, subAgentId, tool.subAgentTitle, false).then((openedChatId) => {
      if (!mounted) return
      if (openedChatId) {
        setChatId(openedChatId)
        setError('')
      }
      else setError('Sub-agent chat history is unavailable.')
    })
    refresh()
    const refreshTimer = isActive ? window.setInterval(refresh, 1000) : undefined
    return () => {
      mounted = false
      if (refreshTimer) window.clearInterval(refreshTimer)
    }
  }, [isActive, openSubAgentSession, sourceChatId, tool.subAgentId, tool.subAgentTitle])

  if (error) return <div className="text-xs" style={{ color: 'var(--danger)' }}>{error}</div>
  if (!chatId || !session) return <div className="text-xs" style={{ color: 'var(--text-3)' }}>Loading sub-agent chat…</div>

  return (
    <section className="space-y-3" aria-label={`Sub-agent chat: ${session.name}`}>
      {selectedTool && <ToolCallModal tool={selectedTool} onClose={() => setSelectedTool(null)} />}
      <div className="text-xs font-semibold" style={{ color: 'var(--blue)' }}>{session.name}</div>
      {messages.length === 0 && <div className="text-xs" style={{ color: 'var(--text-3)' }}>No messages recorded.</div>}
      {messages.map((message) => (
        <article key={message.id} className="space-y-2" style={{ borderLeft: '2px solid var(--border-bright)', paddingLeft: 10 }}>
          <div className="text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>{message.role}</div>
          <PersistedMessageContent
            message={message}
            sourceChatId={chatId}
            onSelectTool={(_, toolId) => setSelectedTool(message.toolCalls?.find((candidate) => candidate.id === toolId) ?? null)}
          />
        </article>
      ))}
    </section>
  )
}

export function ThinkingBlock({ text, defaultOpen = false }: { text: string; defaultOpen?: boolean }) {
  if (!text.trim()) return null

  return (
    <details
      aria-label="Thinking"
      data-testid="thinking-block"
      open={defaultOpen}
      style={{
        border: '1px solid color-mix(in srgb, var(--yellow) 58%, var(--border))',
        borderLeft: '4px solid var(--yellow)',
        borderRadius: 6,
        background: 'color-mix(in srgb, var(--yellow) 12%, var(--surface))',
        color: 'var(--text-2)',
        overflow: 'hidden',
      }}
    >
      <summary
        className="flex items-center gap-2 text-[11px] font-semibold cursor-pointer select-none"
        style={{
          minHeight: 34,
          padding: '0 10px',
          color: 'var(--text)',
          listStyle: 'none',
        }}
      >
        <span aria-hidden="true" style={{ color: 'var(--yellow)', fontSize: 12, fontWeight: 700 }}>{'>'}</span>
        <span style={{ color: 'var(--yellow)', fontSize: 9 }}>●</span>
        <span>Thinking</span>
        <span className="ml-auto text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>
          details
        </span>
      </summary>
      <div
        className="px-3 pb-3 pt-2 text-xs"
        style={{
          borderTop: '1px solid color-mix(in srgb, var(--yellow) 35%, var(--border))',
          color: 'var(--text-2)',
        }}
      >
        <MarkdownContent content={text} />
      </div>
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
  if (!planSummary && planEntries.length === 0) return null

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
}: {
  message: Message
  blocks: MessageBlock[]
  onSelectTool: (messageId: string, toolId: string) => void
  sourceChatId?: string
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

  return (
    <div className="space-y-2">
      {blocks.map((block, index) => {
        if (block.type === 'assistant_text') {
          const review = parseGoalReviewDecision(block.text)
          if (review) return <GoalReviewBlock key={`block-goal-review-${index}`} review={review} />
          return <MarkdownContent key={`block-text-${index}`} content={block.text} />
        }

        if (block.type === 'thought') {
          return <ThinkingBlock key={`block-thought-${index}`} text={block.text} />
        }

        if (block.type === 'tool_call') {
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
}: {
  message: Message
  onSelectTool: (messageId: string, toolId: string) => void
  sourceChatId?: string
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
  const goalReview = message.role === 'assistant' ? parseGoalReviewDecision(message.content) : null

  if (blocks.length > 0) {
    return (
      <div className="space-y-2">
        <PersistedMessageBlocksContent
          message={message}
          blocks={blocks}
          onSelectTool={onSelectTool}
          planActions={planActions}
          sourceChatId={sourceChatId}
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
      {message.content.trim() && (goalReview ? <GoalReviewBlock review={goalReview} /> : <MarkdownContent content={message.content} />)}
      <ThinkingBlock text={thoughts} />
      <ToolCallInlineRows
        tools={toolCalls}
        onSelectTool={(toolId) => onSelectTool(message.id, toolId)}
        renderSubAgentHistory={sourceChatId ? (tool) => <SubAgentHistory sourceChatId={sourceChatId} tool={tool} /> : undefined}
      />
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

  return (
    <div className="flex flex-wrap gap-2">
      {attachments.map((attachment) => (
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
  onResolvePermission: (requestId: string, optionId: string) => void
  onResolveUserInput: (requestId: string, answers: AcpUserInputAnswers) => void
  onCancelTurn: () => void
  onStopRuntime: () => void
  sourceChatId?: string
}) {
  const toolById = new Map(tools.map((tool) => [tool.id, tool]))
  const hasPlanEvent = events.some((event) => event.type === 'plan')
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

  return (
    <div className="space-y-2">
      {events.map((event, index) => {
        if (event.type === 'assistant_text') {
          return <MarkdownContent key={`text-${index}`} content={event.text} />
        }

        if (event.type === 'thought') {
          return <ThinkingBlock key={`thought-${index}`} text={event.text} defaultOpen />
        }

        if (event.type === 'plan') {
          return (
            <PlanBlock
              key={`plan-${index}`}
              summary={planSummary}
              entries={planEntries}
              showActions={planActions?.show}
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
