import { FormEvent, KeyboardEvent, ReactNode, RefObject, useEffect, useMemo, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Session } from '../../types/session'
import {
  useAppStore,
  type AcpBinding,
  type AcpModel,
  type AcpPendingPermission,
  type AcpPendingUserInput,
  type AcpPlanEntry,
  type AcpToolCall,
  type AcpTurnEvent,
  type AcpUserInputAnswers,
} from '../../store/useAppStore'
import type { Message, MessageBlock } from '../../types/message'
import type { Provider } from '../../types/provider'
import { copyTextToClipboard } from '../../utils/copySelection'

interface ChatViewProps {
  session: Session
}

interface SelectedToolCallRef {
  id: string
  messageId?: string
}

interface ModelOption {
  id: string
  label: string
  shortLabel: string
  detail: string
}

const GEMINI_FALLBACK_ACP_MODEL_OPTIONS: ModelOption[] = [
  { id: '', label: 'CLI default', shortLabel: 'CLI default', detail: 'Use Gemini CLI settings' },
  { id: 'auto-gemini-3', label: 'Auto 3', shortLabel: 'Auto 3', detail: 'Gemini 3 routing' },
  { id: 'auto-gemini-2.5', label: 'Auto 2.5', shortLabel: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  { id: 'pro', label: 'Pro', shortLabel: 'Pro', detail: 'Prioritize capability' },
  { id: 'flash', label: 'Flash', shortLabel: 'Flash', detail: 'Prioritize speed' },
  { id: 'flash-lite', label: 'Flash Lite', shortLabel: 'Flash Lite', detail: 'Fastest option' },
]

const FRIENDLY_MODEL_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'CLI default', detail: 'Use Gemini CLI settings' },
  'auto-gemini-3': { label: 'Auto 3', shortLabel: 'Auto 3', detail: 'Gemini 3 routing' },
  'auto-gemini-2.5': { label: 'Auto 2.5', shortLabel: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  pro: { label: 'Pro', shortLabel: 'Pro', detail: 'Prioritize capability' },
  flash: { label: 'Flash', shortLabel: 'Flash', detail: 'Prioritize speed' },
  'flash-lite': { label: 'Flash Lite', shortLabel: 'Flash Lite', detail: 'Fastest option' },
}

const PLAN_APPROVE_PROMPT = 'Proceed with the plan.'
const PLAN_DENY_PROMPT = 'Do not proceed with this plan. Please revise it before making changes.'

function providerDisplayName(provider?: Provider, fallbackId = '') {
  if (provider?.shortName?.trim()) return provider.shortName.trim()
  if (provider?.name?.trim()) return provider.name.trim()
  if (fallbackId === 'codex-cli') return 'Codex'
  return 'Gemini'
}

function providerDefaultModelOption(providerName: string): ModelOption {
  return {
    id: '',
    label: 'CLI default',
    shortLabel: 'CLI default',
    detail: `Use ${providerName} CLI settings`,
  }
}

function providerRuntimeLabel(provider?: Provider, acp?: AcpBinding) {
  const protocol = acp?.protocolKind || provider?.structuredProtocol || 'gemini-acp'
  if (protocol === 'codex-app-server') return 'App Server'
  if (protocol === 'none') return 'CLI'
  return 'ACP'
}

function isCodexProvider(provider?: Provider, providerId = '') {
  return providerId === 'codex-cli' || provider?.structuredProtocol === 'codex-app-server'
}

function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
}

function modelOptionFromRuntime(model: AcpModel): ModelOption | null {
  const id = model.id.trim()
  if (!id) return null
  const friendly = FRIENDLY_MODEL_LABELS[id]
  if (friendly) {
    return { id, ...friendly, detail: model.description || friendly.detail }
  }
  const label = model.name.trim() || titleFromModelId(id)
  return {
    id,
    label,
    shortLabel: label.length <= 16 ? label : titleFromModelId(id),
    detail: model.description.trim() || id,
  }
}

function buildModelOptions(
  acp: AcpBinding | undefined,
  selectedModelId: string,
  provider: Provider | undefined,
  providerId: string
): ModelOption[] {
  const providerName = providerDisplayName(provider, providerId)
  const runtimeOptions = (acp?.availableModels ?? []).flatMap((model) => {
    const option = modelOptionFromRuntime(model)
    return option ? [option] : []
  })
  const defaultOption = providerDefaultModelOption(providerName)
  const fallbackOptions = isCodexProvider(provider, providerId)
    ? [defaultOption]
    : [defaultOption, ...GEMINI_FALLBACK_ACP_MODEL_OPTIONS.slice(1)]
  const baseOptions = runtimeOptions.length > 0
    ? [defaultOption, ...runtimeOptions]
    : fallbackOptions
  const options: ModelOption[] = []
  const seen = new Set<string>()

  for (const option of baseOptions) {
    if (seen.has(option.id)) continue
    seen.add(option.id)
    options.push(option)
  }

  if (selectedModelId && !seen.has(selectedModelId)) {
    const friendly = FRIENDLY_MODEL_LABELS[selectedModelId]
    options.push(
      friendly
        ? { id: selectedModelId, ...friendly }
        : {
            id: selectedModelId,
            label: titleFromModelId(selectedModelId),
            shortLabel: titleFromModelId(selectedModelId),
            detail: selectedModelId,
          }
    )
  }

  return options
}

function modelOptionFor(options: ModelOption[], modelId?: string) {
  return options.find((option) => option.id === (modelId ?? '')) ?? options[0] ?? providerDefaultModelOption('provider')
}

function statusLabel(acp?: AcpBinding) {
  if (!acp) return 'Stopped'
  if (acp.lifecycleState === 'waitingPermission') return 'Permission'
  if (acp.lifecycleState === 'waitingUserInput') return 'Input'
  if (acp.processing) return 'Running'
  if (acp.lifecycleState === 'error') return 'Error'
  if (acp.running) return 'Ready'
  return 'Stopped'
}

function statusColor(acp?: AcpBinding) {
  if (!acp) return 'var(--text-3)'
  if (acp.lifecycleState === 'error') return 'var(--red)'
  if (acp.lifecycleState === 'waitingPermission') return 'var(--yellow)'
  if (acp.lifecycleState === 'waitingUserInput') return 'var(--yellow)'
  if (acp.processing) return 'var(--blue)'
  if (acp.running) return 'var(--green)'
  return 'var(--text-3)'
}

function toolStatusColor(tool: AcpToolCall) {
  if (tool.status === 'completed') return 'var(--green)'
  if (tool.status === 'failed') return 'var(--red)'
  if (tool.status === 'in_progress') return 'var(--blue)'
  return 'var(--text-3)'
}

function roleAccent(role: string) {
  if (role === 'user') return 'var(--accent)'
  if (role === 'assistant') return 'var(--blue)'
  return 'var(--yellow)'
}

function roleLabel(role: string, assistantLabel: string) {
  if (role === 'user') return 'You'
  if (role === 'assistant') return assistantLabel
  return 'System'
}

function diagnosticTail(value: string, maxChars = 6000) {
  if (value.length <= maxChars) return value
  return `[showing last ${maxChars} chars]\n${value.slice(value.length - maxChars)}`
}

function formatDiagnosticLine(entry: AcpBinding['diagnostics'][number]) {
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

function buildAcpErrorCopyText(acp: AcpBinding, title: string) {
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

function CopyTextButton({
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

function AcpErrorDetails({ acp, title }: { acp: AcpBinding; title: string }) {
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

function ProviderIcon({ providerId }: { providerId?: string }) {
  const codex = providerId === 'codex-cli'
  return (
    <span
      aria-hidden="true"
      className="inline-flex items-center justify-center"
      style={{
        width: 16,
        height: 16,
        borderRadius: 4,
        background: codex
          ? 'linear-gradient(135deg, #111827 0%, #3b82f6 52%, #22c55e 100%)'
          : 'linear-gradient(135deg, #8ab4ff 0%, #c58af9 48%, #4ade80 100%)',
        color: '#ffffff',
        fontSize: 10,
        lineHeight: 1,
      }}
    >
      {codex ? 'C' : '✦'}
    </span>
  )
}

function safeHref(url: string) {
  return /^(https?:|mailto:)/i.test(url) ? url : undefined
}

function renderInlineMarkdown(text: string, keyPrefix: string): ReactNode[] {
  const nodes: ReactNode[] = []
  const pattern = /(`[^`]+`|\*\*[^*]+?\*\*|\[[^\]]+\]\([^)]+\))/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = pattern.exec(text)) !== null) {
    if (match.index > lastIndex) {
      nodes.push(text.slice(lastIndex, match.index))
    }

    const token = match[0]
    const key = `${keyPrefix}-${match.index}`
    if (token.startsWith('`')) {
      nodes.push(<code key={key}>{token.slice(1, -1)}</code>)
    } else if (token.startsWith('**')) {
      nodes.push(<strong key={key}>{token.slice(2, -2)}</strong>)
    } else {
      const link = token.match(/^\[([^\]]+)\]\(([^)]+)\)$/)
      const href = link ? safeHref(link[2]) : undefined
      nodes.push(
        href ? (
          <a key={key} href={href} target="_blank" rel="noreferrer">
            {link?.[1]}
          </a>
        ) : (
          <span key={key}>{link?.[1] ?? token}</span>
        )
      )
    }

    lastIndex = match.index + token.length
  }

  if (lastIndex < text.length) {
    nodes.push(text.slice(lastIndex))
  }

  return nodes
}

function splitTableRow(line: string) {
  const trimmed = line.trim().replace(/^\|/, '').replace(/\|$/, '')
  const cells: string[] = []
  let cell = ''
  let escaped = false

  for (const char of trimmed) {
    if (escaped) {
      cell += char
      escaped = false
      continue
    }

    if (char === '\\') {
      escaped = true
      continue
    }

    if (char === '|') {
      cells.push(cell.trim())
      cell = ''
      continue
    }

    cell += char
  }

  if (escaped) {
    cell += '\\'
  }
  cells.push(cell.trim())
  return cells
}

function parseTableSeparator(line: string, expectedCells: number): Array<'left' | 'center' | 'right'> | null {
  const cells = splitTableRow(line)
  if (cells.length !== expectedCells) return null

  const alignments: Array<'left' | 'center' | 'right'> = []
  for (const cell of cells) {
    const normalized = cell.replace(/\s+/g, '')
    if (!/^:?-{3,}:?$/.test(normalized)) return null
    if (normalized.startsWith(':') && normalized.endsWith(':')) alignments.push('center')
    else if (normalized.endsWith(':')) alignments.push('right')
    else alignments.push('left')
  }

  return alignments
}

function isPotentialTableRow(line: string) {
  return line.includes('|') && splitTableRow(line).length >= 2
}

function MarkdownTextBlock({ text, blockKey }: { text: string; blockKey: string }) {
  const lines = text.replace(/\r\n/g, '\n').split('\n')
  const nodes: ReactNode[] = []
  let paragraph: string[] = []
  let index = 0

  const flushParagraph = () => {
    if (paragraph.length === 0) return
    const content = paragraph.join(' ')
    nodes.push(<p key={`${blockKey}-p-${nodes.length}`}>{renderInlineMarkdown(content, `${blockKey}-p-${nodes.length}`)}</p>)
    paragraph = []
  }

  while (index < lines.length) {
    const line = lines[index]
    const trimmed = line.trim()

    if (!trimmed) {
      flushParagraph()
      index++
      continue
    }

    if (index + 1 < lines.length && isPotentialTableRow(trimmed)) {
      const headerCells = splitTableRow(trimmed)
      const alignments = parseTableSeparator(lines[index + 1].trim(), headerCells.length)
      if (alignments) {
        flushParagraph()
        index += 2
        const bodyRows: string[][] = []
        while (index < lines.length && isPotentialTableRow(lines[index].trim())) {
          const rowCells = splitTableRow(lines[index].trim())
          if (rowCells.length !== headerCells.length) break
          bodyRows.push(rowCells)
          index++
        }

        nodes.push(
          <div key={`${blockKey}-table-${index}`} className="prose-msg-table-scroll">
            <table>
              <thead>
                <tr>
                  {headerCells.map((cell, cellIndex) => (
                    <th key={`${blockKey}-th-${index}-${cellIndex}`} style={{ textAlign: alignments[cellIndex] }}>
                      {renderInlineMarkdown(cell, `${blockKey}-th-${index}-${cellIndex}`)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {bodyRows.map((row, rowIndex) => (
                  <tr key={`${blockKey}-tr-${index}-${rowIndex}`}>
                    {row.map((cell, cellIndex) => (
                      <td
                        key={`${blockKey}-td-${index}-${rowIndex}-${cellIndex}`}
                        style={{ textAlign: alignments[cellIndex] }}
                      >
                        {renderInlineMarkdown(cell, `${blockKey}-td-${index}-${rowIndex}-${cellIndex}`)}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )
        continue
      }
    }

    const heading = trimmed.match(/^(#{1,3})\s+(.+)$/)
    if (heading) {
      flushParagraph()
      const level = heading[1].length
      const content = renderInlineMarkdown(heading[2], `${blockKey}-h-${index}`)
      if (level === 1) nodes.push(<h1 key={`${blockKey}-h-${index}`}>{content}</h1>)
      else if (level === 2) nodes.push(<h2 key={`${blockKey}-h-${index}`}>{content}</h2>)
      else nodes.push(<h3 key={`${blockKey}-h-${index}`}>{content}</h3>)
      index++
      continue
    }

    if (/^[-*]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^[-*]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^[-*]\s+/, ''))
        index++
      }
      nodes.push(
        <ul key={`${blockKey}-ul-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ul-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ul-${index}-${itemIndex}`)}
            </li>
          ))}
        </ul>
      )
      continue
    }

    if (/^\d+[.)]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^\d+[.)]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^\d+[.)]\s+/, ''))
        index++
      }
      nodes.push(
        <ol key={`${blockKey}-ol-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ol-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ol-${index}-${itemIndex}`)}
            </li>
          ))}
        </ol>
      )
      continue
    }

    if (/^>\s?/.test(trimmed)) {
      flushParagraph()
      const quoteLines: string[] = []
      while (index < lines.length && /^>\s?/.test(lines[index].trim())) {
        quoteLines.push(lines[index].trim().replace(/^>\s?/, ''))
        index++
      }
      nodes.push(
        <blockquote key={`${blockKey}-quote-${index}`}>
          {renderInlineMarkdown(quoteLines.join(' '), `${blockKey}-quote-${index}`)}
        </blockquote>
      )
      continue
    }

    paragraph.push(trimmed)
    index++
  }

  flushParagraph()
  return <>{nodes}</>
}

function MarkdownContent({ content }: { content: string }) {
  const parts: ReactNode[] = []
  const fencePattern = /```([A-Za-z0-9_-]+)?\n?([\s\S]*?)```/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = fencePattern.exec(content)) !== null) {
    if (match.index > lastIndex) {
      parts.push(
        <MarkdownTextBlock
          key={`text-${lastIndex}`}
          blockKey={`text-${lastIndex}`}
          text={content.slice(lastIndex, match.index)}
        />
      )
    }

    const language = match[1]?.trim()
    parts.push(
      <pre key={`code-${match.index}`}>
        {language && <div className="mb-2 text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>{language}</div>}
        <code>{match[2].replace(/\n$/, '')}</code>
      </pre>
    )
    lastIndex = match.index + match[0].length
  }

  if (lastIndex < content.length) {
    parts.push(
      <MarkdownTextBlock
        key={`text-${lastIndex}`}
        blockKey={`text-${lastIndex}`}
        text={content.slice(lastIndex)}
      />
    )
  }

  return <div className="prose-msg">{parts}</div>
}

function ToolCallInlineRows({ tools, onSelectTool }: { tools: AcpToolCall[]; onSelectTool: (toolId: string) => void }) {
  if (tools.length === 0) return null

  return (
    <div className="space-y-1">
      {tools.map((tool) => (
        <button
          key={tool.id}
          type="button"
          onClick={() => onSelectTool(tool.id)}
          className="w-full flex items-center gap-2 text-left"
          style={{
            border: '1px solid var(--border)',
            borderRadius: 6,
            background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
            color: 'var(--text-2)',
            padding: '6px 8px',
          }}
          title="Open tool details"
        >
          <span style={{ color: toolStatusColor(tool), fontSize: 9 }}>●</span>
          <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>Tool call:</span>
          <span className="text-xs truncate" style={{ color: 'var(--text)' }}>{tool.title || tool.id}</span>
          {tool.status && <span className="ml-auto text-[11px]" style={{ color: 'var(--text-3)' }}>{tool.status}</span>}
        </button>
      ))}
    </div>
  )
}

function PermissionInlineCard({
  permission,
  onResolve,
}: {
  permission: AcpPendingPermission
  onResolve: (requestId: string, optionId: string) => void
}) {
  return (
    <div
      className="my-2"
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
      <div className="flex flex-wrap gap-2">
        {permission.options.map((option) => (
          <button
            key={option.id}
            type="button"
            className="px-3 h-7 text-[11px] font-medium"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: option.kind.startsWith('allow') ? 'var(--accent-dim)' : 'var(--surface-up)',
              color: 'var(--text)',
            }}
            onClick={() => onResolve(permission.requestId, option.id)}
          >
            {option.name || option.id}
          </button>
        ))}
        <button
          type="button"
          className="px-3 h-7 text-[11px]"
          style={{
            borderRadius: 6,
            border: '1px solid var(--border)',
            background: 'transparent',
            color: 'var(--text-2)',
          }}
          onClick={() => onResolve(permission.requestId, 'cancelled')}
        >
          Cancel
        </button>
      </div>
    </div>
  )
}

function UserInputInlineCard({
  input,
  onResolve,
}: {
  input: AcpPendingUserInput
  onResolve: (requestId: string, answers: AcpUserInputAnswers) => void
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
      className="my-2"
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
                        className="px-3 py-1.5 text-[11px] text-left"
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
          className="px-3 h-7 text-[11px] font-medium"
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

function ToolCallModal({ tool, onClose }: { tool: AcpToolCall; onClose: () => void }) {
  const toolCopyText = [
    tool.title || tool.id || 'Tool call',
    `id: ${tool.id || 'unknown'}`,
    `kind: ${tool.kind || 'unknown'}`,
    `status: ${tool.status || 'unknown'}`,
    '',
    tool.content || 'No tool output yet.',
  ].join('\n')

  return (
    <div
      className="absolute inset-0 flex items-center justify-center"
      style={{
        zIndex: 1000,
        background: 'rgba(0, 0, 0, 0.48)',
        padding: 18,
      }}
      onMouseDown={onClose}
    >
      <section
        role="dialog"
        aria-modal="true"
        aria-label="Tool details"
        className="w-full"
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
        <div
          className="flex items-center gap-3 px-4"
          style={{
            minHeight: 44,
            borderBottom: '1px solid var(--border)',
          }}
        >
          <span style={{ color: toolStatusColor(tool), fontSize: 10 }}>●</span>
          <div className="min-w-0 flex-1">
            <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
              {tool.title || tool.id}
            </div>
            <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>
              {[tool.kind, tool.status].filter(Boolean).join(' / ') || 'tool call'}
            </div>
          </div>
          <CopyTextButton text={toolCopyText} label="Copy" title="Copy tool output" />
          <button
            type="button"
            title="Close tool details"
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
        </div>
        <div className="p-4 overflow-auto" style={{ maxHeight: 'calc(min(720px, 88vh) - 44px)' }}>
          <div className="grid gap-2 text-xs mb-4" style={{ color: 'var(--text-2)' }}>
            <div><span style={{ color: 'var(--text-3)' }}>id:</span> {tool.id || 'unknown'}</div>
            <div><span style={{ color: 'var(--text-3)' }}>kind:</span> {tool.kind || 'unknown'}</div>
            <div><span style={{ color: 'var(--text-3)' }}>status:</span> {tool.status || 'unknown'}</div>
          </div>
          <pre
            className="whitespace-pre-wrap text-xs"
            style={{
              border: '1px solid var(--border)',
              borderRadius: 6,
              background: 'var(--bg)',
              color: 'var(--text)',
              padding: 12,
              overflowWrap: 'anywhere',
              fontFamily: 'inherit',
            }}
          >
            {tool.content || 'No tool output yet.'}
          </pre>
        </div>
      </section>
    </div>
  )
}

function MessageFrame({
  role,
  children,
  assistantLabel,
  copyText = '',
}: {
  role: Message['role']
  children: ReactNode
  assistantLabel: string
  copyText?: string
}) {
  return (
    <div
      className="flex"
      style={{ justifyContent: role === 'user' ? 'flex-end' : 'flex-start' }}
    >
      <article
        className="min-w-0"
        style={{
          maxWidth: role === 'user' ? '78%' : '100%',
          border: role === 'user' ? '1px solid var(--border)' : '1px solid transparent',
          borderLeft: role !== 'user' ? `2px solid ${roleAccent(role)}` : undefined,
          borderRadius: role === 'user' ? 7 : 0,
          padding: role === 'user' ? '9px 11px' : '2px 0 2px 12px',
          background: role === 'user' ? 'color-mix(in srgb, var(--accent-dim) 55%, var(--surface))' : 'transparent',
          color: 'var(--text)',
        }}
      >
        <div className="flex items-center gap-1.5 text-[11px] mb-1" style={{ color: roleAccent(role) }}>
          <span style={{ fontSize: 8 }}>●</span>
          <span>{roleLabel(role, assistantLabel)}</span>
          {copyText.trim() && (
            <span className="ml-auto">
              <CopyTextButton text={copyText} label="Copy" title="Copy message" />
            </span>
          )}
        </div>
        {children}
      </article>
    </div>
  )
}

function ThinkingBlock({ text, defaultOpen = false }: { text: string; defaultOpen?: boolean }) {
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

function planStatusLabel(status: string) {
  if (status === 'inProgress') return 'in progress'
  if (status === 'completed') return 'completed'
  if (status === 'pending') return 'pending'
  return status || 'pending'
}

function planStatusColor(status: string) {
  if (status === 'completed') return 'var(--green)'
  if (status === 'inProgress') return 'var(--blue)'
  return 'var(--text-3)'
}

function PlanBlock({
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
          <button
            type="button"
            className="px-3 h-7 text-[11px] font-medium"
            disabled={actionsDisabled}
            title={actionsDisabled ? disabledTitle : 'Approve plan'}
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
          <button
            type="button"
            className="px-3 h-7 text-[11px] font-medium"
            disabled={actionsDisabled}
            title={actionsDisabled ? disabledTitle : 'Deny plan'}
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
        </div>
      )}
    </section>
  )
}

function PersistedMessageBlocksContent({
  message,
  blocks,
  onSelectTool,
  planActions,
}: {
  message: Message
  blocks: MessageBlock[]
  onSelectTool: (messageId: string, toolId: string) => void
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

function PersistedMessageContent({
  message,
  onSelectTool,
  planActions,
}: {
  message: Message
  onSelectTool: (messageId: string, toolId: string) => void
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

  if (blocks.length > 0) {
    return (
      <PersistedMessageBlocksContent
        message={message}
        blocks={blocks}
        onSelectTool={onSelectTool}
        planActions={planActions}
      />
    )
  }

  if (!thoughts && toolCalls.length === 0 && !planSummary.trim() && planEntries.length === 0) {
    return <MarkdownContent content={message.content} />
  }

  return (
    <div className="space-y-2">
      {message.content.trim() && <MarkdownContent content={message.content} />}
      <ThinkingBlock text={thoughts} />
      <ToolCallInlineRows tools={toolCalls} onSelectTool={(toolId) => onSelectTool(message.id, toolId)} />
      <PlanBlock
        summary={planSummary}
        entries={planEntries}
        showActions={planActions?.show}
        actionsDisabled={planActions?.disabled}
        disabledTitle={planActions?.disabledTitle}
        onApprove={planActions?.onApprove}
        onDeny={planActions?.onDeny}
      />
    </div>
  )
}

function TurnTimelineContent({
  events,
  tools,
  planSummary,
  planEntries,
  planActions,
  pendingPermission,
  pendingUserInput,
  onSelectTool,
  onResolvePermission,
  onResolveUserInput,
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
  onSelectTool: (toolId: string) => void
  onResolvePermission: (requestId: string, optionId: string) => void
  onResolveUserInput: (requestId: string, answers: AcpUserInputAnswers) => void
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
              <ToolCallInlineRows tools={[tool]} onSelectTool={onSelectTool} />
              {shouldRenderPendingPermission && (
                <PermissionInlineCard permission={pendingPermission} onResolve={onResolvePermission} />
              )}
              {shouldRenderPendingUserInput && (
                <UserInputInlineCard input={pendingUserInput} onResolve={onResolveUserInput} />
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
            />
          )
        }

        if (event.type === 'user_input_request' && pendingUserInput?.requestId === event.requestId) {
          return (
            <UserInputInlineCard
              key={`user-input-${event.requestId}-${index}`}
              input={pendingUserInput}
              onResolve={onResolveUserInput}
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
        <UserInputInlineCard input={pendingUserInput} onResolve={onResolveUserInput} />
      )}
    </div>
  )
}

function ComposerToolbar({
  acp,
  provider,
  providers,
  providerId,
  providerName,
  runtimeLabel,
  elapsedSeconds,
  canSend,
  modelId,
  approvalModeId,
  canChangeProvider,
  providerOpen,
  modelOpen,
  settingsOpen,
  providerMenuRef,
  modelMenuRef,
  settingsMenuRef,
  onToggleProvider,
  onToggleModel,
  onToggleSettings,
  onSelectProvider,
  onSelectModel,
  onTogglePlan,
  onCancel,
}: {
  acp?: AcpBinding
  provider: Provider
  providers: Provider[]
  providerId: string
  providerName: string
  runtimeLabel: string
  elapsedSeconds: number
  canSend: boolean
  modelId?: string
  approvalModeId?: string
  canChangeProvider: boolean
  providerOpen: boolean
  modelOpen: boolean
  settingsOpen: boolean
  providerMenuRef: RefObject<HTMLDivElement>
  modelMenuRef: RefObject<HTMLDivElement>
  settingsMenuRef: RefObject<HTMLDivElement>
  onToggleProvider: () => void
  onToggleModel: () => void
  onToggleSettings: () => void
  onSelectProvider: (providerId: string) => void
  onSelectModel: (modelId: string) => void
  onTogglePlan: () => void
  onCancel: () => void
}) {
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const providerOptions = providers.length > 0 ? providers : [provider]
  const modelDisabled = Boolean(
    acp?.processing ||
    acp?.lifecycleState === 'waitingPermission' ||
    acp?.lifecycleState === 'waitingUserInput'
  )
  const planActive = approvalModeId === 'plan'
  const hasRuntimeModes = Boolean(acp?.running && acp.availableModes.length > 0)
  const planAvailable = !hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'plan')
  const planDisabled = Boolean(modelDisabled || !planAvailable)
  const modeLabel = planActive ? 'Plan' : 'Default'
  const chipStyle = {
    height: 26,
    borderRadius: 6,
    border: '1px solid var(--border)',
    background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
    color: 'var(--text-2)',
  }

  return (
    <div
      className="flex items-center gap-2 flex-wrap px-2 py-2 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      <div ref={providerMenuRef} className="relative">
        <button
          type="button"
          title="Select provider"
          onClick={onToggleProvider}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: providerOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: providerOpen ? 'var(--border-bright)' : 'var(--border)',
          }}
        >
          <ProviderIcon providerId={providerId} />
          <span>{providerName}</span>
        </button>
        {providerOpen && (
          <div
            className="absolute left-0"
            style={{
              bottom: 32,
              width: 230,
              zIndex: 40,
              border: '1px solid var(--border-bright)',
              borderRadius: 8,
              background: 'var(--surface)',
              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
              padding: 6,
            }}
          >
            <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Provider</div>
            {providerOptions.map((candidate) => {
              const candidateName = providerDisplayName(candidate, candidate.id)
              const selected = candidate.id === providerId
              const disabled = !selected && !canChangeProvider
              return (
                <button
                  key={candidate.id}
                  type="button"
                  onClick={() => {
                    if (disabled) return
                    onSelectProvider(candidate.id)
                  }}
                  disabled={disabled}
                  className="w-full flex items-center gap-2 text-left px-2 py-2"
                  style={{
                    borderRadius: 6,
                    background: selected ? 'var(--accent-dim)' : 'transparent',
                    color: selected ? 'var(--text)' : 'var(--text-2)',
                    opacity: disabled ? 0.5 : 1,
                  }}
                >
                  <ProviderIcon providerId={candidate.id} />
                  <span className="flex-1">{candidateName}</span>
                  {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                </button>
              )
            })}
          </div>
        )}
      </div>
      <div ref={modelMenuRef} className="relative">
        <button
          type="button"
          title="Select model"
          onClick={onToggleModel}
          disabled={modelDisabled}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: modelOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: modelOpen ? 'var(--border-bright)' : 'var(--border)',
            opacity: modelDisabled ? 0.55 : 1,
          }}
        >
          <span>Model</span>
          <span style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
        </button>
        {modelOpen && !modelDisabled && (
          <div
            className="absolute left-0"
            style={{
              bottom: 32,
              width: 260,
              zIndex: 40,
              border: '1px solid var(--border-bright)',
              borderRadius: 8,
              background: 'var(--surface)',
              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
              padding: 6,
            }}
          >
            <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Model</div>
            {modelOptions.map((option) => {
              const selected = option.id === currentModel.id
              return (
                <button
                  key={option.id || 'default'}
                  type="button"
                  onClick={() => onSelectModel(option.id)}
                  className="w-full grid gap-0.5 text-left px-2 py-2"
                  style={{
                    borderRadius: 6,
                    background: selected ? 'var(--accent-dim)' : 'transparent',
                    color: selected ? 'var(--text)' : 'var(--text-2)',
                  }}
                >
                  <span className="flex items-center gap-2">
                    <span className="flex-1">{option.label}</span>
                    {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                  </span>
                  <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>
                </button>
              )
            })}
          </div>
        )}
      </div>
      <button
        type="button"
        title={planAvailable ? 'Toggle planning mode' : 'Planning mode unavailable'}
        aria-pressed={planActive}
        onClick={onTogglePlan}
        disabled={planDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: planActive ? 'color-mix(in srgb, var(--accent) 55%, var(--border))' : 'var(--border)',
          background: planActive ? 'var(--accent-dim)' : chipStyle.background,
          color: planActive ? 'var(--text)' : 'var(--text-2)',
          opacity: planDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: planActive ? 'var(--accent)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>Plan</span>
      </button>
      <button type="button" title="Runtime" className="inline-flex items-center gap-1.5 px-2" style={chipStyle}>
        <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>
        <span>{runtimeLabel}</span>
      </button>
      <button type="button" title="Usage" className="inline-flex items-center gap-1.5 px-2" style={chipStyle}>
        <span>Usage</span>
        <span style={{ color: 'var(--text-3)' }}>--</span>
      </button>
      {acp?.processing && (
        <span
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: 'var(--text)',
            borderColor: 'color-mix(in srgb, var(--blue) 40%, var(--border))',
          }}
        >
          <span style={{ color: 'var(--blue)', fontSize: 10 }}>●</span>
          <span>Working {elapsedSeconds}s</span>
        </span>
      )}
      <div className="ml-auto flex items-center gap-2">
        <div ref={settingsMenuRef} className="relative">
          <button
            type="button"
            title="Settings"
            onClick={onToggleSettings}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: settingsOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: settingsOpen ? 'var(--border-bright)' : 'var(--border)',
            }}
          >
            <span>⚙</span>
          </button>
          {settingsOpen && (
            <div
              className="absolute right-0"
              style={{
                bottom: 32,
                width: 250,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 10,
              }}
            >
              <div className="text-xs font-semibold mb-2" style={{ color: 'var(--text)' }}>Chat settings</div>
              <div className="grid gap-2 text-[11px]" style={{ color: 'var(--text-2)' }}>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Provider</span>
                  <span>{providerName}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Runtime</span>
                  <span>{runtimeLabel}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Model</span>
                  <span>{currentModel.label}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Mode</span>
                  <span>{modeLabel}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Usage</span>
                  <span>Unavailable</span>
                </div>
              </div>
            </div>
          )}
        </div>
        {acp?.processing ? (
          <button
            type="button"
            title="Cancel turn"
            onClick={onCancel}
            className="h-[26px] px-3 text-xs font-semibold inline-flex items-center"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: 'var(--surface-up)',
              color: 'var(--text)',
            }}
          >
            Stop
          </button>
        ) : (
          <button
            type="submit"
            title="Send prompt"
            disabled={!canSend}
            className="h-[26px] px-3 text-xs font-semibold inline-flex items-center"
            style={{
              borderRadius: 6,
              border: '1px solid var(--border-bright)',
              background: canSend ? 'var(--accent)' : 'var(--surface-up)',
              color: canSend ? '#fff' : 'var(--text-3)',
            }}
          >
            Send
          </button>
        )}
      </div>
    </div>
  )
}

export function ChatView({ session }: ChatViewProps) {
  const [draft, setDraft] = useState('')
  const [submitting, setSubmitting] = useState(false)
  const [selectedToolCallRef, setSelectedToolCallRef] = useState<SelectedToolCallRef | null>(null)
  const [providerOpen, setProviderOpen] = useState(false)
  const [modelOpen, setModelOpen] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [elapsedSeconds, setElapsedSeconds] = useState(0)
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const providers = useAppStore((s) => s.providers)
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const resolveAcpUserInput = useAppStore((s) => s.resolveAcpUserInput)
  const setSessionProvider = useAppStore((s) => s.setSessionProvider)
  const setSessionModel = useAppStore((s) => s.setSessionModel)
  const setSessionApprovalMode = useAppStore((s) => s.setSessionApprovalMode)
  const bottomRef = useRef<HTMLDivElement>(null)
  const providerMenuRef = useRef<HTMLDivElement>(null)
  const modelMenuRef = useRef<HTMLDivElement>(null)
  const settingsMenuRef = useRef<HTMLDivElement>(null)

  const canSend = useMemo(
    () => draft.trim().length > 0 && !submitting && !acp?.processing,
    [draft, submitting, acp?.processing]
  )

  const selectedToolCall = useMemo(
    () => {
      if (!selectedToolCallRef) return null

      if (selectedToolCallRef.messageId) {
        const message = messages.find((candidate) => candidate.id === selectedToolCallRef.messageId)
        return message?.toolCalls?.find((tool) => tool.id === selectedToolCallRef.id) ?? null
      }

      return (acp?.toolCalls ?? []).find((tool) => tool.id === selectedToolCallRef.id) ?? null
    },
    [acp?.toolCalls, messages, selectedToolCallRef]
  )

  const turnEvents = acp?.turnEvents ?? []
  const firstTurnEvent = turnEvents.find((event) => event.type === 'assistant_text' ? event.text.length > 0 : true)
  const turnAssistantMessageIndex = acp?.turnAssistantMessageIndex ?? -1
  const turnUserMessageIndex = acp?.turnUserMessageIndex ?? -1
  const turnSerial = acp?.turnSerial ?? 0
  const processingStartedAtMs = acp?.processing ? acp.processingStartedAtMs : null
  const renderTimelineAfterUser =
    turnEvents.length > 0 &&
    turnUserMessageIndex >= 0 &&
    turnUserMessageIndex < messages.length &&
    (turnAssistantMessageIndex < 0 || turnAssistantMessageIndex >= messages.length || firstTurnEvent?.type !== 'assistant_text')
  const renderTimelineAtAssistant =
    turnEvents.length > 0 &&
    !renderTimelineAfterUser &&
    turnAssistantMessageIndex >= 0 &&
    turnAssistantMessageIndex < messages.length

  useEffect(() => {
    bottomRef.current?.scrollIntoView?.({ block: 'end' })
  }, [
    messages.length,
    messages[messages.length - 1]?.content,
    messages[messages.length - 1]?.planSummary,
    messages[messages.length - 1]?.planEntries?.length,
    acp?.toolCalls.length,
    acp?.planSummary,
    acp?.planEntries.length,
    turnEvents.length,
    turnEvents[turnEvents.length - 1]?.type,
    turnEvents[turnEvents.length - 1]?.text,
    turnSerial,
    acp?.lastError,
  ])

  useEffect(() => {
    if (selectedToolCallRef && !selectedToolCall) {
      setSelectedToolCallRef(null)
    }
  }, [selectedToolCall, selectedToolCallRef])

  useEffect(() => {
    setSelectedToolCallRef(null)
  }, [turnSerial])

  useEffect(() => {
    if (acp?.processing || acp?.lifecycleState === 'waitingPermission' || acp?.lifecycleState === 'waitingUserInput') {
      setModelOpen(false)
    }
  }, [acp?.lifecycleState, acp?.processing])

  useEffect(() => {
    const onMouseDown = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return

      if (providerOpen && providerMenuRef.current && !providerMenuRef.current.contains(target)) {
        setProviderOpen(false)
      }

      if (modelOpen && modelMenuRef.current && !modelMenuRef.current.contains(target)) {
        setModelOpen(false)
      }

      if (settingsOpen && settingsMenuRef.current && !settingsMenuRef.current.contains(target)) {
        setSettingsOpen(false)
      }
    }

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setProviderOpen(false)
      setModelOpen(false)
      setSettingsOpen(false)
      setSelectedToolCallRef(null)
    }

    document.addEventListener('mousedown', onMouseDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onMouseDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [modelOpen, providerOpen, settingsOpen])

  useEffect(() => {
    if (processingStartedAtMs === null) {
      setElapsedSeconds(0)
      return undefined
    }

    const updateElapsed = () => {
      setElapsedSeconds(Math.max(0, Math.floor((Date.now() - processingStartedAtMs) / 1000)))
    }
    updateElapsed()
    const interval = window.setInterval(updateElapsed, 1000)
    return () => window.clearInterval(interval)
  }, [processingStartedAtMs])

  const submit = async (event?: FormEvent) => {
    event?.preventDefault()
    if (!canSend) return
    const prompt = draft.trim()
    setSubmitting(true)
    const ok = await sendAcpPrompt(session.id, prompt)
    setSubmitting(false)
    if (ok) setDraft('')
  }

  const onComposerKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      void submit()
    }
  }

  const pendingPermission = acp?.pendingPermission
  const pendingUserInput = acp?.pendingUserInput
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
  const currentProviderId = session.providerId || acp?.providerId || 'gemini-cli'
  const currentProvider = useMemo<Provider>(
    () =>
      providers.find((candidate) => candidate.id === currentProviderId) ?? {
        id: currentProviderId,
        name: currentProviderId === 'codex-cli' ? 'Codex CLI' : 'Gemini CLI',
        shortName: currentProviderId === 'codex-cli' ? 'Codex' : 'Gemini',
        color: '#8ab4ff',
        description: '',
        outputMode: 'cli',
        supportsCli: true,
        supportsStructured: true,
        structuredProtocol: currentProviderId === 'codex-cli' ? 'codex-app-server' : 'gemini-acp',
      },
    [currentProviderId, providers]
  )
  const currentProviderName = providerDisplayName(currentProvider, currentProviderId)
  const currentRuntimeLabel = providerRuntimeLabel(currentProvider, acp)
  const currentErrorTitle = `${currentProviderName} ${currentRuntimeLabel} error`
  const canChangeProvider = messages.length === 0 && !acp?.running && !acp?.processing
  const currentModelId = acp?.currentModelId || session.modelId || ''
  const currentModeId = acp?.currentModeId || session.approvalMode || 'default'
  const latestPlanMessageIndex = messages.reduce((latest, message, index) => {
    const hasPlan = message.role === 'assistant' && (Boolean(message.planSummary?.trim()) || (message.planEntries?.length ?? 0) > 0)
    return hasPlan ? index : latest
  }, -1)
  const latestPlanHasLaterUser =
    latestPlanMessageIndex >= 0 && messages.slice(latestPlanMessageIndex + 1).some((message) => message.role === 'user')
  const canShowPlanActions = isCodexProvider(currentProvider, currentProviderId) && latestPlanMessageIndex >= 0 && !latestPlanHasLaterUser
  const planActionBlockedByRuntime =
    acp?.processing ||
    acp?.lifecycleState === 'waitingPermission' ||
    acp?.lifecycleState === 'waitingUserInput'
  const planActionsDisabled = Boolean(submitting || planActionBlockedByRuntime)
  const planActionsDisabledTitle = planActionBlockedByRuntime
    ? 'Codex is still working.'
    : 'Plan action is unavailable.'
  const sendPlanAction = async (prompt: string, nextModeId: 'default' | 'plan') => {
    if (planActionsDisabled) return
    setSubmitting(true)
    const modeOk = await setSessionApprovalMode(session.id, nextModeId)
    if (modeOk) {
      await sendAcpPrompt(session.id, prompt)
    }
    setSubmitting(false)
  }
  const activePlanActions = canShowPlanActions
    ? {
        show: true,
        disabled: planActionsDisabled,
        disabledTitle: planActionsDisabledTitle,
        onApprove: () => void sendPlanAction(PLAN_APPROVE_PROMPT, 'default'),
        onDeny: () => void sendPlanAction(PLAN_DENY_PROMPT, 'plan'),
      }
    : undefined
  const planActionsForMessage = (index: number) =>
    canShowPlanActions && index === latestPlanMessageIndex
      ? activePlanActions
      : undefined

  return (
    <div className="relative h-full flex overflow-hidden" style={{ background: 'var(--bg)' }}>
      {selectedToolCall && (
        <ToolCallModal tool={selectedToolCall} onClose={() => setSelectedToolCallRef(null)} />
      )}
      <div className="flex-1 flex flex-col min-w-0">
        <div className="flex-1 overflow-auto" data-copy-surface="chat">
          <div className="mx-auto w-full px-4 py-5" style={{ maxWidth: 940 }}>
            <div className="flex items-center gap-2 mb-5 text-xs" style={{ color: 'var(--text-2)' }}>
              <span style={{ color: statusColor(acp), fontSize: 9 }}>●</span>
              <span>{statusLabel(acp)}</span>
              {acp?.agentInfo?.title && (
                <span style={{ color: 'var(--text-3)' }}>{acp.agentInfo.title}</span>
              )}
              {acp?.sessionId && (
                <span className="truncate" style={{ color: 'var(--text-3)' }}>
                  {acp.sessionId}
                </span>
              )}
            </div>

            <div className="space-y-4">
              {messages.map((message, index) => {
                const shouldRenderTimelineAtAssistant = renderTimelineAtAssistant && index === turnAssistantMessageIndex
                const shouldSkipAssistantMessage = renderTimelineAfterUser && index === turnAssistantMessageIndex

                if (shouldSkipAssistantMessage) return null

                return (
                  <div key={message.id} className="space-y-2">
                    <MessageFrame role={message.role} assistantLabel={currentProviderName} copyText={message.content}>
                      {shouldRenderTimelineAtAssistant ? (
                        <TurnTimelineContent
                          key={`turn-${turnSerial}-assistant`}
                          events={turnEvents}
	                          tools={acp?.toolCalls ?? []}
	                          planSummary={acp?.planSummary ?? ''}
	                          planEntries={acp?.planEntries ?? []}
	                          planActions={activePlanActions}
	                          pendingPermission={pendingPermission ?? null}
	                          pendingUserInput={pendingUserInput ?? null}
	                          onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                          onResolvePermission={(requestId, optionId) => {
	                            void resolveAcpPermission(session.id, requestId, optionId)
	                          }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
                        />
                      ) : (
                        <PersistedMessageContent
                          message={message}
                          onSelectTool={(messageId, toolId) => setSelectedToolCallRef({ id: toolId, messageId })}
                          planActions={planActionsForMessage(index)}
                        />
                      )}
                    </MessageFrame>
                    {renderTimelineAfterUser && index === turnUserMessageIndex && (
                      <MessageFrame key={`turn-${turnSerial}-after-user`} role="assistant" assistantLabel={currentProviderName}>
                        <TurnTimelineContent
                          key={`turn-${turnSerial}-after-user-content`}
                          events={turnEvents}
	                          tools={acp?.toolCalls ?? []}
	                          planSummary={acp?.planSummary ?? ''}
	                          planEntries={acp?.planEntries ?? []}
	                          planActions={activePlanActions}
	                          pendingPermission={pendingPermission ?? null}
	                          pendingUserInput={pendingUserInput ?? null}
	                          onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                          onResolvePermission={(requestId, optionId) => {
	                            void resolveAcpPermission(session.id, requestId, optionId)
	                          }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
                        />
                      </MessageFrame>
                    )}
                  </div>
                )
              })}
              {turnEvents.length > 0 && !renderTimelineAfterUser && !renderTimelineAtAssistant && (
                <MessageFrame key={`turn-${turnSerial}-fallback`} role="assistant" assistantLabel={currentProviderName}>
                  <TurnTimelineContent
                    key={`turn-${turnSerial}-fallback-content`}
                    events={turnEvents}
	                    tools={acp?.toolCalls ?? []}
	                    planSummary={acp?.planSummary ?? ''}
	                    planEntries={acp?.planEntries ?? []}
	                    planActions={activePlanActions}
	                    pendingPermission={pendingPermission ?? null}
	                    pendingUserInput={pendingUserInput ?? null}
	                    onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                    onResolvePermission={(requestId, optionId) => {
	                      void resolveAcpPermission(session.id, requestId, optionId)
	                    }}
                      onResolveUserInput={(requestId, answers) => {
                        void resolveAcpUserInput(session.id, requestId, answers)
                      }}
                  />
                </MessageFrame>
              )}
              <div ref={bottomRef} />
            </div>
          </div>
        </div>

        <form
          onSubmit={submit}
          className="flex-shrink-0"
          style={{
            borderTop: '1px solid var(--border)',
            background: 'var(--surface)',
          }}
        >
          <div className="mx-auto p-3" style={{ maxWidth: 940 }}>
            <div
              className="mb-2 flex items-center gap-2 text-[11px]"
              style={{ color: 'var(--text-3)', minWidth: 0 }}
              title={workspaceDirectory || 'No workspace directory selected'}
            >
              <span style={{ color: 'var(--text-2)', flexShrink: 0 }}>Workspace</span>
              <span
                className="truncate"
                style={{
                  border: '1px solid var(--border)',
                  borderRadius: 6,
                  background: 'var(--bg)',
                  color: workspaceDirectory ? 'var(--text-2)' : 'var(--text-3)',
                  padding: '3px 7px',
                  minWidth: 0,
                }}
              >
                {workspaceDirectory || 'No workspace directory selected'}
              </span>
            </div>
            {acp?.lastError && (
              <div
                className="mb-2 text-xs"
                style={{
                  border: '1px solid color-mix(in srgb, var(--red) 45%, var(--border))',
                  borderRadius: 6,
                  padding: '8px 10px',
                  background: 'color-mix(in srgb, var(--red) 10%, var(--surface))',
                  color: 'var(--text)',
                  overflowWrap: 'anywhere',
                }}
              >
                <div className="flex items-start gap-2">
                  <div className="min-w-0 flex-1">
                    <span style={{ color: 'var(--red)', fontWeight: 600 }}>{currentErrorTitle}</span>
                    <span style={{ color: 'var(--text-2)' }}> · </span>
                    {acp.lastError}
                  </div>
                  <CopyTextButton text={buildAcpErrorCopyText(acp, currentErrorTitle)} label="Copy error" title="Copy error details" />
                </div>
                <AcpErrorDetails acp={acp} title={currentErrorTitle} />
		              </div>
	            )}
            <div
              style={{
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--bg)',
                overflow: 'visible',
              }}
            >
            <textarea
              value={draft}
              onChange={(event) => setDraft(event.target.value)}
              onKeyDown={onComposerKeyDown}
              rows={3}
              placeholder={`Message ${currentProviderName}`}
              disabled={submitting}
              className="w-full resize-none text-sm"
              style={{
                minHeight: 72,
                maxHeight: 160,
                border: 'none',
                background: 'transparent',
                color: 'var(--text)',
                padding: '10px 12px',
                outline: 'none',
              }}
            />
            <ComposerToolbar
              acp={acp}
              provider={currentProvider}
              providers={providers}
              providerId={currentProviderId}
              providerName={currentProviderName}
              runtimeLabel={currentRuntimeLabel}
              elapsedSeconds={elapsedSeconds}
              canSend={canSend}
              modelId={currentModelId}
              approvalModeId={currentModeId}
              canChangeProvider={canChangeProvider}
              providerOpen={providerOpen}
              modelOpen={modelOpen}
              settingsOpen={settingsOpen}
              providerMenuRef={providerMenuRef}
              modelMenuRef={modelMenuRef}
              settingsMenuRef={settingsMenuRef}
              onToggleProvider={() => {
                setProviderOpen((value) => !value)
                setModelOpen(false)
                setSettingsOpen(false)
              }}
              onToggleModel={() => {
                if (acp?.processing || acp?.lifecycleState === 'waitingPermission' || acp?.lifecycleState === 'waitingUserInput') return
                setModelOpen((value) => !value)
                setProviderOpen(false)
                setSettingsOpen(false)
              }}
              onToggleSettings={() => {
                setSettingsOpen((value) => !value)
                setProviderOpen(false)
                setModelOpen(false)
              }}
              onSelectProvider={(providerId) => {
                setProviderOpen(false)
                if (providerId === currentProviderId || !canChangeProvider) return
                void setSessionProvider(session.id, providerId)
              }}
              onSelectModel={(modelId) => {
                setModelOpen(false)
                void setSessionModel(session.id, modelId)
              }}
              onTogglePlan={() => {
                const nextMode = currentModeId === 'plan' ? 'default' : 'plan'
                void setSessionApprovalMode(session.id, nextMode)
              }}
              onCancel={() => void cancelAcpTurn(session.id)}
            />
            </div>
          </div>
        </form>
      </div>
    </div>
  )
}
