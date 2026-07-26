// Tool call inline rows, permission cards, user-input cards, tool modal, and
// the MessageFrame wrapper. Extracted from ChatView.tsx (MO-3).

import { ReactNode, useEffect, useState } from 'react'
import { createPortal } from 'react-dom'
import { ChevronLeft, ChevronRight, User, Pencil, RotateCcw, Wrench } from 'lucide-react'
import type {
  AcpPendingPermission,
  AcpPendingUserInput,
  AcpPermissionOption,
  AcpToolCall,
  AcpUserInputAnswers,
} from '../../store/useAppStore'
import type { Message } from '../../types/message'
import { IconButton, Tooltip } from '../ui'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import {
  CopyTextButton,
  roleAccent,
  roleLabel,
  ToolStatusIcon,
  toolDisplayKind,
  toolDisplayTitle,
} from './StatusHelpers'

function cleanToolOutput(value: string) {
  return value
    .replace(/\\u001b\[[0-?]*[ -/]*[@-~]/gi, '')
    .replace(new RegExp(`${String.fromCharCode(27)}\\[[0-?]*[ -/]*[@-~]`, 'g'), '')
    .replace(/\\n/g, '\n')
}

export function SubAgentRunningPanel({
  tool,
  onSelectTool,
  renderHistory,
}: {
  tool: AcpToolCall
  onSelectTool: (toolId: string) => void
  renderHistory?: () => ReactNode
}) {
  const [open, setOpen] = useState(false)
  const displayTitle = toolDisplayTitle(tool)
  return (
    <details
      className="w-full uam-subagent-panel"
      open={open}
      onToggle={(event) => setOpen(event.currentTarget.open)}
      style={{
        borderRadius: 8,
        border: '1px solid color-mix(in srgb, var(--blue) 35%, var(--border-bright))',
        borderLeft: '3px solid var(--blue)',
        background: 'color-mix(in srgb, var(--blue) 8%, var(--surface))',
        color: 'var(--text)',
        boxShadow: '0 1px 0 color-mix(in srgb, var(--blue) 10%, transparent)',
      }}
    >
      <summary
        className="w-full text-left cursor-pointer"
        style={{
          display: 'grid',
          gridTemplateColumns: 'minmax(0, 1fr) auto',
          gap: 8,
          alignItems: 'center',
          padding: '10px 12px',
          listStyle: 'none',
        }}
      >
        <span className="min-w-0 flex items-center gap-2" style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
          <span
            aria-hidden="true"
            style={{
              display: 'inline-flex',
              alignItems: 'center',
              justifyContent: 'center',
              width: 22,
              height: 22,
              borderRadius: 6,
              background: 'var(--blue-dim)',
              color: 'var(--blue)',
              flexShrink: 0,
            }}
          >
            <User size={14} aria-hidden />
          </span>
          <span className="min-w-0" style={{ display: 'flex', flexDirection: 'column', minWidth: 0, gap: 2 }}>
            <span style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 10, fontWeight: 700, letterSpacing: '0.04em', textTransform: 'uppercase', color: 'var(--blue)' }}>
              Sub-agent:
              <span style={{ fontSize: 10, fontWeight: 500, color: 'var(--text-3)', textTransform: 'none', letterSpacing: 0 }}>
                {tool.kind && tool.kind !== 'sub-agent' ? tool.kind : ''}
              </span>
            </span>
            <span className="truncate" style={{ fontSize: 12, color: 'var(--text)' }}>{displayTitle}</span>
          </span>
        </span>
        <ToolStatusIcon status={tool.status} />
      </summary>
      {open && (
        <div className="space-y-3" style={{ borderTop: '1px solid var(--border)', padding: 12 }}>
          {renderHistory?.() ?? <pre className="whitespace-pre-wrap text-xs">{tool.content || 'No sub-agent history available yet.'}</pre>}
          <button
            type="button"
            onClick={() => onSelectTool(tool.id)}
            className="h-7 px-2 text-xs"
            style={{ border: '1px solid var(--border)', borderRadius: 5, background: 'var(--bg)', color: 'var(--text-2)' }}
          >
            Tool details
          </button>
        </div>
      )}
    </details>
  )
}

export function ToolCallInlineRows({
  tools,
  onSelectTool,
  renderSubAgentHistory,
}: {
  tools: AcpToolCall[]
  onSelectTool: (toolId: string) => void
  renderSubAgentHistory?: (tool: AcpToolCall) => ReactNode
}) {
  if (tools.length === 0) return null

  return (
    <div className="uam-tool-timeline">
      {tools.map((tool) => {
        if (tool.isSubAgent) {
          return (
            <div key={tool.id} className="uam-tool-timeline__item">
              <SubAgentRunningPanel tool={tool} onSelectTool={onSelectTool} renderHistory={renderSubAgentHistory ? () => renderSubAgentHistory(tool) : undefined} />
            </div>
          )
        }

        const displayTitle = toolDisplayTitle(tool)
        const normalizedStatus = tool.status.trim().toLowerCase()
        const active = normalizedStatus === 'running' || normalizedStatus === 'in_progress' || normalizedStatus === 'inprogress'
        return (
          <div key={tool.id} className="uam-tool-timeline__item">
            <button
              type="button"
              title="Open tool details"
              data-active={active}
              onClick={() => onSelectTool(tool.id)}
              className="w-full grid text-left uam-tool-row"
            >
              <Wrench className="uam-tool-row__icon" size={13} aria-hidden />
              <span className="uam-tool-row__kind">Tool</span>
              <span className="text-xs truncate" style={{ color: 'var(--text)' }}>{displayTitle}</span>
              <ToolStatusIcon status={tool.status} />
            </button>
          </div>
        )
      })}
    </div>
  )
}

export function PermissionInlineCard({
  permission,
  onResolve,
  waitIsStale,
  waitStaleReason,
  waitSeconds,
  onCancelTurn,
  onStopRuntime,
}: {
  permission: AcpPendingPermission
  onResolve: (requestId: string, optionId: string) => void
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  onCancelTurn?: () => void
  onStopRuntime?: () => void
}) {
  const normalizedOptions = normalizePermissionOptions(permission.options)

  return (
    <div
      className="my-2 uam-attention-card"
      style={{
        border: '1px solid var(--border-bright)',
        borderRadius: 7,
        padding: 10,
        background: 'color-mix(in srgb, var(--surface) 82%, var(--bg))',
      }}
    >
      <div className="text-xs font-semibold mb-1" style={{ color: 'var(--text)' }}>
        {permission.title || 'Permission required'}
      </div>
      {permission.content && (
        <pre
          className="text-[11px] whitespace-pre-wrap mb-2"
          style={{ color: 'var(--text-2)', fontFamily: 'inherit', overflowWrap: 'anywhere' }}
        >
          {permission.content}
        </pre>
      )}
      {permission.safetyRequiresApproval && (
        <div
          role="alert"
          className="mb-2 rounded-md px-2 py-1.5 text-[11px]"
          style={{ border: '1px solid color-mix(in srgb, var(--yellow) 55%, var(--border))', background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))', color: 'var(--text-2)' }}
        >
          {permission.safetyRisk === 'warn_high' ? 'High-risk' : 'Command safety'} warning ({permission.safetyTier ?? 'medium'} tier). Approve or deny this action.
        </div>
      )}
      {waitIsStale && (
        <div
          className="mb-2 text-[11px]"
          data-testid="stale-wait-warning"
          style={{
            border: '1px solid color-mix(in srgb, var(--yellow) 52%, var(--border))',
            borderRadius: 6,
            background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))',
            color: 'var(--text-2)',
            padding: '7px 8px',
          }}
        >
          <div className="font-medium" style={{ color: 'var(--text)' }}>
            This approval has not had runtime activity for {Math.max(120, waitSeconds ?? 0)}s.
          </div>
          <div>{waitStaleReason || 'The provider may be waiting on a stale command or tool request.'}</div>
          <div className="flex flex-wrap gap-2 mt-2">
            <button
              type="button"
              className="uam-choice-button px-2.5 h-7 text-[11px] font-medium"
              style={{
                borderRadius: 6,
                border: '1px solid var(--border-bright)',
                background: 'var(--surface-up)',
                color: 'var(--text)',
              }}
              onClick={onCancelTurn}
            >
              Cancel turn
            </button>
            <button
              type="button"
              className="uam-choice-button px-2.5 h-7 text-[11px]"
              style={{
                borderRadius: 6,
                border: '1px solid var(--border)',
                background: 'transparent',
                color: 'var(--text-2)',
              }}
              onClick={onStopRuntime}
            >
              Stop runtime
            </button>
          </div>
        </div>
      )}
      <div className="flex flex-wrap gap-2">
        {normalizedOptions.map((option) => (
          <button
            key={option.id}
            type="button"
            className="uam-choice-button px-3 h-7 text-[11px] font-medium"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: option.kind.startsWith('allow') ? 'var(--accent-dim)' : 'var(--surface-up)',
              color: 'var(--text)',
            }}
            onClick={() => onResolve(permission.requestId, option.id)}
          >
            {option.displayName}
          </button>
        ))}
      </div>
    </div>
  )
}

export function isCancelPermissionOption(option: AcpPermissionOption) {
  const id = option.id.trim().toLowerCase()
  const name = option.name.trim().toLowerCase()
  const kind = option.kind.trim().toLowerCase()
  return id === 'cancelled' || id === 'cancel' || name === 'cancel' || kind === 'cancel'
}

export function normalizePermissionOptions(options: AcpPermissionOption[]) {
  const byId = new Map<string, AcpPermissionOption>()
  let hasCancelOption = false

  for (const option of options) {
    if (isCancelPermissionOption(option)) {
      if (hasCancelOption) {
        continue
      }
      hasCancelOption = true
    }

    if (!byId.has(option.id)) {
      byId.set(option.id, option)
    }
  }

  const normalized = Array.from(byId.values())
  if (!hasCancelOption) {
    normalized.push({ id: 'cancelled', name: 'Cancel', kind: 'cancel' })
  }

  const labelCounts = new Map<string, number>()
  for (const option of normalized) {
    const label = option.name || option.id
    labelCounts.set(label, (labelCounts.get(label) ?? 0) + 1)
  }

  return normalized.map((option) => {
    const label = option.name || option.id
    return {
      ...option,
      displayName: (labelCounts.get(label) ?? 0) > 1 ? `${label} (${option.id})` : label,
    }
  })
}

export function UserInputInlineCard({
  input,
  onResolve,
  waitIsStale,
  waitStaleReason,
  waitSeconds,
  onCancelTurn,
  onStopRuntime,
}: {
  input: AcpPendingUserInput
  onResolve: (requestId: string, answers: AcpUserInputAnswers) => void
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  onCancelTurn?: () => void
  onStopRuntime?: () => void
}) {
  const [values, setValues] = useState<Record<string, string>>(() => {
    const initial: Record<string, string> = {}
    for (const question of input.questions) {
      initial[question.id] = ''
    }
    return initial
  })

  useEffect(() => {
    setValues((current) => {
      const next: Record<string, string> = {}
      for (const question of input.questions) {
        next[question.id] = current[question.id] ?? ''
      }
      return next
    })
  }, [input.requestId, input.questions])

  const canSubmit = input.questions.every((question) => (values[question.id] ?? '').trim().length > 0)
  const submit = () => {
    if (!canSubmit) return
    const answers: AcpUserInputAnswers = {}
    for (const question of input.questions) {
      answers[question.id] = [(values[question.id] ?? '').trim()]
    }
    onResolve(input.requestId, answers)
  }

  return (
    <div
      className="my-2 uam-attention-card"
      data-testid="user-input-card"
      style={{
        border: '1px solid color-mix(in srgb, var(--yellow) 56%, var(--border-bright))',
        borderLeft: '4px solid var(--yellow)',
        borderRadius: 7,
        padding: 10,
        background: 'color-mix(in srgb, var(--yellow) 9%, var(--surface))',
      }}
    >
      <div className="flex items-center gap-2 text-xs font-semibold mb-2" style={{ color: 'var(--text)' }}>
        <span style={{ color: 'var(--yellow)', fontSize: 9 }}>●</span>
        <span>Codex needs input</span>
      </div>
      {waitIsStale && (
        <div
          className="mb-3 text-[11px]"
          data-testid="stale-wait-warning"
          style={{
            border: '1px solid color-mix(in srgb, var(--yellow) 52%, var(--border))',
            borderRadius: 6,
            background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))',
            color: 'var(--text-2)',
            padding: '7px 8px',
          }}
        >
          <div className="font-medium" style={{ color: 'var(--text)' }}>
            This input request has not had runtime activity for {Math.max(120, waitSeconds ?? 0)}s.
          </div>
          <div>{waitStaleReason || 'The provider may be waiting on a stale input request.'}</div>
          <div className="flex flex-wrap gap-2 mt-2">
            <button
              type="button"
              className="uam-choice-button px-2.5 h-7 text-[11px] font-medium"
              style={{
                borderRadius: 6,
                border: '1px solid var(--border-bright)',
                background: 'var(--surface-up)',
                color: 'var(--text)',
              }}
              onClick={onCancelTurn}
            >
              Cancel turn
            </button>
            <button
              type="button"
              className="uam-choice-button px-2.5 h-7 text-[11px]"
              style={{
                borderRadius: 6,
                border: '1px solid var(--border)',
                background: 'transparent',
                color: 'var(--text-2)',
              }}
              onClick={onStopRuntime}
            >
              Stop runtime
            </button>
          </div>
        </div>
      )}
      <div className="space-y-3">
        {input.questions.map((question) => {
          const selected = values[question.id] ?? ''
          const showTextInput = question.isOther || question.options.length === 0
          return (
            <fieldset key={question.id} className="space-y-2" style={{ minWidth: 0 }}>
              {(question.header || question.question) && (
                <legend className="text-xs font-medium" style={{ color: 'var(--text)' }}>
                  {question.header || question.question}
                </legend>
              )}
              {question.header && question.question && (
                <div className="text-[11px]" style={{ color: 'var(--text-2)' }}>
                  {question.question}
                </div>
              )}
              {question.options.length > 0 && (
                <div className="flex flex-wrap gap-2">
                  {question.options.map((option) => {
                    const active = selected === option.label
                    return (
                      <button
                        key={`${question.id}-${option.label}`}
                        type="button"
                        className="uam-choice-button px-3 py-1.5 text-[11px] text-left"
                        style={{
                          borderRadius: 6,
                          border: active
                            ? '1px solid color-mix(in srgb, var(--accent) 72%, var(--border-bright))'
                            : '1px solid var(--border)',
                          background: active ? 'var(--accent-dim)' : 'var(--surface-up)',
                          color: 'var(--text)',
                        }}
                        onClick={() =>
                          setValues((current) => ({
                            ...current,
                            [question.id]: option.label,
                          }))
                        }
                      >
                        <span className="block font-medium">{option.label}</span>
                        {option.description && (
                          <span className="block mt-0.5" style={{ color: 'var(--text-3)' }}>
                            {option.description}
                          </span>
                        )}
                      </button>
                    )
                  })}
                </div>
              )}
              {showTextInput && (
                <input
                  type={question.isSecret ? 'password' : 'text'}
                  value={selected}
                  aria-label={question.question || question.header || question.id}
                  className="w-full text-xs outline-none"
                  style={{
                    height: 30,
                    borderRadius: 6,
                    border: '1px solid var(--border)',
                    background: 'var(--bg)',
                    color: 'var(--text)',
                    padding: '0 9px',
                  }}
                  onChange={(event) =>
                    setValues((current) => ({
                      ...current,
                      [question.id]: event.target.value,
                    }))
                  }
                />
              )}
            </fieldset>
          )
        })}
      </div>
      <div className="flex justify-end pt-3">
        <button
          type="button"
          className="uam-choice-button px-3 h-7 text-[11px] font-medium"
          disabled={!canSubmit}
          style={{
            borderRadius: 6,
            border: '1px solid var(--border-bright)',
            background: canSubmit ? 'var(--accent)' : 'var(--surface-up)',
            color: canSubmit ? '#ffffff' : 'var(--text-3)',
          }}
          onClick={submit}
        >
          Submit
        </button>
      </div>
    </div>
  )
}

export function ToolCallModal({
  tool,
  chatId,
  onClose,
  onOpenSubAgent,
  accentColor,
}: {
  tool: AcpToolCall
  chatId?: string
  onClose: () => void
  onOpenSubAgent?: () => void
  accentColor?: string
}) {
  const [deferredContent, setDeferredContent] = useState<string | null>(null)
  const [contentError, setContentError] = useState('')
  const shouldLoadContent = Boolean(tool.contentDeferred && chatId && isCefContext())
  const output = cleanToolOutput(
    shouldLoadContent && deferredContent === null
      ? contentError || 'Loading tool output…'
      : deferredContent ?? (tool.content || 'No tool output yet.')
  )
  const toolCopyText = [
    toolDisplayTitle(tool) || tool.id || 'Tool call',
    `id: ${tool.id || 'unknown'}`,
    `kind: ${tool.kind || 'unknown'}`,
    `status: ${tool.status || 'unknown'}`,
    ...(tool.isSubAgent ? [`subAgentId: ${tool.subAgentId || 'unknown'}`, `subAgentTitle: ${tool.subAgentTitle || 'unknown'}`] : []),
    '',
    output,
  ].join('\n')

  useEffect(() => {
    if (!shouldLoadContent || !chatId)
    {
      setDeferredContent(null)
      setContentError('')
      return
    }

    let canceled = false
    setDeferredContent(null)
    setContentError('')
    void sendToCEF<{ content?: string }>({
      action: 'getToolCallContent',
      payload: { chatId, toolCallId: tool.id },
    }).then((response) => {
      if (canceled) return
      if (!response.ok) {
        setContentError(response.error || 'Failed to load tool output.')
        return
      }
      setDeferredContent(response.data?.content ?? '')
    })
    return () => { canceled = true }
  }, [chatId, shouldLoadContent, tool.id])

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [onClose])

  return createPortal(
    <div
      className="fixed inset-0 flex items-center justify-center uam-tool-modal-backdrop"
      style={{
        zIndex: 1000,
        background: 'rgba(0, 0, 0, 0.48)',
        padding: 18,
        ...(accentColor ? {
          '--accent': accentColor,
          '--accent-dim': `color-mix(in srgb, ${accentColor} 12%, transparent)`,
        } : {}),
      }}
      onMouseDown={onClose}
    >
      <section
        role="dialog"
        aria-modal="true"
        aria-label="Tool details"
        tabIndex={-1}
        className="w-full uam-tool-modal"
        style={{
          maxWidth: 680,
          maxHeight: 'min(720px, 88vh)',
          overflow: 'hidden',
          borderRadius: 8,
          border: '1px solid var(--border-bright)',
          background: 'var(--surface)',
          boxShadow: '0 22px 70px rgba(0, 0, 0, 0.42)',
        }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="uam-tool-modal__header">
          <ToolStatusIcon status={tool.status} />
          <div className="min-w-0 flex-1">
            <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
              {toolDisplayTitle(tool)}
            </div>
            <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>
              {[toolDisplayKind(tool), tool.kind].filter(Boolean).join(' / ') || 'tool call'}
            </div>
          </div>
          <div className="flex items-center gap-2">
            {tool.isSubAgent && onOpenSubAgent && (
              <Tooltip label="Open sub-agent chat">
                <button
                  type="button"
                  onClick={onOpenSubAgent}
                  className="px-2 h-7 text-xs"
                  style={{
                    borderRadius: 5,
                    border: '1px solid var(--border-bright)',
                    background: 'var(--accent-dim)',
                    color: 'var(--text)',
                  }}
                >
                  Open chat
                </button>
              </Tooltip>
            )}
            <CopyTextButton text={toolCopyText} label="Copy" title="Copy tool output" />
          </div>
          <Tooltip label="Close tool details">
            <button
              type="button"
              aria-label="Close tool details"
              onClick={onClose}
              className="px-2 h-7 text-xs"
              style={{
                borderRadius: 5,
                border: '1px solid var(--border)',
                background: 'var(--bg)',
                color: 'var(--text-2)',
              }}
            >
              Close
            </button>
          </Tooltip>
        </div>
        <div className="uam-tool-modal__body">
          <div className="uam-tool-modal__meta">
            <span>{tool.kind || 'tool'}</span>
            <span>{tool.status || 'unknown'}</span>
            <span title={tool.id || 'unknown'}>{tool.id || 'unknown'}</span>
            {tool.isSubAgent && (
              <span>{tool.subAgentId || tool.subAgentTitle || 'provider sub-agent'}</span>
            )}
          </div>
          <div className="uam-tool-modal__output-label">Output</div>
          <pre
            className="whitespace-pre-wrap text-xs uam-tool-modal__output"
          >
            {output}
          </pre>
        </div>
      </section>
    </div>,
    document.body
  )
}

export function MessageFrame({
  role,
  children,
  assistantLabel,
  copyText = '',
  branchLabel,
  branchNavigation,
  onEdit,
  onRevert,
  actionsDisabled = false,
  streaming = false,
  goalReview = false,
}: {
  role: Message['role']
  children: ReactNode
  assistantLabel: string
  copyText?: string
  branchLabel?: string
  branchNavigation?: {
    current: number
    total: number
    onPrevious: () => void
    onNext: () => void
  }
  onEdit?: () => void
  onRevert?: () => void
  actionsDisabled?: boolean
  streaming?: boolean
  goalReview?: boolean
}) {
  const accent = goalReview ? 'var(--purple)' : roleAccent(role)
  return (
    <div
      className="flex"
      style={{ justifyContent: role === 'user' ? 'flex-end' : 'flex-start' }}
    >
      <article
        className={`min-w-0 uam-message-frame${streaming ? ' is-streaming' : ''}${goalReview ? ' uam-message-frame--goal-review' : ''}`}
        data-message-kind={goalReview ? 'goal-review' : role}
        aria-label={goalReview ? 'Goal Reviewer' : roleLabel(role, assistantLabel)}
        style={{
          borderLeft: role !== 'user' ? `2px solid ${accent}` : undefined,
          borderRadius: goalReview ? 8 : 0,
          padding: role === 'user' ? undefined : goalReview ? '8px 10px 10px 12px' : '2px 12px 2px 12px',
          background: role === 'user'
            ? undefined
            : goalReview
              ? 'color-mix(in srgb, var(--purple) 5%, var(--message-assistant-bg))'
              : 'var(--message-assistant-bg)',
          color: 'var(--text)',
        }}
      >
        <div className="uam-message-frame__header flex items-center gap-1.5 text-[11px] mb-1" style={{ color: accent }}>
          <span>{goalReview ? 'Goal Reviewer' : roleLabel(role, assistantLabel)}</span>
          {branchLabel && (
            <span className="rounded px-1.5 py-0.5 text-[10px]" style={{ background: 'var(--accent-dim)', color: 'var(--accent)' }}>
              {branchLabel}
            </span>
          )}
          {(copyText.trim() || onEdit || onRevert) && (
            <span className="ml-auto flex items-center gap-1 uam-message-frame__actions">
              <CopyTextButton text={copyText} label="Copy message" title="Copy message" />
              {onEdit && (
                <IconButton
                  icon={<Pencil size={13} aria-hidden />}
                  label="Edit message in new branch"
                  size="sm"
                  disabled={actionsDisabled}
                  onClick={onEdit}
                />
              )}
              {onRevert && (
                <IconButton
                  icon={<RotateCcw size={13} aria-hidden />}
                  label="Revert to message in new branch"
                  size="sm"
                  disabled={actionsDisabled}
                  onClick={onRevert}
                />
              )}
            </span>
          )}
        </div>
        {children}
        {branchNavigation && (
          <div
            className="uam-message-frame__actions uam-message-frame__branch-navigation"
            role="group"
            aria-label="Message branches"
          >
            <IconButton
              icon={<ChevronLeft size={13} aria-hidden />}
              label="Previous message branch"
              size="sm"
              disabled={branchNavigation.current <= 1}
              onClick={branchNavigation.onPrevious}
            />
            <span className="min-w-[36px] text-center text-[10px] tabular-nums" style={{ color: 'var(--text-3)' }}>
              {branchNavigation.current} / {branchNavigation.total}
            </span>
            <IconButton
              icon={<ChevronRight size={13} aria-hidden />}
              label="Next message branch"
              size="sm"
              disabled={branchNavigation.current >= branchNavigation.total}
              onClick={branchNavigation.onNext}
            />
          </div>
        )}
      </article>
    </div>
  )
}
