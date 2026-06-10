import { ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, memo, ReactNode, RefObject, useEffect, useMemo, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Session } from '../../types/session'
import {
  useAppStore,
  type AcpBinding,
  type AcpModel,
  type AcpPendingPermission,
  type AcpPermissionOption,
  type AcpPendingUserInput,
  type AcpPlanEntry,
  type AcpToolCall,
  type AcpTurnEvent,
  type AcpUserInputAnswers,
  type ChatAttachmentInput,
} from '../../store/useAppStore'
import type { Attachment, Message, MessageBlock } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { Goal, GoalStatus } from '../../types/goal'
import { GoalBanner } from '../shared/GoalBanner'
import { copyTextToClipboard } from '../../utils/copySelection'
import {
  CODEX_CLI_PROVIDER_ID,
  CLAUDE_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  fallbackProviderForId,
  providerRuntimeKindLabel,
  providerShortName,
  providerUsesProtocol,
} from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'

interface ChatViewProps {
  session: Session
}

const INITIAL_RENDERED_MESSAGES = 200
const RENDERED_MESSAGE_BATCH_SIZE = 100

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

const CODEX_REASONING_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'Default', detail: 'Use Codex default reasoning' },
  none: { label: 'None', shortLabel: 'None', detail: 'No extra reasoning' },
  minimal: { label: 'Minimal', shortLabel: 'Minimal', detail: 'Fastest reasoning' },
  low: { label: 'Low', shortLabel: 'Low', detail: 'Faster responses' },
  medium: { label: 'Medium', shortLabel: 'Medium', detail: 'Balanced reasoning' },
  high: { label: 'High', shortLabel: 'High', detail: 'Deeper reasoning' },
  xhigh: { label: 'XHigh', shortLabel: 'XHigh', detail: 'Maximum reasoning' },
}

const CODEX_SPEED_LABELS: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>> = {
  '': { label: 'CLI default', shortLabel: 'Default', detail: 'Use Codex default speed' },
  fast: { label: 'Fast', shortLabel: 'Fast', detail: 'Prioritize latency' },
  flex: { label: 'Flex', shortLabel: 'Flex', detail: 'Use flexible service tier' },
}

const PLAN_APPROVE_PROMPT = 'Proceed with the plan.'
const PLAN_DENY_PROMPT = 'Do not proceed with this plan. Please revise it before making changes.'

type LocalAttachmentStatus = 'ready' | 'staging' | 'failed'
type ComposerIconName = 'editor' | 'folder' | 'git-tree' | 'markdown' | 'plus' | 'send' | 'terminal'

interface LocalAttachment extends Attachment {
  status: LocalAttachmentStatus
  error?: string
}

function acpRuntimeBlocksControlChanges(acp?: AcpBinding | null): boolean {
  return Boolean(
    acp?.processing ||
    acp?.lifecycleState === 'waitingPermission' ||
    acp?.lifecycleState === 'waitingUserInput'
  )
}

function ComposerIcon({ name, size = 14 }: { name: ComposerIconName; size?: number }) {
  if (name === 'markdown') {
    return (
      <span
        aria-hidden="true"
        className="font-semibold leading-none"
        style={{ fontSize: 11, letterSpacing: 0 }}
      >
        .md
      </span>
    )
  }

  if (name === 'folder') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M2.5 4.2h4l1.1 1.4h5.9v6.2a1 1 0 0 1-1 1h-10a1 1 0 0 1-1-1V5.2a1 1 0 0 1 1-1Z" />
      </svg>
    )
  }

  if (name === 'editor') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <rect x="2.5" y="3" width="11" height="8.5" rx="1.2" />
        <path d="M5.5 13h5" />
        <path d="M8 11.5V13" />
        <path d="m5.6 6.1 1.2 1.2-1.2 1.2" />
        <path d="M8.2 8.6h2.2" />
      </svg>
    )
  }

  if (name === 'git-tree') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <circle cx="4" cy="3.5" r="1.6" />
        <circle cx="12" cy="8" r="1.6" />
        <circle cx="4" cy="12.5" r="1.6" />
        <path d="M4 5.1v5.8" />
        <path d="M5.6 3.5h1.7A2.7 2.7 0 0 1 10 6.2V8h.4" />
      </svg>
    )
  }

  if (name === 'send') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M2.5 8 13 3.2 10.4 13 7.5 9.2 2.5 8Z" />
        <path d="m7.5 9.2 2.2-2.4" />
      </svg>
    )
  }

  if (name === 'terminal') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <rect x="2" y="3" width="12" height="10" rx="1.5" />
        <path d="m4.7 7.3 2 1.1-2 1.1" />
        <path d="M8 10h4" />
      </svg>
    )
  }

  return (
    <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" aria-hidden="true">
      <path d="M8 3.2v9.6" />
      <path d="M3.2 8h9.6" />
    </svg>
  )
}

function filePathFromBrowserFile(file: File): string {
  const maybeFile = file as File & { path?: string }
  return typeof maybeFile.path === 'string' ? maybeFile.path : ''
}

function readFileBase64(file: File): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onload = () => {
      const result = typeof reader.result === 'string' ? reader.result : ''
      const comma = result.indexOf(',')
      resolve(comma >= 0 ? result.slice(comma + 1) : result)
    }
    reader.onerror = () => reject(reader.error ?? new Error('Failed to read file.'))
    reader.readAsDataURL(file)
  })
}

function attachmentLabel(attachment: Attachment) {
  const path = attachment.path?.trim()
  return path || attachment.name
}

function fileUriToPath(uri: string): string {
  if (!uri.startsWith('file://')) return uri
  try {
    return decodeURIComponent(new URL(uri).pathname)
  } catch {
    return uri.replace(/^file:\/\//, '')
  }
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
  return providerRuntimeKindLabel(provider, acp?.protocolKind)
}

function isCodexProvider(provider?: Provider, providerId = '') {
  return providerUsesProtocol(provider, providerId, CODEX_CLI_PROVIDER_ID)
}

function isClaudeProvider(provider?: Provider, providerId = '') {
  return providerUsesProtocol(provider, providerId, CLAUDE_CLI_PROVIDER_ID)
}

function isCopilotProvider(provider?: Provider, providerId = '') {
  return providerUsesProtocol(provider, providerId, COPILOT_CLI_PROVIDER_ID)
}

function isOpenCodeProvider(provider?: Provider, providerId = '') {
  return providerUsesProtocol(provider, providerId, OPENCODE_CLI_PROVIDER_ID)
}

function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
}

function modelOptionFromRuntime(model: AcpModel, useFriendlyLabels: boolean): ModelOption | null {
  const id = model.id.trim()
  if (!id) return null
  if (useFriendlyLabels) {
    const friendly = FRIENDLY_MODEL_LABELS[id]
    if (friendly) {
      return { id, ...friendly, detail: model.description || friendly.detail }
    }
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
  const providerName = providerShortName(provider, providerId)
  const codexProvider = isCodexProvider(provider, providerId)
  const claudeProvider = isClaudeProvider(provider, providerId)
  const copilotProvider = isCopilotProvider(provider, providerId)
  const openCodeProvider = isOpenCodeProvider(provider, providerId)
  const runtimeOptions = (acp?.availableModels ?? []).flatMap((model) => {
    const option = modelOptionFromRuntime(model, !codexProvider && !claudeProvider && !copilotProvider && !openCodeProvider)
    return option ? [option] : []
  })
  const defaultOption = providerDefaultModelOption(providerName)
  const fallbackOptions = codexProvider
    ? [defaultOption]
    : claudeProvider
      ? [defaultOption, { id: 'sonnet', label: 'Sonnet', shortLabel: 'Sonnet', detail: 'Latest Sonnet alias' }, { id: 'opus', label: 'Opus', shortLabel: 'Opus', detail: 'Latest Opus alias' }]
      : copilotProvider
        ? [defaultOption]
        : openCodeProvider
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
    const friendly = codexProvider || openCodeProvider ? undefined : FRIENDLY_MODEL_LABELS[selectedModelId]
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

function codexSelectedRuntimeModel(acp: AcpBinding | undefined, modelId: string): AcpModel | undefined {
  const models = acp?.availableModels ?? []
  return models.find((model) => model.id === modelId) ?? models.find((model) => Boolean(model.defaultReasoningEffort)) ?? models[0]
}

function labeledOption(id: string, labels: Record<string, Pick<ModelOption, 'label' | 'shortLabel' | 'detail'>>): ModelOption {
  const fallback = labels[id] ?? {
    label: titleFromModelId(id),
    shortLabel: titleFromModelId(id),
    detail: id,
  }
  return { id, ...fallback }
}

function buildCodexReasoningOptions(acp: AcpBinding | undefined, modelId: string, selectedReasoningEffort = ''): ModelOption[] {
  const runtimeModel = codexSelectedRuntimeModel(acp, modelId)
  const runtimeEfforts = runtimeModel?.supportedReasoningEfforts ?? []
  const base = runtimeEfforts.length > 0 ? runtimeEfforts : ['none', 'minimal', 'low', 'medium', 'high', 'xhigh']
  const ids = ['', ...base]
  if (selectedReasoningEffort && !ids.includes(selectedReasoningEffort)) ids.push(selectedReasoningEffort)
  return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_REASONING_LABELS))
}

function buildCodexSpeedOptions(acp: AcpBinding | undefined, modelId: string, selectedServiceTier = ''): ModelOption[] {
  const runtimeModel = codexSelectedRuntimeModel(acp, modelId)
  const runtimeTiers = runtimeModel?.additionalSpeedTiers ?? []
  const ids = ['', ...new Set([...runtimeTiers, 'fast', 'flex'])]
  if (selectedServiceTier && !ids.includes(selectedServiceTier)) ids.push(selectedServiceTier)
  return Array.from(new Set(ids)).map((id) => labeledOption(id, CODEX_SPEED_LABELS))
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
  if (tool.status === 'running') return 'var(--blue)'
  return 'var(--text-3)'
}

function toolDisplayKind(tool: AcpToolCall) {
  return tool.isSubAgent ? 'Sub-agent' : 'Tool call'
}

function toolDisplayTitle(tool: AcpToolCall) {
  return tool.isSubAgent
    ? (tool.subAgentTitle || tool.title || tool.subAgentId || tool.id)
    : (tool.title || tool.id)
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

const MarkdownContent = memo(function MarkdownContent({ content }: { content: string }) {
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
})

function SubAgentRunningPanel({ tool, onSelectTool }: { tool: AcpToolCall; onSelectTool: (toolId: string) => void }) {
  const isActive = tool.status === 'running' || tool.status === 'in_progress' || tool.status === 'pending'
  const statusColor = toolStatusColor(tool)
  const displayTitle = toolDisplayTitle(tool)
  return (
    <button
      type="button"
      onClick={() => onSelectTool(tool.id)}
      className="w-full text-left uam-subagent-panel"
      style={{
        display: 'grid',
        gridTemplateColumns: 'minmax(0, 1fr) auto',
        gap: 8,
        alignItems: 'center',
        padding: '10px 12px',
        borderRadius: 8,
        border: '1px solid color-mix(in srgb, var(--blue) 35%, var(--border-bright))',
        borderLeft: '3px solid var(--blue)',
        background: 'color-mix(in srgb, var(--blue) 8%, var(--surface))',
        color: 'var(--text)',
        boxShadow: '0 1px 0 color-mix(in srgb, var(--blue) 10%, transparent)',
      }}
      title="Open sub-agent details"
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
          <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
            <circle cx="8" cy="4.4" r="2.1" />
            <path d="M4.4 13.2a3.6 3.6 0 0 1 7.2 0" />
            <path d="M2.5 8.6h2.2M11.3 8.6h2.2" />
          </svg>
        </span>
        <span className="min-w-0" style={{ display: 'flex', flexDirection: 'column', minWidth: 0, gap: 2 }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 10, fontWeight: 700, letterSpacing: '0.04em', textTransform: 'uppercase', color: 'var(--blue)' }}>
            <span aria-hidden="true" style={{ display: 'inline-block', width: 6, height: 6, borderRadius: 999, background: isActive ? statusColor : 'var(--text-3)', boxShadow: isActive ? `0 0 0 3px color-mix(in srgb, ${statusColor} 24%, transparent)` : 'none', animation: isActive ? 'uam-pulse 1.4s ease-in-out infinite' : 'none' }} />
            Sub-agent:{isActive ? ' running' : ''}
            <span style={{ fontSize: 10, fontWeight: 500, color: 'var(--text-3)', textTransform: 'none', letterSpacing: 0 }}>
              {tool.kind && tool.kind !== 'sub-agent' ? tool.kind : ''}
            </span>
          </span>
          <span className="truncate" style={{ fontSize: 12, color: 'var(--text)' }}>{displayTitle}</span>
        </span>
      </span>
      {tool.status && (
        <span
          className="text-[10px] font-medium"
          style={{
            color: statusColor,
            border: '1px solid color-mix(in srgb, currentColor 22%, var(--border))',
            borderRadius: 6,
            background: 'color-mix(in srgb, currentColor 8%, var(--surface))',
            padding: '2px 7px',
            whiteSpace: 'nowrap',
          }}
        >
          {tool.status.replace('_', ' ')}
        </span>
      )}
    </button>
  )
}

function ToolCallInlineRows({ tools, onSelectTool }: { tools: AcpToolCall[]; onSelectTool: (toolId: string) => void }) {
  if (tools.length === 0) return null

  return (
    <div className="uam-tool-timeline">
      {tools.map((tool) => {
        if (tool.isSubAgent) {
          return (
            <div key={tool.id} className="uam-tool-timeline__item">
              <SubAgentRunningPanel tool={tool} onSelectTool={onSelectTool} />
            </div>
          )
        }

        const displayKind = toolDisplayKind(tool)
        const displayTitle = toolDisplayTitle(tool)
        return (
        <div key={tool.id} className="uam-tool-timeline__item">
          <button
            type="button"
            onClick={() => onSelectTool(tool.id)}
            className="w-full grid text-left uam-tool-row"
            style={{
              gridTemplateColumns: '22px 86px minmax(0, 1fr) auto 18px',
            }}
            title="Open tool details"
          >
            <span
              className="inline-flex items-center justify-center"
              style={{
                width: 20,
                height: 20,
                borderRadius: 5,
                color: 'var(--text-2)',
              }}
              aria-hidden="true"
            >
              <svg width="13" height="13" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
                <path d="M5.2 3.2H3.7A1.7 1.7 0 0 0 2 4.9v6.2a1.7 1.7 0 0 0 1.7 1.7h1.5" />
                <path d="M10.8 3.2h1.5A1.7 1.7 0 0 1 14 4.9v6.2a1.7 1.7 0 0 1-1.7 1.7h-1.5" />
                <path d="M6.5 5.4 4.4 8l2.1 2.6M9.5 5.4 11.6 8l-2.1 2.6" />
              </svg>
            </span>
            <span className="text-[11px] font-medium" style={{ color: 'var(--teal)' }}>{displayKind}:</span>
            <span className="text-xs truncate" style={{ color: 'var(--text)' }}>{displayTitle}</span>
            {tool.status && (
              <span
                className="text-[10px] font-medium"
                style={{
                  color: toolStatusColor(tool),
                  border: '1px solid color-mix(in srgb, currentColor 22%, var(--border))',
                  borderRadius: 6,
                  background: 'color-mix(in srgb, currentColor 8%, var(--surface))',
                  padding: '2px 7px',
                }}
              >
                {tool.status.replace('_', ' ')}
              </span>
            )}
            <span style={{ color: 'var(--text-3)' }} aria-hidden="true">
              <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round">
                <path d="m5 6 3 3 3-3" />
              </svg>
            </span>
          </button>
        </div>
        )
      })}
    </div>
  )
}

function PermissionInlineCard({
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
              className="px-2.5 h-7 text-[11px] font-medium"
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
              className="px-2.5 h-7 text-[11px]"
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
            className="px-3 h-7 text-[11px] font-medium"
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

function isCancelPermissionOption(option: AcpPermissionOption) {
  const id = option.id.trim().toLowerCase()
  const name = option.name.trim().toLowerCase()
  const kind = option.kind.trim().toLowerCase()
  return id === 'cancelled' || id === 'cancel' || name === 'cancel' || kind === 'cancel'
}

function normalizePermissionOptions(options: AcpPermissionOption[]) {
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

function UserInputInlineCard({
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
              className="px-2.5 h-7 text-[11px] font-medium"
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
              className="px-2.5 h-7 text-[11px]"
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

function ToolCallModal({
  tool,
  onClose,
  onOpenSubAgent,
}: {
  tool: AcpToolCall
  onClose: () => void
  onOpenSubAgent?: () => void
}) {
  const toolCopyText = [
    toolDisplayTitle(tool) || tool.id || 'Tool call',
    `id: ${tool.id || 'unknown'}`,
    `kind: ${tool.kind || 'unknown'}`,
    `status: ${tool.status || 'unknown'}`,
    ...(tool.isSubAgent ? [`subAgentId: ${tool.subAgentId || 'unknown'}`, `subAgentTitle: ${tool.subAgentTitle || 'unknown'}`] : []),
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
              {toolDisplayTitle(tool)}
            </div>
            <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>
              {[toolDisplayKind(tool), tool.kind, tool.status].filter(Boolean).join(' / ') || 'tool call'}
            </div>
          </div>
          <div className="flex items-center gap-2">
            {tool.isSubAgent && onOpenSubAgent && (
              <button
                type="button"
                title="Open sub-agent chat"
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
            )}
            <CopyTextButton text={toolCopyText} label="Copy" title="Copy tool output" />
          </div>
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
            {tool.isSubAgent && (
              <div><span style={{ color: 'var(--text-3)' }}>sub-agent:</span> {tool.subAgentId || tool.subAgentTitle || 'tracked from provider event'}</div>
            )}
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

type GoalReviewDecision = {
  decision: 'complete' | 'continue' | 'blocked'
  reason: string
  nextPrompt: string
}

function parseGoalReviewDecision(text: string): GoalReviewDecision | null {
  const trimmed = text.trim()
  if (!trimmed.includes('"decision"')) return null

  const first = trimmed.indexOf('{')
  const last = trimmed.lastIndexOf('}')
  if (first < 0 || last <= first) return null

  try {
    const parsed = JSON.parse(trimmed.slice(first, last + 1)) as Partial<GoalReviewDecision>
    if (parsed.decision !== 'complete' && parsed.decision !== 'continue' && parsed.decision !== 'blocked') {
      return null
    }
    return {
      decision: parsed.decision,
      reason: typeof parsed.reason === 'string' ? parsed.reason : '',
      nextPrompt: typeof parsed.nextPrompt === 'string' ? parsed.nextPrompt : '',
    }
  } catch {
    return null
  }
}

function goalReviewDecisionStyle(decision: GoalReviewDecision['decision']) {
  if (decision === 'complete') return { color: 'var(--green)', label: 'Complete' }
  if (decision === 'blocked') return { color: 'var(--red)', label: 'Blocked' }
  return { color: 'var(--purple)', label: 'Continue' }
}

function GoalReviewBlock({ review }: { review: GoalReviewDecision }) {
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
      <AttachmentList attachments={attachments} />
    </div>
  )
}

function AttachmentList({ attachments }: { attachments: Attachment[] }) {
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

function TurnTimelineContent({
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

function ComposerToolbar({
  acp,
  provider,
  providers,
  providerId,
  providerName,
  canSend,
  modelId,
  session,
  reasoningEffort,
  serviceTier,
  approvalModeId,
  autoApproveCommands,
  memoryEnabled,
  canChangeProvider,
  providerOpen,
  modelOpen,
  reasoningOpen,
  speedOpen,
  settingsOpen,
  providerMenuRef,
  modelMenuRef,
  reasoningMenuRef,
  speedMenuRef,
  settingsMenuRef,
  onToggleProvider,
  onToggleModel,
  onToggleReasoning,
  onToggleSpeed,
  onToggleSettings,
  onSelectProvider,
  onSelectModel,
  onSelectReasoning,
  onSelectSpeed,
  onTogglePlan,
  onToggleAcceptEdits,
  onToggleYolo,
  onToggleMemory,
  goalArmed,
  goalActive,
  goalPaused,
  defaultGoalTokenBudget,
  onToggleGoal,
  onSetDefaultGoalTokenBudget,
  onStopRuntime,
  onAttachFile,
  onOpenMarkdownStore,
}: {
  acp?: AcpBinding
  provider: Provider
  providers: Provider[]
  providerId: string
  providerName: string
  canSend: boolean
  modelId?: string
  session: { id: string }
  reasoningEffort?: string
  serviceTier?: string
  approvalModeId?: string
  autoApproveCommands: boolean
  memoryEnabled: boolean
  canChangeProvider: boolean
  providerOpen: boolean
  modelOpen: boolean
  reasoningOpen: boolean
  speedOpen: boolean
  settingsOpen: boolean
  providerMenuRef: RefObject<HTMLDivElement>
  modelMenuRef: RefObject<HTMLDivElement>
  reasoningMenuRef: RefObject<HTMLDivElement>
  speedMenuRef: RefObject<HTMLDivElement>
  settingsMenuRef: RefObject<HTMLDivElement>
  onToggleProvider: () => void
  onToggleModel: () => void
  onToggleReasoning: () => void
  onToggleSpeed: () => void
  onToggleSettings: () => void
  onSelectProvider: (providerId: string) => void
  onSelectModel: (modelId: string) => void
  onSelectReasoning: (reasoningEffort: string) => void
  onSelectSpeed: (serviceTier: string) => void
  onTogglePlan: () => void
  onToggleAcceptEdits: () => void
  onToggleYolo: () => void
  onToggleMemory: () => void
  goalArmed: boolean
  goalActive: boolean
  goalPaused: boolean
  defaultGoalTokenBudget: number
  onToggleGoal: () => void
  onSetDefaultGoalTokenBudget: (value: number) => void
  onStopRuntime: () => void
  onAttachFile: () => void
  onOpenMarkdownStore: () => void
}) {
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const codexProvider = isCodexProvider(provider, providerId)
  const reasoningOptions = codexProvider ? buildCodexReasoningOptions(acp, currentModel.id, reasoningEffort ?? '') : []
  const speedOptions = codexProvider ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '') : []
  const currentReasoning = modelOptionFor(reasoningOptions, reasoningEffort)
  const currentSpeed = modelOptionFor(speedOptions, serviceTier)
  const providerOptions = providers.length > 0 ? providers : [provider]
  const modelDisabled = acpRuntimeBlocksControlChanges(acp)
  const planActive = approvalModeId === 'plan'
  const acceptEditsActive = approvalModeId === 'acceptEdits'
  const yoloActive = autoApproveCommands
  const claudeProvider = isClaudeProvider(provider, providerId)
  const hasRuntimeModes = Boolean(acp?.running && acp.availableModes.length > 0)
  const planAvailable = !hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'plan')
  const acceptEditsAvailable = claudeProvider && (!hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'acceptEdits'))
  const yoloAvailable = true
  const planDisabled = Boolean(modelDisabled || !planAvailable)
  const acceptEditsDisabled = Boolean(modelDisabled || !acceptEditsAvailable)
  const yoloDisabled = false
  const memoryDisabled = Boolean(modelDisabled)
  const autoLabel = claudeProvider ? 'Auto' : 'Yolo'
  const modeLabel = planActive ? 'Plan' : acceptEditsActive ? 'Accept Edits' : 'Default'
  const running = Boolean(acp?.processing)
  const chipStyle = {
    height: 26,
    borderRadius: 6,
    border: '1px solid var(--border)',
    background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
    color: 'var(--text-2)',
  }
  const iconChipStyle = {
    ...chipStyle,
    width: 30,
    justifyContent: 'center',
  }

  return (
    <div
      className="flex items-center gap-2 flex-wrap px-2 py-2 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      {(providerOptions.length > 1 || providerId !== providerOptions[0]?.id) && (
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
          <ProviderLogo providerId={providerId} />
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
              const candidateName = providerShortName(candidate, candidate.id)
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
                  <ProviderLogo providerId={candidate.id} />
                  <span className="flex-1">{candidateName}</span>
                  {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                </button>
              )
            })}
          </div>
        )}
      </div>
      )}
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
      {codexProvider && (
        <div ref={reasoningMenuRef} className="relative">
          <button
            type="button"
            title="Select Codex reasoning"
            onClick={onToggleReasoning}
            disabled={modelDisabled}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: reasoningOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: reasoningOpen ? 'var(--border-bright)' : 'var(--border)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span>Reasoning</span>
            <span style={{ color: 'var(--text)' }}>{currentReasoning.shortLabel}</span>
          </button>
          {reasoningOpen && !modelDisabled && (
            <div
              className="absolute left-0"
              style={{
                bottom: 32,
                width: 250,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 6,
              }}
            >
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Reasoning</div>
              {reasoningOptions.map((option) => {
                const selected = option.id === currentReasoning.id
                return (
                  <button
                    key={option.id || 'default'}
                    type="button"
                    onClick={() => onSelectReasoning(option.id)}
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
      )}
      {codexProvider && (
        <div ref={speedMenuRef} className="relative">
          <button
            type="button"
            title="Select Codex speed"
            onClick={onToggleSpeed}
            disabled={modelDisabled}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: speedOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: speedOpen ? 'var(--border-bright)' : 'var(--border)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span>Speed</span>
            <span style={{ color: 'var(--text)' }}>{currentSpeed.shortLabel}</span>
          </button>
          {speedOpen && !modelDisabled && (
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
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Speed</div>
              {speedOptions.map((option) => {
                const selected = option.id === currentSpeed.id
                return (
                  <button
                    key={option.id || 'default'}
                    type="button"
                    onClick={() => onSelectSpeed(option.id)}
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
      )}
      <button
        type="button"
        title={planAvailable ? 'Toggle planning mode. Claude Plan is read-only and will not edit files.' : 'Planning mode unavailable'}
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
      {claudeProvider && (
        <button
          type="button"
          title={acceptEditsAvailable ? 'Toggle Accept Edits mode. Claude can edit workspace files without prompting.' : 'Accept Edits mode unavailable'}
          aria-pressed={acceptEditsActive}
          onClick={onToggleAcceptEdits}
          disabled={acceptEditsDisabled}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            borderColor: acceptEditsActive ? 'color-mix(in srgb, var(--green) 52%, var(--border))' : 'var(--border)',
            background: acceptEditsActive ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background,
            color: acceptEditsActive ? 'var(--text)' : 'var(--text-2)',
            opacity: acceptEditsDisabled ? 0.55 : 1,
          }}
        >
          <span style={{ color: acceptEditsActive ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
          <span>Accept Edits</span>
        </button>
      )}
      <button
        type="button"
        title={yoloAvailable ? `Toggle ${autoLabel} mode` : `${autoLabel} mode unavailable`}
        aria-pressed={yoloActive}
        onClick={onToggleYolo}
        disabled={yoloDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: yoloActive ? 'color-mix(in srgb, var(--yellow) 55%, var(--border))' : 'var(--border)',
          background: yoloActive ? 'color-mix(in srgb, var(--yellow) 16%, var(--surface))' : chipStyle.background,
          color: yoloActive ? 'var(--text)' : 'var(--text-2)',
          opacity: yoloDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: yoloActive ? 'var(--yellow)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>{autoLabel}</span>
      </button>
      <button
        type="button"
        title="Toggle memory"
        aria-pressed={memoryEnabled}
        onClick={onToggleMemory}
        disabled={memoryDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: memoryEnabled ? 'color-mix(in srgb, var(--green) 50%, var(--border))' : 'var(--border)',
          background: memoryEnabled ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background,
          color: memoryEnabled ? 'var(--text)' : 'var(--text-2)',
          opacity: memoryDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: memoryEnabled ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>Memory</span>
      </button>
      <button
        type="button"
        title={goalActive ? 'Pause goal mode' : goalPaused ? 'Resume goal mode' : goalArmed ? 'Next message will become the goal' : 'Use the next message as a goal'}
        aria-pressed={goalActive || goalArmed}
        onClick={onToggleGoal}
        disabled={modelDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 55%, var(--border))' : 'var(--border)',
          background: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 16%, var(--surface))' : chipStyle.background,
          color: goalActive || goalArmed ? 'var(--text)' : 'var(--text-2)',
          opacity: modelDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: goalActive || goalArmed ? 'var(--purple)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>{goalArmed ? 'Goal Next' : 'Goal'}</span>
      </button>
      <button
        type="button"
        title="Attach files"
        onClick={onAttachFile}
        className="inline-flex items-center"
        style={iconChipStyle}
      >
        <ComposerIcon name="plus" />
      </button>
      <button
        type="button"
        title="Open Markdown Store"
        onClick={onOpenMarkdownStore}
        className="inline-flex items-center"
        style={iconChipStyle}
      >
        <ComposerIcon name="markdown" />
      </button>
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
                  <span style={{ color: 'var(--text-3)' }}>Model</span>
                  <span>{currentModel.label}</span>
                </div>
                {codexProvider && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Reasoning</span>
                    <span>{currentReasoning.label}</span>
                  </div>
                )}
                {codexProvider && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Speed</span>
                    <span>{currentSpeed.label}</span>
                  </div>
                )}
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Mode</span>
                  <span>{modeLabel}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Memory</span>
                  <span>{memoryEnabled ? 'On' : 'Off'}</span>
                </div>
                <label className="grid gap-1">
                  <span style={{ color: 'var(--text-3)' }}>Goal token budget</span>
                  <input
                    type="number"
                    min={0}
                    value={defaultGoalTokenBudget || ''}
                    placeholder="Unlimited"
                    onChange={(event) => onSetDefaultGoalTokenBudget(parseInt(event.target.value || '0', 10))}
                    className="w-full px-2 py-1 text-xs"
                    style={{
                      border: '1px solid var(--border)',
                      borderRadius: 6,
                      background: 'var(--bg)',
                      color: 'var(--text)',
                      outline: 'none',
                    }}
                  />
                </label>
              </div>
            </div>
          )}
        </div>
        <button
          type={running ? 'button' : 'submit'}
          title={running ? 'Stop runtime' : 'Send prompt'}
          disabled={!running && !canSend}
          onClick={running ? onStopRuntime : undefined}
          className="h-[30px] w-[34px] text-xs font-semibold inline-flex items-center justify-center"
          style={{
            borderRadius: 7,
            border: running
              ? '1px solid color-mix(in srgb, var(--red) 46%, var(--border-bright))'
              : '1px solid color-mix(in srgb, var(--accent) 64%, var(--border-bright))',
            background: running ? 'color-mix(in srgb, var(--red) 14%, var(--surface))' : canSend ? 'var(--accent)' : 'var(--surface-up)',
            color: running ? 'var(--red)' : canSend ? '#fff' : 'var(--text-3)',
            boxShadow: !running && canSend ? '0 8px 18px color-mix(in srgb, var(--accent) 20%, transparent)' : 'none',
          }}
        >
          {running ? (
            <span aria-hidden="true" style={{ width: 9, height: 9, borderRadius: 2, background: 'currentColor' }} />
          ) : (
            <ComposerIcon name="send" size={15} />
          )}
        </button>
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
  const [reasoningOpen, setReasoningOpen] = useState(false)
  const [speedOpen, setSpeedOpen] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [claudePlanPrompt, setClaudePlanPrompt] = useState<string | null>(null)
  const [openWorkspaceError, setOpenWorkspaceError] = useState('')
  const [workspaceActionMessage, setWorkspaceActionMessage] = useState('')
  const [workspaceActionBusy, setWorkspaceActionBusy] = useState(false)
  const [goalError, setGoalError] = useState('')
  const [goalSubmitting, setGoalSubmitting] = useState(false)
  const [goalArmNextMessage, setGoalArmNextMessage] = useState(false)
  const [composerAttachments, setComposerAttachments] = useState<LocalAttachment[]>([])
  const [attachmentError, setAttachmentError] = useState('')
  const [renderedMessageCount, setRenderedMessageCount] = useState(INITIAL_RENDERED_MESSAGES)
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const providers = useAppStore((s) => s.providers)
  const stageChatAttachments = useAppStore((s) => s.stageChatAttachments)
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const stopAcpSession = useAppStore((s) => s.stopAcpSession)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const resolveAcpUserInput = useAppStore((s) => s.resolveAcpUserInput)
  const setSessionProvider = useAppStore((s) => s.setSessionProvider)
  const setSessionModel = useAppStore((s) => s.setSessionModel)
  const setSessionCodexOptions = useAppStore((s) => s.setSessionCodexOptions)
  const setSessionApprovalMode = useAppStore((s) => s.setSessionApprovalMode)
  const setSessionAutoApproveCommands = useAppStore((s) => s.setSessionAutoApproveCommands)
  const setSessionMemoryEnabled = useAppStore((s) => s.setSessionMemoryEnabled)
  const openSessionWorkspace = useAppStore((s) => s.openSessionWorkspace)
  const openSessionWorkspaceEditor = useAppStore((s) => s.openSessionWorkspaceEditor)
  const openSessionTerminal = useAppStore((s) => s.openSessionTerminal)
  const openSubAgentSession = useAppStore((s) => s.openSubAgentSession)
  const createChatWorktree = useAppStore((s) => s.createChatWorktree)
  const discardChatWorktreeChanges = useAppStore((s) => s.discardChatWorktreeChanges)
  const portChatWorktreeChanges = useAppStore((s) => s.portChatWorktreeChanges)
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const markdownStoreAttachments = useAppStore(useShallow((s) => s.markdownStoreAttachedBySessionId[session.id] ?? []))
  const detachMarkdownStoreEntry = useAppStore((s) => s.detachMarkdownStoreEntry)
  const goals = useAppStore((s) => s.goalsByChatId[session.id] ?? [])
  const activeGoalId = useAppStore((s) => s.activeGoalIdByChatId[session.id] ?? null)
  const setGoalStore = useAppStore((s) => s.setGoal)
  const updateGoalStatus = useAppStore((s) => s.updateGoalStatus)
  const removeGoal = useAppStore((s) => s.removeGoal)
  const resumeGoal = useAppStore((s) => s.resumeGoal)
  const defaultGoalTokenBudget = useAppStore((s) => s.defaultGoalTokenBudgetByChatId[session.id] ?? 0)
  const setDefaultGoalTokenBudget = useAppStore((s) => s.setDefaultGoalTokenBudget)
  const bottomRef = useRef<HTMLDivElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)
  const providerMenuRef = useRef<HTMLDivElement>(null)
  const modelMenuRef = useRef<HTMLDivElement>(null)
  const reasoningMenuRef = useRef<HTMLDivElement>(null)
  const speedMenuRef = useRef<HTMLDivElement>(null)
  const settingsMenuRef = useRef<HTMLDivElement>(null)

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
  const earliestRenderedMessageIndex = Math.max(0, messages.length - renderedMessageCount)
  const visibleMessages = useMemo(
    () => messages.slice(earliestRenderedMessageIndex),
    [earliestRenderedMessageIndex, messages]
  )

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
    setComposerAttachments([])
    setAttachmentError('')
    setOpenWorkspaceError('')
    setWorkspaceActionMessage('')
    setWorkspaceActionBusy(false)
    setGoalArmNextMessage(false)
    setRenderedMessageCount(INITIAL_RENDERED_MESSAGES)
  }, [session.id])

  useEffect(() => {
    if (session.workspaceIsolationKind !== 'gitWorktree') {
      setWorkspaceActionMessage('')
    }
  }, [session.workspaceIsolationKind])

  const runtimeBlocksControlChanges = acpRuntimeBlocksControlChanges(acp)

  useEffect(() => {
    if (runtimeBlocksControlChanges) {
      setModelOpen(false)
      setReasoningOpen(false)
      setSpeedOpen(false)
    }
  }, [runtimeBlocksControlChanges])

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

      if (reasoningOpen && reasoningMenuRef.current && !reasoningMenuRef.current.contains(target)) {
        setReasoningOpen(false)
      }

      if (speedOpen && speedMenuRef.current && !speedMenuRef.current.contains(target)) {
        setSpeedOpen(false)
      }

      if (settingsOpen && settingsMenuRef.current && !settingsMenuRef.current.contains(target)) {
        setSettingsOpen(false)
      }
    }

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setProviderOpen(false)
      setModelOpen(false)
      setReasoningOpen(false)
      setSpeedOpen(false)
      setSettingsOpen(false)
      setSelectedToolCallRef(null)
    }

    document.addEventListener('mousedown', onMouseDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onMouseDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [modelOpen, providerOpen, reasoningOpen, settingsOpen, speedOpen])

  const stageFiles = async (files: File[]) => {
    const realFiles = files.filter((file) => file.size > 0 || file.type || file.name)
    if (realFiles.length === 0) return

    const pending = realFiles.map<LocalAttachment>((file) => ({
      id: `${Date.now()}-${Math.random().toString(16).slice(2)}-${file.name}`,
      name: file.name || 'attachment',
      type: file.type.startsWith('image/') ? 'image' : 'file',
      size: file.size,
      path: filePathFromBrowserFile(file),
      status: 'staging',
    }))
    setAttachmentError('')
    setComposerAttachments((current) => [...current, ...pending])

    try {
      const items: ChatAttachmentInput[] = []
      for (let index = 0; index < realFiles.length; index += 1) {
        const file = realFiles[index]
        const pendingItem = pending[index]
        const path = filePathFromBrowserFile(file)
        items.push({
          id: pendingItem.id,
          name: file.name || 'attachment',
          kind: file.type.startsWith('image/') ? 'image' : 'file',
          mimeType: file.type,
          size: file.size,
          path,
          dataBase64: path ? undefined : await readFileBase64(file),
        })
      }

      const staged = await stageChatAttachments(session.id, items)
      setComposerAttachments((current) =>
        current.map((attachment) => {
          const replacement = staged.find((candidate) => candidate.id === attachment.id)
          return replacement
            ? { ...replacement, status: 'ready' as LocalAttachmentStatus }
            : attachment
        })
      )
    } catch (err) {
      const message = err instanceof Error ? err.message : 'Failed to stage attachments.'
      setAttachmentError(message)
      const failedIds = new Set(pending.map((attachment) => attachment.id))
      setComposerAttachments((current) =>
        current.map((attachment) =>
          failedIds.has(attachment.id) ? { ...attachment, status: 'failed', error: message } : attachment
        )
      )
    }
  }

  const stageDirectoryPaths = async (paths: string[]) => {
    const uniquePaths = Array.from(new Set(paths.map((path) => path.trim()).filter(Boolean)))
    if (uniquePaths.length === 0) return

    const pending = uniquePaths.map<LocalAttachment>((path) => ({
      id: `${Date.now()}-${Math.random().toString(16).slice(2)}-${path}`,
      name: path.split(/[\\/]/).pop() || path,
      type: 'directory',
      size: 0,
      path,
      status: 'staging',
    }))
    setAttachmentError('')
    setComposerAttachments((current) => [...current, ...pending])

    try {
      const staged = await stageChatAttachments(session.id, pending.map((attachment) => ({
        id: attachment.id,
        name: attachment.name,
        kind: 'directory',
        path: attachment.path,
      })))
      setComposerAttachments((current) =>
        current.map((attachment) => {
          const replacement = staged.find((candidate) => candidate.id === attachment.id)
          return replacement
            ? { ...replacement, status: 'ready' as LocalAttachmentStatus }
            : attachment
        })
      )
    } catch (err) {
      const message = err instanceof Error ? err.message : 'Failed to stage directory references.'
      setAttachmentError(message)
      const failedIds = new Set(pending.map((attachment) => attachment.id))
      setComposerAttachments((current) =>
        current.map((attachment) =>
          failedIds.has(attachment.id) ? { ...attachment, status: 'failed', error: message } : attachment
        )
      )
    }
  }

  const onComposerDrop = (event: DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    const files = Array.from(event.dataTransfer.files)
    const uriList = event.dataTransfer.getData('text/uri-list')
    if (files.length === 0 && uriList.trim()) {
      const paths = uriList
        .split(/\r?\n/)
        .filter((line) => line.trim() && !line.startsWith('#'))
        .map(fileUriToPath)
      void stageDirectoryPaths(paths)
    }
    void stageFiles(files)
  }

  const onComposerPaste = (event: ClipboardEvent<HTMLTextAreaElement>) => {
    const files = Array.from(event.clipboardData.files)
    if (files.length === 0) return
    void stageFiles(files)
  }

  const submitGoal = async (prompt: string) => {
    const goalMatch = prompt.match(/^\/goal\s+(.+?)(?:\s+--budget\s+(\d+))?\s*$/)
    if (!goalMatch) return false

    const objective = goalMatch[1].trim()
    const tokenBudget = goalMatch[2] ? parseInt(goalMatch[2], 10) : defaultGoalTokenBudget

    if (!objective) {
      setGoalError('Goal objective is required.')
      return true
    }

    setGoalSubmitting(true)
    const goalId = await setGoalStore(session.id, objective, tokenBudget)
    setGoalSubmitting(false)

    if (goalId) {
      setDraft('')
      setGoalError('')
    } else {
      setGoalError('Failed to create goal.')
    }
    return true
  }

  const submit = async (event?: FormEvent) => {
    event?.preventDefault()
    if (!canSend) return
    const prompt = draft.trim()

    // Handle /goal command
    if (prompt.startsWith('/goal ')) {
      void submitGoal(prompt)
      return
    }

    const readyAttachments = composerAttachments
      .filter((attachment) => attachment.status === 'ready')
      .map(({ status, error, ...attachment }) => attachment)
    if (goalArmNextMessage) {
      setGoalSubmitting(true)
      const goalId = await setGoalStore(session.id, prompt, defaultGoalTokenBudget)
      if (!goalId) {
        setGoalSubmitting(false)
        setGoalError('Failed to create goal.')
        return
      }
      const ok = await sendAcpPrompt(session.id, prompt, readyAttachments)
      setGoalSubmitting(false)
      if (!ok) {
        setGoalError('Goal was created, but the first prompt failed to send.')
        return
      }
      setGoalError('')
      setGoalArmNextMessage(false)
      setDraft('')
      setComposerAttachments([])
      setAttachmentError('')
      return
    }

    if (isClaudeProvider(currentProvider, currentProviderId) && currentModeId === 'plan') {
      setClaudePlanPrompt(prompt)
      return
    }
    setSubmitting(true)
    const ok = await sendAcpPrompt(session.id, prompt, readyAttachments)
    setSubmitting(false)
    if (ok) {
      setDraft('')
      setComposerAttachments([])
      setAttachmentError('')
    }
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
  const isGitWorktree = session.workspaceIsolationKind === 'gitWorktree'
  const sourceWorkspaceDirectory = session.workspaceSourceDirectory?.trim() || (!isGitWorktree ? workspaceDirectory : '')
  const workspaceActionsDisabled = workspaceActionBusy || Boolean(acp?.running || acp?.processing)
  const openWorkspace = async () => {
    if (!workspaceDirectory) return
    setOpenWorkspaceError('')
    const ok = await openSessionWorkspace(session.id)
    if (!ok) {
      setOpenWorkspaceError('Failed to open workspace directory.')
    }
  }
  const openWorkspaceEditor = async () => {
    if (!workspaceDirectory) return
    setOpenWorkspaceError('')
    setWorkspaceActionMessage('')
    const ok = await openSessionWorkspaceEditor(session.id)
    if (ok) {
      setWorkspaceActionMessage('Opened workspace editor.')
    } else {
      setOpenWorkspaceError('Failed to open workspace editor.')
    }
  }
  const openWorkspaceTerminal = async () => {
    if (!workspaceDirectory) return
    setOpenWorkspaceError('')
    setWorkspaceActionMessage('')
    const ok = await openSessionTerminal(session.id)
    if (ok) {
      setWorkspaceActionMessage('Opened terminal at workspace.')
    } else {
      setOpenWorkspaceError('Failed to open terminal.')
    }
  }
  const openSelectedSubAgentSession = async () => {
    if (!selectedToolCall?.isSubAgent || !selectedToolCall.subAgentId) return
    const ok = await openSubAgentSession(session.id, selectedToolCall.subAgentId, selectedToolCall.subAgentTitle)
    if (ok) {
      setSelectedToolCallRef(null)
    }
  }
  const runWorkspaceAction = async (action: 'create' | 'discard' | 'port') => {
    if (workspaceActionsDisabled) return
    setOpenWorkspaceError('')
    setWorkspaceActionMessage('')
    setWorkspaceActionBusy(true)
    const result =
      action === 'create'
        ? await createChatWorktree(session.id)
        : action === 'discard'
          ? await discardChatWorktreeChanges(session.id)
          : await portChatWorktreeChanges(session.id)
    setWorkspaceActionBusy(false)
    if (result.ok) {
      setWorkspaceActionMessage(result.message || (action === 'port' ? 'Applied chat changes and returned to the source workspace.' : 'Workspace action complete.'))
    } else {
      setOpenWorkspaceError(result.message || 'Workspace action failed.')
    }
  }
  const currentProviderId = session.providerId || acp?.providerId || DEFAULT_PROVIDER_ID
  const providerSupported = providers.some((candidate) => candidate.id === currentProviderId)
  const currentProvider = useMemo<Provider>(
    () =>
      providers.find((candidate) => candidate.id === currentProviderId) ?? fallbackProviderForId(currentProviderId),
    [currentProviderId, providers]
  )
  const currentProviderName = providerShortName(currentProvider, currentProviderId)
  const currentRuntimeLabel = providerRuntimeLabel(currentProvider, acp)
  const currentErrorTitle = `${currentProviderName} ${currentRuntimeLabel} error`
  const unsupportedProviderMessage = providerSupported
    ? ''
    : `${currentProviderName} is not supported in this build. Switch this chat to Gemini CLI to continue.`
  const canChangeProvider = messages.length === 0 && !acp?.running && !acp?.processing
  const canSend = useMemo(
    () => providerSupported && draft.trim().length > 0 && !submitting && !goalSubmitting && !acp?.processing && !composerAttachments.some((attachment) => attachment.status !== 'ready'),
    [providerSupported, draft, submitting, goalSubmitting, acp?.processing, composerAttachments]
  )
  const currentModelId = acp?.currentModelId || session.modelId || ''
  const currentModeId = acp?.currentModeId || session.approvalMode || 'default'
  const latestPlanMessageIndex = messages.reduce((latest, message, index) => {
    const hasPlan = message.role === 'assistant' && (Boolean(message.planSummary?.trim()) || (message.planEntries?.length ?? 0) > 0)
    return hasPlan ? index : latest
  }, -1)
  const latestPlanHasLaterUser =
    latestPlanMessageIndex >= 0 && messages.slice(latestPlanMessageIndex + 1).some((message) => message.role === 'user')
  const canShowPlanActions = isCodexProvider(currentProvider, currentProviderId) && latestPlanMessageIndex >= 0 && !latestPlanHasLaterUser
  const planActionBlockedByRuntime = runtimeBlocksControlChanges
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
  const activeGoal = activeGoalId ? goals.find((g) => g.id === activeGoalId) ?? null : null
  const displayedGoal = activeGoal ?? (goals.length > 0 ? goals[goals.length - 1] : null)
  const goalPaused = displayedGoal?.status === 'paused'
  const handleCompleteGoal = () => {
    if (displayedGoal) {
      void updateGoalStatus(displayedGoal.id, 'complete')
    }
  }
  const handleResumeGoal = () => {
    if (displayedGoal) {
      void resumeGoal(session.id, displayedGoal.id)
    }
  }
  const handlePauseGoal = () => {
    if (displayedGoal) {
      void updateGoalStatus(displayedGoal.id, 'paused')
    }
  }
  const handleRemoveGoal = () => {
    if (displayedGoal) {
      void removeGoal(displayedGoal.id)
    }
  }
  const handleToggleGoal = () => {
    if (activeGoal) {
      void updateGoalStatus(activeGoal.id, 'paused')
      return
    }
    if (displayedGoal && displayedGoal.status !== 'active') {
      void resumeGoal(session.id, displayedGoal.id)
      return
    }
    setGoalArmNextMessage((value) => !value)
    setGoalError('')
  }

  const activePlanActions = canShowPlanActions && providerSupported
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
  const resolveClaudePlanPrompt = async (nextModeId: 'acceptEdits' | 'default' | 'plan') => {
    const prompt = claudePlanPrompt?.trim()
    if (!prompt) {
      setClaudePlanPrompt(null)
      return
    }

    setSubmitting(true)
    const modeOk = nextModeId === 'plan' ? true : await setSessionApprovalMode(session.id, nextModeId)
    if (modeOk) {
      const readyAttachments = composerAttachments
        .filter((attachment) => attachment.status === 'ready')
        .map(({ status, error, ...attachment }) => attachment)
      const ok = readyAttachments.length > 0
        ? await sendAcpPrompt(session.id, prompt, readyAttachments)
        : await sendAcpPrompt(session.id, prompt)
      if (ok) {
        setDraft('')
        setComposerAttachments([])
        setAttachmentError('')
        setClaudePlanPrompt(null)
      }
    }
    setSubmitting(false)
  }

  return (
    <div className="relative h-full flex overflow-hidden" style={{ background: 'var(--bg)' }}>
      {selectedToolCall && (
        <ToolCallModal
          tool={selectedToolCall}
          onClose={() => setSelectedToolCallRef(null)}
          onOpenSubAgent={selectedToolCall.isSubAgent ? () => void openSelectedSubAgentSession() : undefined}
        />
      )}
      <div className="flex-1 flex flex-col min-w-0">
        <div className="flex-1 overflow-auto" data-copy-surface="chat">
          <div className="w-full px-4 py-4">
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
              {earliestRenderedMessageIndex > 0 && (
                <div className="flex justify-center">
                  <button
                    type="button"
                    className="uam-secondary-button"
                    onClick={() => setRenderedMessageCount((current) => current + RENDERED_MESSAGE_BATCH_SIZE)}
                  >
                    <span>Show earlier messages</span>
                  </button>
                </div>
              )}
              {visibleMessages.map((message, visibleIndex) => {
                const index = earliestRenderedMessageIndex + visibleIndex
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
                            waitIsStale={acp?.waitIsStale}
                            waitStaleReason={acp?.waitStaleReason}
                            waitSeconds={acp?.waitSeconds}
	                          onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                          onResolvePermission={(requestId, optionId) => {
	                            void resolveAcpPermission(session.id, requestId, optionId)
	                          }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
                            onCancelTurn={() => void cancelAcpTurn(session.id)}
                            onStopRuntime={() => void stopAcpSession(session.id)}
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
                            waitIsStale={acp?.waitIsStale}
                            waitStaleReason={acp?.waitStaleReason}
                            waitSeconds={acp?.waitSeconds}
	                          onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                          onResolvePermission={(requestId, optionId) => {
	                            void resolveAcpPermission(session.id, requestId, optionId)
	                          }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
                            onCancelTurn={() => void cancelAcpTurn(session.id)}
                            onStopRuntime={() => void stopAcpSession(session.id)}
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
                      waitIsStale={acp?.waitIsStale}
                      waitStaleReason={acp?.waitStaleReason}
                      waitSeconds={acp?.waitSeconds}
	                    onSelectTool={(toolId) => setSelectedToolCallRef({ id: toolId })}
	                    onResolvePermission={(requestId, optionId) => {
	                      void resolveAcpPermission(session.id, requestId, optionId)
	                    }}
                      onResolveUserInput={(requestId, answers) => {
                        void resolveAcpUserInput(session.id, requestId, answers)
                      }}
                      onCancelTurn={() => void cancelAcpTurn(session.id)}
                      onStopRuntime={() => void stopAcpSession(session.id)}
                  />
                </MessageFrame>
              )}
              <div ref={bottomRef} />
            </div>
          </div>
        </div>

        {goalError && (
          <div className="px-4 py-1 text-xs" style={{ background: 'var(--surface)', color: 'var(--danger)' }}>
            {goalError}
          </div>
        )}

        {displayedGoal && (
          <GoalBanner
            goal={displayedGoal}
            onComplete={handleCompleteGoal}
            onPause={handlePauseGoal}
            onResume={handleResumeGoal}
            onRemove={handleRemoveGoal}
          />
        )}

        <form
          onSubmit={submit}
          className="flex-shrink-0"
          style={{
            borderTop: '1px solid var(--border)',
            background: 'var(--surface)',
          }}
        >
	          <div className="p-3">
	            <div
	              className="mb-2 flex flex-wrap items-center gap-2 text-[11px]"
	              style={{ color: 'var(--text-3)', minWidth: 0 }}
	              title={workspaceDirectory || 'No workspace directory selected'}
	            >
	              <span style={{ color: 'var(--text-2)', flexShrink: 0 }}>Workspace</span>
	              {isGitWorktree && (
	                <span
	                  style={{
	                    border: '1px solid color-mix(in srgb, var(--green) 42%, var(--border))',
	                    borderRadius: 6,
	                    background: 'color-mix(in srgb, var(--green) 10%, var(--surface))',
	                    color: 'var(--text-2)',
	                    padding: '3px 7px',
	                    flexShrink: 0,
	                  }}
	                  title={sourceWorkspaceDirectory ? `Source: ${sourceWorkspaceDirectory}` : 'Isolated Git worktree'}
	                >
	                  Git worktree
	                </span>
	              )}
	              <span
	                className="min-w-0 flex-1 truncate"
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
	              <button
	                type="button"
                disabled={!workspaceDirectory}
                onClick={() => void openWorkspace()}
                className="h-[24px] w-[28px] inline-flex flex-shrink-0 items-center justify-center text-[11px] font-medium"
                title="Open workspace in Finder or File Explorer"
                style={{
                  border: '1px solid var(--border)',
                  borderRadius: 6,
                  background: workspaceDirectory ? 'var(--surface-up)' : 'var(--bg)',
                  color: workspaceDirectory ? 'var(--text-2)' : 'var(--text-3)',
                  opacity: workspaceDirectory ? 1 : 0.55,
                }}
	              >
	                <ComposerIcon name="folder" size={13} />
	      </button>
	      <button
        type="button"
        disabled={!workspaceDirectory}
        onClick={() => void openWorkspaceEditor()}
        className="h-[24px] w-[28px] inline-flex flex-shrink-0 items-center justify-center text-[11px] font-medium"
        title="Open workspace in configured editor"
        style={{
          border: '1px solid var(--border)',
          borderRadius: 6,
          background: workspaceDirectory ? 'var(--surface-up)' : 'var(--bg)',
          color: workspaceDirectory ? 'var(--text-2)' : 'var(--text-3)',
          opacity: workspaceDirectory ? 1 : 0.55,
        }}
      >
        <ComposerIcon name="editor" size={13} />
      </button>
      <button
        type="button"
        disabled={!workspaceDirectory}
        onClick={() => void openWorkspaceTerminal()}
        className="h-[24px] w-[28px] inline-flex flex-shrink-0 items-center justify-center text-[11px] font-medium"
        title={!workspaceDirectory ? 'Select a workspace directory to open a terminal' : 'Open a terminal at the workspace location'}
        style={{
          border: '1px solid var(--border)',
          borderRadius: 6,
          background: workspaceDirectory ? 'var(--surface-up)' : 'var(--bg)',
          color: workspaceDirectory ? 'var(--text-2)' : 'var(--text-3)',
          opacity: workspaceDirectory ? 1 : 0.55,
        }}
      >
        <ComposerIcon name="terminal" size={13} />
      </button>
      {!isGitWorktree && (
        <button
          type="button"
          disabled={!workspaceDirectory || workspaceActionsDisabled}
          onClick={() => void runWorkspaceAction('create')}
          className="h-[24px] w-[28px] inline-flex flex-shrink-0 items-center justify-center text-[11px] font-medium"
          title={workspaceActionsDisabled ? 'Stop the runtime before changing workspace isolation' : 'Create an isolated Git worktree for this chat'}
          style={{
            border: '1px solid var(--border)',
            borderRadius: 6,
            background: workspaceDirectory && !workspaceActionsDisabled ? 'var(--surface-up)' : 'var(--bg)',
            color: workspaceDirectory && !workspaceActionsDisabled ? 'var(--text-2)' : 'var(--text-3)',
            opacity: workspaceDirectory && !workspaceActionsDisabled ? 1 : 0.55,
          }}
        >
          <ComposerIcon name="git-tree" size={13} />
        </button>
      )}
	              {isGitWorktree && (
	                <>
	                  <button
	                    type="button"
	                    disabled={workspaceActionsDisabled}
	                    onClick={() => void runWorkspaceAction('discard')}
	                    className="h-[24px] flex-shrink-0 px-2 text-[11px] font-medium"
	                    title={workspaceActionsDisabled ? 'Stop the runtime before discarding worktree changes' : 'Discard worktree changes and return this chat to the source workspace'}
	                    style={{
	                      border: '1px solid var(--border)',
	                      borderRadius: 6,
	                      background: workspaceActionsDisabled ? 'var(--bg)' : 'var(--surface-up)',
	                      color: workspaceActionsDisabled ? 'var(--text-3)' : 'var(--text-2)',
	                      opacity: workspaceActionsDisabled ? 0.55 : 1,
	                    }}
	                  >
	                    Discard & return
	                  </button>
	                  <button
	                    type="button"
	                    disabled={workspaceActionsDisabled}
	                    onClick={() => void runWorkspaceAction('port')}
	                    className="h-[24px] flex-shrink-0 px-2 text-[11px] font-medium"
	                    title={workspaceActionsDisabled ? 'Stop the runtime before porting worktree changes' : 'Apply this chat worktree diff and return this chat to the source workspace'}
	                    style={{
	                      border: '1px solid color-mix(in srgb, var(--accent) 42%, var(--border))',
	                      borderRadius: 6,
	                      background: workspaceActionsDisabled ? 'var(--bg)' : 'color-mix(in srgb, var(--accent) 12%, var(--surface))',
	                      color: workspaceActionsDisabled ? 'var(--text-3)' : 'var(--text)',
	                      opacity: workspaceActionsDisabled ? 0.55 : 1,
	                    }}
	                  >
	                    Port back
	                  </button>
	                </>
	              )}
	            </div>
	            {isGitWorktree && sourceWorkspaceDirectory && (
	              <div
	                className="mb-2 truncate text-[11px]"
	                style={{ color: 'var(--text-3)' }}
	                title={sourceWorkspaceDirectory}
	              >
	                Source {sourceWorkspaceDirectory}
	              </div>
	            )}
	            {workspaceActionMessage && (
	              <div
	                className="mb-2 text-xs"
	                style={{
	                  border: '1px solid color-mix(in srgb, var(--green) 38%, var(--border))',
	                  borderRadius: 6,
	                  padding: '8px 10px',
	                  background: 'color-mix(in srgb, var(--green) 8%, var(--surface))',
	                  color: 'var(--text)',
	                  overflowWrap: 'anywhere',
	                }}
	              >
	                {workspaceActionMessage}
	              </div>
	            )}
	            {openWorkspaceError && (
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
                {openWorkspaceError}
              </div>
            )}
            {!providerSupported && (
              <div
                className="mb-2 text-xs"
                style={{
                  border: '1px solid color-mix(in srgb, var(--yellow) 45%, var(--border))',
                  borderRadius: 6,
                  padding: '8px 10px',
                  background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))',
                  color: 'var(--text)',
                  overflowWrap: 'anywhere',
                }}
              >
                {unsupportedProviderMessage}
              </div>
            )}
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
            {claudePlanPrompt !== null && (
              <div
                className="mb-2 text-xs"
                style={{
                  border: '1px solid color-mix(in srgb, var(--accent) 38%, var(--border))',
                  borderRadius: 6,
                  padding: '8px 10px',
                  background: 'color-mix(in srgb, var(--accent) 9%, var(--surface))',
                  color: 'var(--text)',
                }}
              >
                <div className="flex flex-wrap items-center gap-2">
                  <span className="min-w-0 flex-1" style={{ color: 'var(--text-2)' }}>
                    Claude Plan mode is read-only. Choose how to proceed with this prompt.
                  </span>
                  <button
                    type="button"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('acceptEdits')}
                    className="px-2 py-1"
                    style={{
                      border: '1px solid color-mix(in srgb, var(--green) 48%, var(--border))',
                      borderRadius: 6,
                      background: 'color-mix(in srgb, var(--green) 16%, var(--surface))',
                      color: 'var(--text)',
                      opacity: submitting ? 0.6 : 1,
                    }}
                  >
                    Accept edits and proceed
                  </button>
                  <button
                    type="button"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('default')}
                    className="px-2 py-1"
                    style={{
                      border: '1px solid var(--border)',
                      borderRadius: 6,
                      background: 'var(--bg)',
                      color: 'var(--text-2)',
                      opacity: submitting ? 0.6 : 1,
                    }}
                  >
                    Review each edit
                  </button>
                  <button
                    type="button"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('plan')}
                    className="px-2 py-1"
                    style={{
                      border: '1px solid var(--border)',
                      borderRadius: 6,
                      background: 'var(--bg)',
                      color: 'var(--text-2)',
                      opacity: submitting ? 0.6 : 1,
                    }}
                  >
                    Keep planning
                  </button>
                </div>
              </div>
            )}
            <div
              onDragOver={(event) => event.preventDefault()}
              onDrop={onComposerDrop}
              style={{
                border: '1px solid var(--border-bright)',
                borderRadius: 9,
                background: 'var(--bg)',
                overflow: 'visible',
              }}
            >
            {(markdownStoreAttachments.length > 0 || composerAttachments.length > 0) && (
              <div className="flex flex-wrap gap-2 px-3 pt-3">
                {markdownStoreAttachments.map((entry) => (
                  <span
                    key={entry.filePath}
                    className="inline-flex items-center gap-2 max-w-full text-[11px]"
                    title={entry.filePath}
                    style={{
                      border: '1px solid var(--border)',
                      borderRadius: 999,
                      background: 'var(--surface-up)',
                      color: 'var(--text-2)',
                      padding: '3px 7px',
                    }}
                  >
                    <span className="truncate max-w-[260px]">{entry.title || entry.filePath.split(/[\\/]/).pop()}</span>
                    <button
                      type="button"
                      onClick={() => detachMarkdownStoreEntry(session.id, entry.filePath)}
                      title="Remove Markdown Store attachment"
                      style={{ color: 'var(--text-3)' }}
                    >
                      x
                    </button>
                  </span>
                ))}
                {composerAttachments.map((attachment) => (
                  <span
                    key={attachment.id}
                    className="inline-flex items-center gap-2 max-w-full text-[11px]"
                    title={attachment.error || attachmentLabel(attachment)}
                    style={{
                      border: `1px solid ${attachment.status === 'failed' ? 'color-mix(in srgb, var(--red) 55%, var(--border))' : 'var(--border)'}`,
                      borderRadius: 999,
                      background: attachment.status === 'failed' ? 'color-mix(in srgb, var(--red) 10%, var(--surface))' : 'var(--surface-up)',
                      color: attachment.status === 'failed' ? 'var(--red)' : 'var(--text-2)',
                      padding: '3px 7px',
                    }}
                  >
                    <span className="truncate max-w-[260px]">{attachment.name}</span>
                    <span style={{ color: 'var(--text-3)' }}>
                      {attachment.status === 'staging' ? 'copying' : attachment.type === 'directory' ? 'dir' : attachment.type}
                    </span>
                    <button
                      type="button"
                      onClick={() => setComposerAttachments((current) => current.filter((item) => item.id !== attachment.id))}
                      title="Remove attachment"
                      style={{ color: 'var(--text-3)' }}
                    >
                      x
                    </button>
                  </span>
                ))}
              </div>
            )}
            {attachmentError && (
              <div className="px-3 pt-2 text-[11px]" style={{ color: 'var(--red)' }}>
                {attachmentError}
              </div>
            )}
            <textarea
              value={draft}
              onChange={(event) => setDraft(event.target.value)}
              onKeyDown={onComposerKeyDown}
              onPaste={onComposerPaste}
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
            <input
              ref={fileInputRef}
              type="file"
              multiple
              className="hidden"
              onChange={(event) => {
                const files = Array.from(event.currentTarget.files ?? [])
                event.currentTarget.value = ''
                void stageFiles(files)
              }}
            />
            <ComposerToolbar
              acp={acp}
              provider={currentProvider}
              providers={providers}
              providerId={currentProviderId}
              providerName={currentProviderName}
              canSend={canSend}
              modelId={currentModelId}
              session={session}
              reasoningEffort={session.reasoningEffort ?? ''}
              serviceTier={session.serviceTier ?? ''}
              approvalModeId={currentModeId}
              autoApproveCommands={session.autoApproveCommands ?? false}
              memoryEnabled={session.memoryEnabled ?? true}
              canChangeProvider={canChangeProvider}
              providerOpen={providerOpen}
              modelOpen={modelOpen}
              reasoningOpen={reasoningOpen}
              speedOpen={speedOpen}
              settingsOpen={settingsOpen}
              providerMenuRef={providerMenuRef}
              modelMenuRef={modelMenuRef}
              reasoningMenuRef={reasoningMenuRef}
              speedMenuRef={speedMenuRef}
              settingsMenuRef={settingsMenuRef}
              onToggleProvider={() => {
                setProviderOpen((value) => !value)
                setModelOpen(false)
                setReasoningOpen(false)
                setSpeedOpen(false)
                setSettingsOpen(false)
              }}
              onToggleModel={() => {
                if (runtimeBlocksControlChanges) return
                setModelOpen((value) => !value)
                setProviderOpen(false)
                setReasoningOpen(false)
                setSpeedOpen(false)
                setSettingsOpen(false)
              }}
              onToggleReasoning={() => {
                if (runtimeBlocksControlChanges) return
                setReasoningOpen((value) => !value)
                setProviderOpen(false)
                setModelOpen(false)
                setSpeedOpen(false)
                setSettingsOpen(false)
              }}
              onToggleSpeed={() => {
                if (runtimeBlocksControlChanges) return
                setSpeedOpen((value) => !value)
                setProviderOpen(false)
                setModelOpen(false)
                setReasoningOpen(false)
                setSettingsOpen(false)
              }}
              onToggleSettings={() => {
                setSettingsOpen((value) => !value)
                setProviderOpen(false)
                setModelOpen(false)
                setReasoningOpen(false)
                setSpeedOpen(false)
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
              onSelectReasoning={(reasoningEffort) => {
                setReasoningOpen(false)
                void setSessionCodexOptions(session.id, { reasoningEffort, serviceTier: session.serviceTier ?? '' })
              }}
              onSelectSpeed={(serviceTier) => {
                setSpeedOpen(false)
                void setSessionCodexOptions(session.id, { reasoningEffort: session.reasoningEffort ?? '', serviceTier })
              }}
              onTogglePlan={() => {
                const nextMode = currentModeId === 'plan' ? 'default' : 'plan'
                void setSessionApprovalMode(session.id, nextMode)
              }}
              onToggleAcceptEdits={() => {
                const nextMode = currentModeId === 'acceptEdits' ? 'default' : 'acceptEdits'
                void setSessionApprovalMode(session.id, nextMode)
              }}
              onToggleYolo={() => {
                void setSessionAutoApproveCommands(session.id, !(session.autoApproveCommands ?? false))
              }}
              onToggleMemory={() => {
                void setSessionMemoryEnabled(session.id, !(session.memoryEnabled ?? true))
              }}
              goalArmed={goalArmNextMessage}
              goalActive={Boolean(activeGoal)}
              goalPaused={Boolean(goalPaused)}
              defaultGoalTokenBudget={defaultGoalTokenBudget}
              onToggleGoal={handleToggleGoal}
              onSetDefaultGoalTokenBudget={(value) => setDefaultGoalTokenBudget(session.id, value)}
              onStopRuntime={() => void stopAcpSession(session.id)}
              onAttachFile={() => fileInputRef.current?.click()}
              onOpenMarkdownStore={() => void openMarkdownStore()}
            />
            </div>
          </div>
        </form>
      </div>
    </div>
  )
}
