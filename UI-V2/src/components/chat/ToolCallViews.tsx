// Tool call inline rows, permission cards, user-input cards, tool modal, and
// the MessageFrame wrapper. Extracted from ChatView.tsx (MO-3).

import { ReactNode, useCallback, useEffect, useId, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { ConversationTurn } from './ConversationTurn'
import { useToolQuestionDialog } from './useToolQuestionDialog'
import './ToolDetails.css'
import { ChevronLeft, ChevronRight, User, Pencil, RotateCcw, Wrench, X } from 'lucide-react'
import type {
  AcpPendingPermission,
  AcpPermissionOption,
  AcpToolCall,
} from '../../store/useAppStore'
import type { Message } from '../../types/message'
import { IconButton } from '../ui'
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

function managedTranscriptChatId(value: string) {
  return value.match(/"transcriptChatId"\s*:\s*"([^"\\]+)"/)?.[1] ?? ''
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
  const transcriptAvailable = Boolean(tool.subAgentId)
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
              {transcriptAvailable ? 'Subtask:' : 'Provider sub-agent event:'}
              <span style={{ fontSize: 10, fontWeight: 500, color: 'var(--text-3)', textTransform: 'none', letterSpacing: 0 }}>
                {tool.kind && tool.kind !== 'sub-agent' ? tool.kind : ''}
              </span>
            </span>
            <span className="truncate" style={{ fontSize: 12, color: 'var(--text)' }}>{displayTitle}</span>
          </span>
        </span>
        <span className="flex flex-col items-end gap-1">
          <ToolStatusIcon status={tool.status} />
          <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>{transcriptAvailable ? 'Transcript available' : 'Provider event'}</span>
        </span>
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
  onResolve: (requestId: string, optionId: string) => Promise<boolean>
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  onCancelTurn?: () => void
  onStopRuntime?: () => void
}) {
  const normalizedOptions = normalizePermissionOptions(permission.options)
  const [submittingOptionId, setSubmittingOptionId] = useState('')
  const [submitError, setSubmitError] = useState('')
  const submittingRef = useRef(false)

  useEffect(() => {
    submittingRef.current = false
    setSubmittingOptionId('')
    setSubmitError('')
  }, [permission.requestId])

  const resolve = async (optionId: string) => {
    if (submittingRef.current) return
    submittingRef.current = true
    setSubmittingOptionId(optionId)
    setSubmitError('')
    let accepted = false
    try {
      accepted = await onResolve(permission.requestId, optionId)
      if (!accepted) {
        setSubmitError('The provider did not accept that response. Try again.')
      }
    } catch {
      setSubmitError('The permission response failed. Try again.')
    } finally {
      if (!accepted) {
        submittingRef.current = false
        setSubmittingOptionId('')
      }
    }
  }

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
              disabled={Boolean(submittingOptionId)}
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
              disabled={Boolean(submittingOptionId)}
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
            disabled={Boolean(submittingOptionId)}
            aria-busy={submittingOptionId === option.id}
            onClick={() => void resolve(option.id)}
          >
            {submittingOptionId === option.id ? 'Submitting…' : option.displayName}
          </button>
        ))}
      </div>
      {submitError && <div role="alert" className="mt-2 text-[11px]" style={{ color: 'var(--error)' }}>{submitError}</div>}
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

export { UserInputInlineCard } from './QuestionInput'

type ToolCallModalProps = {
  tool: AcpToolCall
  chatId?: string
  onClose: () => void
  onOpenSubAgent?: () => void
  accentColor?: string
}

export function ToolCallModal(props: ToolCallModalProps) {
  return <ToolCallDetails key={`${props.chatId ?? ''}:${props.tool.id}`} {...props} />
}

function ToolCallDetails({
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
  type ToolContentPage = {
    content: string
    offset: number
    nextOffset: number
    previousOffset: number
    lastOffset: number
    totalBytes: number
    hasPrevious: boolean
    hasMore: boolean
  }
	const isLive = tool.status === 'running' || tool.status === 'in_progress' || tool.status === 'pending'
  const [contentPage, setContentPage] = useState<ToolContentPage | null>(null)
	const [contentPages, setContentPages] = useState<ToolContentPage[]>([])
  const [contentError, setContentError] = useState('')
  const [contentLoading, setContentLoading] = useState(false)
  const [retryOffset, setRetryOffset] = useState(0)
	const [followLive, setFollowLive] = useState(isLive)
  const contentRequestSerial = useRef(0)
  const [managedTranscript, setManagedTranscript] = useState<{ runId: string; title: string; status: string; executionCapability: string; messages: Array<{ role: string; content: string; thoughts: string }> } | null>(null)
  const [managedTranscriptError, setManagedTranscriptError] = useState('')
  const [managedTranscriptLoading, setManagedTranscriptLoading] = useState(false)
  const [managedResumeMessage, setManagedResumeMessage] = useState('')
  const shouldLoadContent = Boolean(tool.contentDeferred && chatId && isCefContext())
  const initialContentOffset = useRef(isLive ? Number.MAX_SAFE_INTEGER : 0).current
  const output = cleanToolOutput(
	shouldLoadContent && contentPages.length === 0
      ? contentLoading ? 'Loading tool output…' : 'Tool output is unavailable.'
	  : contentPages.length > 0
		? contentPages
			.slice()
			.sort((left, right) => left.offset - right.offset)
			.map((page, index, pages) => index > 0 && page.offset > pages[index - 1].nextOffset
				? `\n\n… ${page.offset - pages[index - 1].nextOffset} bytes not loaded …\n\n${page.content}`
				: page.content)
			.join('')
		: tool.content || 'No tool output yet.'
  )
  const transcriptChatId = chatId ? managedTranscriptChatId(output) : ''
  const toolCopyText = cleanToolOutput(contentPages.length
    ? [...contentPages].sort((left, right) => left.offset - right.offset).map(page => page.content).join('')
    : shouldLoadContent ? '' : tool.content)
  const [tab, setTab] = useState('Output')
  const dialogRef = useToolQuestionDialog(onClose)
  const outputRef = useRef<HTMLPreElement>(null)
  const id = useId()
  const tabs = transcriptChatId ? ['Output', 'Details', 'Transcript'] : ['Output', 'Details']
  const activeTab = tabs.includes(tab) ? tab : 'Output'
  const disposedRef = useRef(false)
  const transcriptBusyRef = useRef(false)
  const resumeBusyRef = useRef(false)
  useEffect(() => {
    disposedRef.current = false
    return () => { disposedRef.current = true; ++contentRequestSerial.current }
  }, [])
  useEffect(() => {
    if (isLive && followLive && outputRef.current) outputRef.current.scrollTop = outputRef.current.scrollHeight
  }, [output, followLive, isLive, activeTab])

  const loadContent = useCallback(async (offset: number, replace = false) => {
    if (!shouldLoadContent || !chatId) return
    const requestSerial = ++contentRequestSerial.current
    setRetryOffset(offset)
    setContentLoading(true)
    setContentError('')
    const response = await sendToCEF<ToolContentPage>({
      action: 'getToolCallContent',
      payload: { chatId, toolCallId: tool.id, offset },
    }).catch(() => ({ ok: false, error: 'Failed to load tool output.', data: undefined }))
    if (requestSerial !== contentRequestSerial.current) return
    setContentLoading(false)
    if (!response.ok || !response.data) {
      setContentError(response.error || 'Failed to load tool output.')
      return
    }
    setContentPage(response.data)
	setContentPages((current) => {
	  if (replace) return [response.data!]
	  const pages = current.filter((page) => page.offset !== response.data!.offset)
	  pages.push(response.data!)
	  return pages
	})
  }, [chatId, shouldLoadContent, tool.id])

  useEffect(() => {
    ++contentRequestSerial.current
    setContentPage(null)
	setContentPages([])
    setContentError('')
    setContentLoading(false)
	setRetryOffset(initialContentOffset)
	setFollowLive(isLive)
	if (shouldLoadContent) void loadContent(initialContentOffset, true)
	// The selected tool is fixed for the lifetime of this modal. A status change
	// must not discard pages the user has already loaded.
	// eslint-disable-next-line react-hooks/exhaustive-deps
  }, [initialContentOffset, loadContent, shouldLoadContent, tool.id])

	useEffect(() => {
	  if (!shouldLoadContent || !isLive || !followLive) return
	  const timer = window.setInterval(() => void loadContent(Number.MAX_SAFE_INTEGER, true), 1500)
	  return () => window.clearInterval(timer)
	}, [followLive, isLive, loadContent, shouldLoadContent])

  useEffect(() => {
    setManagedTranscript(null)
    setManagedTranscriptError('')
    setManagedTranscriptLoading(false)
    setManagedResumeMessage('')
  }, [tool.id])

  const openManagedTranscript = async () => {
    if (!chatId || !transcriptChatId || transcriptBusyRef.current) return
    transcriptBusyRef.current = true
    setManagedTranscriptLoading(true)
    setManagedTranscriptError('')
    const response = await sendToCEF<{ runId?: string; title?: string; status?: string; executionCapability?: string; messages?: unknown[] }>({
      action: 'getManagedAgentTranscript',
      payload: { chatId, transcriptChatId },
    }).catch(() => ({ ok: false, error: 'Managed agent transcript is unavailable.', data: undefined }))
    if (disposedRef.current) return
    transcriptBusyRef.current = false
    if (!response.ok) {
      setManagedTranscriptError(response.error || 'Managed agent transcript is unavailable.')
      setManagedTranscriptLoading(false)
      return
    }
    const messages = Array.isArray(response.data?.messages)
      ? response.data.messages.flatMap((value) => {
          if (!value || typeof value !== 'object') return []
          const message = value as Record<string, unknown>
          const role = typeof message.role === 'string' ? message.role : ''
          const content = typeof message.content === 'string' ? message.content : ''
          const thoughts = typeof message.thoughts === 'string' ? message.thoughts : ''
          return role && (content || thoughts) ? [{ role, content, thoughts }] : []
        })
      : []
    setManagedTranscript({
      runId: response.data?.runId || '',
      title: response.data?.title || 'Managed agent transcript',
      status: response.data?.status || 'unknown',
	  executionCapability: response.data?.executionCapability || 'unknown',
      messages,
    })
    setManagedTranscriptLoading(false)
  }

  const resumeManagedRun = async () => {
    if (!managedTranscript?.runId || managedTranscript.status !== 'interrupted' || resumeBusyRef.current) return
    resumeBusyRef.current = true
    setManagedResumeMessage('Resuming…')
    const response = await sendToCEF<{ runId?: string }>({
      action: 'resumeAgentRun',
      payload: { runId: managedTranscript.runId },
    }).catch(() => ({ ok: false, error: 'Failed to resume the interrupted run.', data: undefined }))
    if (disposedRef.current) return
    resumeBusyRef.current = response.ok
    setManagedResumeMessage(response.ok
      ? `Fresh run queued${response.data?.runId ? `: ${response.data.runId}` : '.'}`
      : response.error || 'Failed to resume the interrupted run.')
  }

  return createPortal(
    <div className="tool-details-backdrop" style={accentColor ? { '--accent': accentColor } as React.CSSProperties : undefined}
      onMouseDown={event => { if (event.target === event.currentTarget) onClose() }}>
      <section ref={dialogRef} role="dialog" aria-modal="true" aria-labelledby={`${id}-title`} tabIndex={-1} className="uam-tool-modal tool-details-dialog">
        <header className="tool-details-header">
          <ToolStatusIcon status={tool.status} />
          <h2 id={`${id}-title`}>{toolDisplayTitle(tool)}</h2>
          <CopyTextButton text={toolCopyText} label="Copy loaded output" title="Copy loaded output" />
          <IconButton size="sm" icon={<X size={17} aria-hidden />} label="Close tool details" onClick={onClose} />
        </header>
        <div role="tablist" aria-label="Tool information" className="tool-details-tabs">
          {tabs.map((name, index) => <button key={name} type="button" role="tab" id={`${id}-${name}-tab`}
            aria-controls={`${id}-panel`} aria-selected={activeTab === name} tabIndex={activeTab === name ? 0 : -1}
            onClick={() => setTab(name)} onKeyDown={event => {
              const next = event.key === 'ArrowRight' ? (index + 1) % tabs.length : event.key === 'ArrowLeft' ? (index + tabs.length - 1) % tabs.length : event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : -1
              if (next < 0) return
              event.preventDefault()
              setTab(tabs[next])
              document.getElementById(`${id}-${tabs[next]}-tab`)?.focus()
            }}>{name}</button>)}
        </div>
        <div role="tabpanel" id={`${id}-panel`} aria-labelledby={`${id}-${activeTab}-tab`} tabIndex={0}
          className={`tool-details-panel${activeTab === 'Output' ? ' tool-details-panel--output' : ''}`}>
          {activeTab === 'Output' && <>
          {isLive && !shouldLoadContent && <button type="button" className="tool-details-follow" aria-pressed={followLive} onClick={() => setFollowLive(value => !value)}>Follow live</button>}
          {shouldLoadContent && contentPage && (
            <div className="mb-2 flex flex-wrap items-center gap-2 text-[10px]" style={{ color: 'var(--text-3)' }}>
			  <button type="button" className="uam-choice-button h-7 px-2" disabled={contentLoading || !contentPage.hasPrevious} onClick={() => { setFollowLive(false); void loadContent(contentPage.previousOffset) }}>Load earlier</button>
			  <button type="button" className="uam-choice-button h-7 px-2" disabled={contentLoading || !contentPage.hasMore} onClick={() => { setFollowLive(false); void loadContent(contentPage.nextOffset) }}>Load later</button>
			  <button type="button" className="uam-choice-button h-7 px-2" aria-pressed={isLive ? followLive : undefined} disabled={contentLoading} onClick={() => { if (isLive && followLive) setFollowLive(false); else { setFollowLive(isLive); void loadContent(Number.MAX_SAFE_INTEGER, isLive) } }}>{isLive ? 'Follow live' : 'Load latest'}</button>
              <span aria-live="polite">
                {contentPage.totalBytes === 0
                  ? '0 bytes'
                  : `Bytes ${contentPage.offset + 1}–${contentPage.nextOffset} of ${contentPage.totalBytes}`}
              </span>
            </div>
          )}
          {contentError && (
            <div role="alert" className="mb-2 flex items-center gap-2 text-[11px]" style={{ color: 'var(--error)' }}>
              <span>{contentError}</span>
              <button type="button" className="uam-choice-button h-7 px-2" disabled={contentLoading} onClick={() => void loadContent(retryOffset)}>Retry</button>
            </div>
          )}
          <pre
            ref={outputRef}
            onScroll={event => {
              if (isLive && event.currentTarget.scrollHeight - event.currentTarget.scrollTop - event.currentTarget.clientHeight > 24) setFollowLive(false)
            }}
            aria-label="Tool output chunk"
            tabIndex={0}
            className="whitespace-pre-wrap text-xs uam-tool-modal__output"
          >
            {output}
          </pre>
          </>}
          {activeTab === 'Details' && <div className="tool-details-meta">
            <dl>
              <dt>Tool ID</dt><dd>{tool.id}</dd>
              <dt>Type</dt><dd>{[toolDisplayKind(tool), tool.kind].filter(Boolean).join(' / ') || 'tool'}</dd>
              <dt>Status</dt><dd>{tool.status || 'unknown'}</dd>
              {tool.isSubAgent && <><dt>Agent</dt><dd>{tool.subAgentId || tool.subAgentTitle || 'provider sub-agent'}</dd></>}
              {managedTranscript && <><dt>Run ID</dt><dd>{managedTranscript.runId}</dd><dt>Execution</dt><dd>{managedTranscript.executionCapability}</dd></>}
            </dl>
            {tool.isSubAgent && onOpenSubAgent && <button type="button" onClick={onOpenSubAgent}>Open chat</button>}
          </div>}
          {activeTab === 'Transcript' && <div className="tool-details-transcript">
          {transcriptChatId && !managedTranscript && (
            <button
              type="button"
              onClick={() => void openManagedTranscript()}
              disabled={managedTranscriptLoading}
              className="mt-3 h-8 rounded px-3 text-xs"
              style={{ border: '1px solid var(--border-bright)', background: 'var(--accent-dim)', color: 'var(--text)' }}
            >
              {managedTranscriptLoading ? 'Loading transcript…' : 'View managed agent transcript'}
            </button>
          )}
          {managedTranscriptError && <div role="alert" className="mt-2 text-xs" style={{ color: 'var(--red)' }}>{managedTranscriptError}</div>}
          {managedTranscript && (
            <section className="mt-3 grid gap-2" aria-label="Managed agent transcript">
              <div className="flex items-center justify-between gap-2 text-xs">
                <strong>{managedTranscript.title}</strong>
                <span className="flex items-center gap-2">
				  <span style={{ color: 'var(--text-3)' }}>{managedTranscript.status}</span>

                  {managedTranscript.status === 'interrupted' && (
                    <button
                      type="button"
                      onClick={() => void resumeManagedRun()}
                      disabled={resumeBusyRef.current}
                      className="h-7 rounded px-2"
                      style={{ border: '1px solid var(--border-bright)', background: 'var(--accent-dim)', color: 'var(--text)' }}
                    >Resume as fresh run</button>
                  )}
                </span>
              </div>
              {managedResumeMessage && <div role="status" className="text-xs" style={{ color: managedResumeMessage.startsWith('Failed') ? 'var(--red)' : 'var(--text-2)' }}>{managedResumeMessage}</div>}
              {managedTranscript.messages.length === 0 ? (
                <div className="text-xs" style={{ color: 'var(--text-3)' }}>No persisted messages yet.</div>
              ) : managedTranscript.messages.map((message, index) => (
                <div key={`${message.role}-${index}`} className="rounded border p-2 text-xs" style={{ borderColor: 'var(--border)', background: 'var(--bg)' }}>
                  <div className="mb-1 font-semibold capitalize" style={{ color: 'var(--text-2)' }}>{message.role}</div>
                  {message.thoughts && <div className="mb-2 whitespace-pre-wrap" style={{ color: 'var(--text-3)' }}>{message.thoughts}</div>}
                  <div className="whitespace-pre-wrap">{message.content}</div>
                </div>
              ))}
            </section>
          )}
          </div>}
        </div>
      </section>
    </div>, document.body
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
  if (role === 'user' || role === 'assistant') {
    return <ConversationTurn role={role} assistantLabel={assistantLabel} copyText={copyText}
      branchLabel={branchLabel} branchNavigation={branchNavigation} onEdit={onEdit} onRevert={onRevert}
      actionsDisabled={actionsDisabled} streaming={streaming} goalReview={goalReview}>{children}</ConversationTurn>
  }
  const accent = goalReview ? 'var(--purple)' : roleAccent(role)
  return (
    <div
      className="flex"
      style={{ justifyContent: 'flex-start' }}
    >
      <article
        className={`min-w-0 uam-message-frame${streaming ? ' is-streaming' : ''}${goalReview ? ' uam-message-frame--goal-review' : ''}`}
        data-message-kind={goalReview ? 'goal-review' : role}
        aria-label={goalReview ? 'Goal Reviewer' : roleLabel(role, assistantLabel)}
        style={{
          borderLeft: `2px solid ${accent}`,
          borderRadius: goalReview ? 8 : 0,
          padding: goalReview ? '8px 10px 10px 12px' : '2px 12px 2px 12px',
          background: goalReview
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
