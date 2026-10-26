// Status labels, colors, and diagnostic display helpers for the chat view.

import { useEffect, useRef, useState } from 'react'
import { Check, Circle, CircleCheck, CircleX, Clock3, Copy, LoaderCircle } from 'lucide-react'
import type { ReactNode } from 'react'
import type { LucideIcon } from 'lucide-react'
import type { AcpBinding, AcpToolCall, CliBinding } from '../../store/useAppStore'
import { copyTextToClipboard } from '../../utils/copySelection'
import { Tooltip } from '../ui'

/** Bar for a token quantity; callers pick the tone and keep exact numbers in text. */
export function ContextMeter({ ratio, level = 'ok' }: { ratio: number; level?: 'ok' | 'warning' | 'error' }) {
  return <span className="conversation-meter" data-level={level} aria-hidden>
    <span style={{ transform: `scaleX(${Math.min(1, Math.max(0, ratio))})` }} />
  </span>
}

const STATUS_STRIP_EXIT_MS = 200

function formatElapsed(seconds: number) {
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  if (minutes < 60) return `${minutes}m ${String(seconds % 60).padStart(2, '0')}s`
  return `${Math.floor(minutes / 60)}h ${String(minutes % 60).padStart(2, '0')}m`
}

/** Seconds since the strip opened, ticking once a second only while it is visible. */
function useElapsedSeconds(open: boolean) {
  const startedAt = useRef<number | null>(null)
  const [, setTick] = useState(0)
  if (open && startedAt.current === null) startedAt.current = Date.now()
  if (!open) startedAt.current = null
  useEffect(() => {
    if (!open) return
    const timer = window.setInterval(() => setTick((value) => value + 1), 1000)
    return () => window.clearInterval(timer)
  }, [open])
  return startedAt.current === null ? 0 : Math.max(0, Math.floor((Date.now() - startedAt.current) / 1000))
}

export type ChatStatusTone = 'info' | 'agents' | 'warning'

/**
 * One centred status divider above the composer, styled like the compacted-context line in the transcript:
 * a hairline either side of a tiny illustrated scene, the title and a live elapsed timer.
 * Keeps its last content mounted briefly so the exit can animate.
 */
export function ChatStatusStrip({ open, tone, scene, title, detail, timeMode = 'elapsed' }: {
  open: boolean
  tone: ChatStatusTone
  scene: ReactNode
  title: string
  detail?: ReactNode
  /** 'elapsed' ticks while work runs; 'since' shows the clock time the state began (e.g. when it stopped). */
  timeMode?: 'elapsed' | 'since'
}) {
  const [present, setPresent] = useState(open)
  if (open && !present) setPresent(true)
  const lastShown = useRef<{ tone: ChatStatusTone; scene: ReactNode; title: string; detail?: ReactNode } | null>(null)
  if (open) lastShown.current = { tone, scene, title, detail }
  const elapsed = useElapsedSeconds(open && timeMode === 'elapsed')
  // The provider reports no stop time, so 'since' records the moment the strip first appeared.
  const since = useRef<number | null>(null)
  if (open && since.current === null) since.current = Date.now()
  if (!open && !present) since.current = null
  const frozenElapsed = useRef(0)
  if (open) frozenElapsed.current = elapsed
  useEffect(() => {
    if (open || !present) return
    const timer = window.setTimeout(() => setPresent(false), STATUS_STRIP_EXIT_MS)
    return () => window.clearTimeout(timer)
  }, [open, present])
  if (!present || !lastShown.current) return null
  const shown = lastShown.current
  return (
    <div role="status" className="chat-status-strip" data-tone={shown.tone} data-state={open ? 'open' : 'closed'}>
      <span className="chat-status-strip__pill">
        <span className="chat-status-strip__scene">{shown.scene}</span>
        <span className="chat-status-strip__title">{shown.title}</span>
        {timeMode === 'since' && since.current !== null
          ? <time className="chat-status-strip__time" dateTime={new Date(since.current).toISOString()}>since {new Date(since.current).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}</time>
          : <span className="chat-status-strip__time" aria-label={`for ${formatElapsed(frozenElapsed.current)}`}>{formatElapsed(frozenElapsed.current)}</span>}
        {shown.detail && <span className="chat-status-strip__detail">{shown.detail}</span>}
      </span>
    </div>
  )
}

export function statusLabel(runtime?: AcpBinding | CliBinding) {
  if (!runtime) return 'Stopped'
  if (runtime.lifecycleState === 'unknown') return runtime.running ? 'Connected' : 'Stopped'
  if (runtime.lifecycleState === 'waitingPermission') return 'Permission'
  if (runtime.lifecycleState === 'waitingUserInput') return 'Input'
  if (runtime.processing) return 'Running'
  if (runtime.lifecycleState === 'error') return 'Error'
  if (runtime.running) return 'Ready'
  return 'Stopped'
}

export function statusColor(runtime?: AcpBinding | CliBinding) {
  if (!runtime || runtime.lifecycleState === 'unknown') return 'var(--text-3)'
  if (runtime.lifecycleState === 'error') return 'var(--red)'
  if (runtime.lifecycleState === 'waitingPermission') return 'var(--yellow)'
  if (runtime.lifecycleState === 'waitingUserInput') return 'var(--yellow)'
  if (runtime.processing) return 'var(--blue)'
  if (runtime.running) return 'var(--green)'
  return 'var(--text-3)'
}

export function ToolStatusIcon({ status }: { status: string }) {
  const normalized = status.trim().toLowerCase()
  let Icon: LucideIcon = Circle
  let label = normalized ? normalized.replace(/_/g, ' ') : 'unknown'
  let color = 'var(--text-3)'

  if (normalized === 'completed' || normalized === 'complete' || normalized === 'success' || normalized === 'succeeded') {
    Icon = CircleCheck
    label = 'completed'
    color = 'var(--green)'
  } else if (normalized === 'failed' || normalized === 'error' || normalized === 'cancelled' || normalized === 'canceled') {
    Icon = CircleX
    color = 'var(--red)'
  } else if (normalized === 'running' || normalized === 'in_progress' || normalized === 'inprogress') {
    Icon = LoaderCircle
    label = 'running'
    color = 'var(--blue)'
  } else if (normalized === 'pending') {
    Icon = Clock3
  }

  const accessibleLabel = `Tool status: ${label.charAt(0).toUpperCase()}${label.slice(1)}`
  return (
    <Tooltip label={accessibleLabel}>
      <span
        data-tool-status={normalized || 'unknown'}
        aria-label={accessibleLabel}
        className="inline-flex shrink-0 items-center justify-center"
        style={{ color }}
      >
        <Icon size={15} aria-hidden />
        <span className="sr-only">{accessibleLabel}</span>
      </span>
    </Tooltip>
  )
}

export function toolDisplayKind(tool: AcpToolCall) {
  return tool.isSubAgent ? 'Sub-agent' : 'Tool call'
}

export function toolDisplayTitle(tool: AcpToolCall) {
  return tool.isSubAgent
    ? (tool.subAgentTitle || tool.title || tool.subAgentId || tool.id)
    : (tool.title || tool.id)
}

export function roleAccent(role: string) {
  if (role === 'user') return 'var(--accent)'
  if (role === 'assistant') return 'var(--blue)'
  return 'var(--yellow)'
}

export function roleLabel(role: string, assistantLabel: string) {
  if (role === 'user') return 'You'
  if (role === 'assistant') return assistantLabel
  return 'System'
}

export function diagnosticTail(value: string, maxChars = 6000) {
  if (value.length <= maxChars) return value
  return `[showing last ${maxChars} chars]\n${value.slice(value.length - maxChars)}`
}

export function formatDiagnosticLine(entry: AcpBinding['diagnostics'][number]) {
  const parts = [
    entry.time,
    entry.event,
    entry.reason,
    entry.method ? `method=${entry.method}` : '',
    entry.requestId ? `id=${entry.requestId}` : '',
    typeof entry.code === 'number' ? `code=${entry.code}` : '',
    entry.lifecycleState ? `state=${entry.lifecycleState}` : '',
  ].filter(Boolean)
  const headline = parts.join(' ')
  const body = [entry.message, entry.detail].filter(Boolean).join('\n')
  return body ? `${headline}\n${body}` : headline
}

export function buildAcpErrorCopyText(acp: AcpBinding, title: string) {
  const lines = [title]
  if (acp.lastError.trim()) {
    lines.push('', acp.lastError.trim())
  }
  if (acp.lastExitCode !== null) {
    lines.push('', `Exit code: ${acp.lastExitCode}`)
  }
  if (acp.diagnostics.length > 0) {
    lines.push('', 'Diagnostics', acp.diagnostics.map(formatDiagnosticLine).join('\n\n'))
  }
  if (acp.recentStderr.trim()) {
    lines.push('', 'Recent stderr', diagnosticTail(acp.recentStderr))
  }
  return lines.join('\n')
}

export function CopyTextButton({
  text,
  label = 'Copy',
  title = 'Copy text',
}: {
  text: string
  label?: string
  title?: string
}) {
  const [status, setStatus] = useState<'idle' | 'copied' | 'failed'>('idle')
  const resetTimerRef = useRef<number | null>(null)

  useEffect(() => () => {
    if (resetTimerRef.current !== null) {
      window.clearTimeout(resetTimerRef.current)
    }
  }, [])

  const onCopy = async () => {
    const copied = await copyTextToClipboard(text, document)
    setStatus(copied ? 'copied' : 'failed')
    if (resetTimerRef.current !== null) {
      window.clearTimeout(resetTimerRef.current)
    }
    resetTimerRef.current = window.setTimeout(() => setStatus('idle'), 1600)
  }

  return (
    <Tooltip label={status === 'copied' ? 'Copied' : status === 'failed' ? 'Copy failed' : title}>
      <button
        type="button"
        aria-label={label}
        onClick={onCopy}
        className="inline-flex h-6 w-6 items-center justify-center"
        style={{
          borderRadius: 5,
          border: 0,
          background: 'transparent',
          color: status === 'copied' ? 'var(--green)' : status === 'failed' ? 'var(--red)' : 'var(--text-2)',
          transition: 'color var(--dur-fast) var(--ease), transform var(--dur-fast) var(--ease)',
        }}
      >
        {status === 'copied' ? <Check size={13} aria-hidden /> : status === 'failed' ? <CircleX size={13} aria-hidden /> : <Copy size={13} aria-hidden />}
        <span className="sr-only">{status === 'copied' ? 'Copied' : status === 'failed' ? 'Copy failed' : label}</span>
      </button>
    </Tooltip>
  )
}
