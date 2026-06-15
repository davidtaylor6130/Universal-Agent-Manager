// Status labels, colors, and diagnostic display helpers for the chat view.
// Extracted from ChatView.tsx (MO-3).

import { useEffect, useRef, useState } from 'react'
import type { AcpBinding, AcpToolCall } from '../../store/useAppStore'
import { copyTextToClipboard } from '../../utils/copySelection'

export function statusLabel(acp?: AcpBinding) {
  if (!acp) return 'Stopped'
  if (acp.lifecycleState === 'waitingPermission') return 'Permission'
  if (acp.lifecycleState === 'waitingUserInput') return 'Input'
  if (acp.processing) return 'Running'
  if (acp.lifecycleState === 'error') return 'Error'
  if (acp.running) return 'Ready'
  return 'Stopped'
}

export function statusColor(acp?: AcpBinding) {
  if (!acp) return 'var(--text-3)'
  if (acp.lifecycleState === 'error') return 'var(--red)'
  if (acp.lifecycleState === 'waitingPermission') return 'var(--yellow)'
  if (acp.lifecycleState === 'waitingUserInput') return 'var(--yellow)'
  if (acp.processing) return 'var(--blue)'
  if (acp.running) return 'var(--green)'
  return 'var(--text-3)'
}

export function toolStatusColor(tool: AcpToolCall) {
  if (tool.status === 'completed') return 'var(--green)'
  if (tool.status === 'failed') return 'var(--red)'
  if (tool.status === 'in_progress') return 'var(--blue)'
  if (tool.status === 'running') return 'var(--blue)'
  return 'var(--text-3)'
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
    <button
      type="button"
      title={title}
      onClick={onCopy}
      className="px-2 h-6 text-[11px]"
      style={{
        borderRadius: 5,
        border: '1px solid var(--border)',
        background: status === 'failed' ? 'color-mix(in srgb, var(--red) 16%, var(--surface))' : 'var(--surface-up)',
        color: status === 'copied' ? 'var(--green)' : status === 'failed' ? 'var(--red)' : 'var(--text-2)',
      }}
    >
      {status === 'copied' ? 'Copied' : status === 'failed' ? 'Copy failed' : label}
    </button>
  )
}

export function AcpErrorDetails({ acp, title }: { acp: AcpBinding; title: string }) {
  const diagnostics = acp.diagnostics.slice(-12)
  const diagnosticsText = diagnostics.map(formatDiagnosticLine).join('\n\n')
  const hasDetails =
    diagnostics.length > 0 ||
    acp.recentStderr.trim().length > 0 ||
    acp.lastExitCode !== null

  if (!hasDetails) return null

  return (
    <details className="mt-2">
      <summary className="cursor-pointer select-none" style={{ color: 'var(--text-2)' }}>
        Diagnostics
      </summary>
      <div className="mt-2 grid gap-2">
        <div className="flex justify-end">
          <CopyTextButton text={buildAcpErrorCopyText(acp, title)} label="Copy diagnostics" title="Copy diagnostics" />
        </div>
        {acp.lastExitCode !== null && (
          <div style={{ color: 'var(--text-2)' }}>Exit code: {acp.lastExitCode}</div>
        )}
        {diagnostics.length > 0 && (
          <pre
            className="text-[11px]"
            style={{
              margin: 0,
              maxHeight: 180,
              overflow: 'auto',
              whiteSpace: 'pre-wrap',
              wordBreak: 'break-word',
              border: '1px solid var(--border)',
              borderRadius: 6,
              padding: 8,
              background: 'var(--bg)',
              color: 'var(--text-2)',
            }}
          >
            {diagnosticsText}
          </pre>
        )}
        {acp.recentStderr.trim().length > 0 && (
          <pre
            className="text-[11px]"
            style={{
              margin: 0,
              maxHeight: 180,
              overflow: 'auto',
              whiteSpace: 'pre-wrap',
              wordBreak: 'break-word',
              border: '1px solid var(--border)',
              borderRadius: 6,
              padding: 8,
              background: 'var(--bg)',
              color: 'var(--text-2)',
            }}
          >
            {diagnosticTail(acp.recentStderr)}
          </pre>
        )}
      </div>
    </details>
  )
}
