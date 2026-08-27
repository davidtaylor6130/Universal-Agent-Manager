import { ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, RefObject, type ReactNode, memo, useCallback, useEffect, useId, useMemo, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useShallow } from 'zustand/react/shallow'
import { Session } from '../../types/session'
import { MarkdownContent } from '../markdown/Markdown'
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
  type DictationPushMessage,
  type VcsChangedFile,
  type VcsCommitStatus,
  type VcsType,
  type UamAgentCycleShortcut,
  type UamAgentSummary,
} from '../../store/useAppStore'
import type { Attachment, Message, MessageBlock } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { Goal, GoalStatus } from '../../types/goal'
import { GoalBanner } from '../shared/GoalBanner'
import { copyTextToClipboard } from '../../utils/copySelection'
import {
  DEFAULT_PROVIDER_ID,
  fallbackProviderForId,
  isClaudeProvider,
  isCodexProvider,
  isCopilotProvider,
  isOpenCodeProvider,
  providerCapabilities,
  providerRuntimeKindLabel,
  providerShortName,
} from '../../utils/providerMetadata'
import {
  type ModelOption,
  buildCodexReasoningOptions,
  buildCodexSpeedOptions,
  CODEX_SPEED_INHERIT_ID,
  buildModelOptions,
  FRIENDLY_MODEL_LABELS,
  labeledOption,
  modelOptionFor,
  providerRuntimeLabel,
  selectedRuntimeModel,
} from '../chat/modelOptions'
import {
  AcpErrorDetails,
  buildAcpErrorCopyText,
  CopyTextButton,
  diagnosticTail,
  formatDiagnosticLine,
  roleAccent,
  roleLabel,
  statusColor,
  statusLabel,
  toolDisplayKind,
  toolDisplayTitle,
} from '../chat/StatusHelpers'
import {
  isCancelPermissionOption,
  MessageFrame,
  normalizePermissionOptions,
  PermissionInlineCard,
  SubAgentRunningPanel,
  ToolCallInlineRows,
  ToolCallModal,
  UserInputInlineCard,
} from '../chat/ToolCallViews'
import {
  AttachmentList,
  GoalReviewBlock,
  PersistedMessageContent,
  PlanBlock,
  ThinkingBlock,
  TurnTimelineContent,
  attachmentLabel,
  goalReviewForMessage,
} from '../chat/MessageBlocks'
import {
  acpRuntimeBlocksControlChanges,
  PERMISSION_MODES,
  type ComposerIconName,
  ComposerIcon,
  ComposerToolbar,
  permissionModeIcon,
  permissionModeForTier,
  providerConfigVariantOptions,
  type DictationState,
} from '../chat/Composer'
import { Notice, ViewportMenu, type NoticeTone } from '../ui'
import { ArrowDown, Brain, BookOpen, ChevronRight, CornerUpRight, Cpu, FileText, Paperclip, Shield, Target, X } from 'lucide-react'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'
import { Button, IconButton } from '../ui'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { preferredBranch, setPreferredBranch } from '../../utils/branchPreferenceStorage'
import { replaceSlashAction, slashActionToken } from '../../utils/slashActionToken'
import { readChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'

interface ChatViewProps {
  session: Session
  accentColor?: string
  onOpenTerminalFallback?: () => void
}

type SlashCommand = {
  id: string
  label: string
  hint: string
  icon: ReactNode
  run: () => void
  groupEntries?: SlashCommand[]
}

type ComposerNoticeTone = NoticeTone
type PermissionChangeResult = { ok: boolean; cancelled?: boolean; error?: string }

function normalizePermissionChangeResult(result: boolean | PermissionChangeResult): PermissionChangeResult {
  return typeof result === 'boolean' ? { ok: result } : result
}

const INITIAL_RENDERED_MESSAGES = 200
const EMPTY_GOALS: Goal[] = []
const RENDERED_MESSAGE_BATCH_SIZE = 100
const SCROLL_NEAR_BOTTOM_THRESHOLD = 100
const STEERING_TIMEOUT_MS = 5000

export function uamAgentDisplayName(id: string) {
  if (id === 'build') return 'Build'
  if (id === 'plan') return 'Plan'
  return id.split('-').filter(Boolean).map((part) => part[0].toUpperCase() + part.slice(1)).join(' ')
}

export function buildUamAgentCycle(agents: UamAgentSummary[], favoriteIds: string[]) {
  const selectable = new Set(agents.map((agent) => agent.id))
  const cycle = ['build', 'plan']
  for (const rawId of favoriteIds) {
    const id = rawId.trim().toLowerCase()
    if (selectable.has(id) && !cycle.includes(id)) cycle.push(id)
  }
  return cycle
}

export function matchesUamAgentCycleShortcut(
  shortcut: UamAgentCycleShortcut,
  event: Pick<KeyboardEvent<HTMLTextAreaElement>, 'key' | 'shiftKey' | 'ctrlKey' | 'altKey' | 'metaKey'>,
) {
  if (shortcut === 'disabled' || event.key !== 'Tab' || !event.shiftKey) return false
  return event.ctrlKey === (shortcut === 'control+shift+tab') &&
    event.altKey === (shortcut === 'alt+shift+tab') &&
    event.metaKey === (shortcut === 'meta+shift+tab')
}

interface SelectedToolCallRef {
  id: string
  messageId?: string
}

const PLAN_APPROVE_PROMPT = 'Proceed with the plan.'
const PLAN_DENY_PROMPT = 'Do not proceed with this plan. Please revise it before making changes.'

type LocalAttachmentStatus = 'ready' | 'staging' | 'failed'
type WorkspaceFeedback = { message: string; tone: 'success' | 'warning' | 'error' }
interface LocalAttachment extends Attachment {
  status: LocalAttachmentStatus
  error?: string
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

function joinedDictationText(base: string, finalText: string, interimText: string) {
  return [base.trimEnd(), finalText.trim(), interimText.trim()].filter(Boolean).join(' ')
}

function slashName(value: string, fallback = 'skill') {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || fallback
}

function skillCommandName(entry: { commandName?: string; title: string }) {
  return entry.commandName || slashName(entry.title)
}

function fileUriToPath(uri: string): string {
  if (!uri.startsWith('file://')) return uri
  try {
    return decodeURIComponent(new URL(uri).pathname)
  } catch {
    return uri.replace(/^file:\/\//, '')
  }
}

function lastMessageIndexWithRole(messages: Message[], role: Message['role']) {
  for (let index = messages.length - 1; index >= 0; index -= 1) {
    if (messages[index].role === role) return index
  }
  return -1
}

function setRepositoryReview(sessionId: string, review: VcsCommitStatus | null) {
  useAppStore.setState((state) => {
    const repositoryReviewBySessionId = { ...state.repositoryReviewBySessionId }
    if (review) repositoryReviewBySessionId[sessionId] = review
    else delete repositoryReviewBySessionId[sessionId]
    return { repositoryReviewBySessionId }
  })
}

function RepositoryDiffDialog({
  chatId,
  file,
  vcsType,
  comparisonRef,
  getDiff,
  onClose,
}: {
  chatId: string
  file: VcsChangedFile
  vcsType: VcsType
  comparisonRef?: string
  getDiff: (chatId: string, path: string, vcsType: VcsType, comparisonRef?: string) => Promise<string>
  onClose: () => void
}) {
  const dialogRef = useRef<HTMLElement>(null)
  const [diff, setDiff] = useState<string | null>(file.binary ? '' : null)
  const [error, setError] = useState('')

  useEffect(() => {
    const previouslyFocused = document.activeElement as HTMLElement | null
    dialogRef.current?.focus()
    return () => previouslyFocused?.focus?.()
  }, [])

  useEffect(() => {
    const closeOnEscape = (event: globalThis.KeyboardEvent) => {
      if (event.key === 'Escape') onClose()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [onClose])

  useEffect(() => {
    if (file.binary) return
    let cancelled = false
    setDiff(null)
    setError('')
    void (comparisonRef ? getDiff(chatId, file.path, vcsType, comparisonRef) : getDiff(chatId, file.path, vcsType))
      .then((nextDiff) => {
        if (!cancelled) setDiff(nextDiff)
      })
      .catch((reason) => {
        if (!cancelled) setError(reason instanceof Error ? reason.message : 'Failed to load this diff.')
      })
    return () => { cancelled = true }
  }, [chatId, comparisonRef, file.binary, file.path, getDiff, vcsType])

  const lines = diff?.split('\n') ?? []

  return createPortal(
    <div
      className="fixed inset-0 z-[1000] flex items-center justify-center p-2 sm:p-4"
      style={{ background: 'rgba(0, 0, 0, 0.48)', backdropFilter: 'blur(3px)' }}
      onMouseDown={onClose}
    >
      <section
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={`Changes in ${file.path}`}
        tabIndex={-1}
        className="flex h-[calc(100dvh-1rem)] w-full max-w-5xl flex-col overflow-hidden rounded-lg sm:h-auto sm:max-h-[88vh]"
        style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="flex min-h-12 items-center gap-3 px-3 sm:px-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <FileText size={17} aria-hidden className="shrink-0" style={{ color: 'var(--accent)' }} />
          <div className="min-w-0 flex-1">
            <div className="truncate text-sm font-semibold" title={file.path} style={{ color: 'var(--text)' }}>{file.path}</div>
            <div className="flex gap-3 font-mono text-[11px]">
              <span style={{ color: 'var(--text-3)' }}>{file.status.trim() || 'M'}</span>
              {!file.binary && <><span style={{ color: 'var(--green)' }}>+{file.additions}</span><span style={{ color: 'var(--red)' }}>-{file.deletions}</span></>}
            </div>
          </div>
          {diff && <CopyTextButton text={diff} label="Copy diff" title="Copy file diff" />}
          <IconButton icon={<X size={16} />} label="Close file changes" onClick={onClose} />
        </header>
        <div className="min-h-0 flex-1 overflow-auto" style={{ background: 'var(--bg)' }}>
          {file.binary ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>Binary changes cannot be previewed.</div>
          ) : error ? (
            <div role="alert" className="m-4 rounded-md p-3 text-sm" style={{ border: '1px solid var(--red)', color: 'var(--red)' }}>{error}</div>
          ) : diff === null ? (
            <div role="status" className="p-6 text-center text-sm" style={{ color: 'var(--text-3)' }}>Loading file changes…</div>
          ) : diff.length === 0 ? (
            <div className="p-6 text-center text-sm" style={{ color: 'var(--text-2)' }}>No textual diff is available for this file.</div>
          ) : (
            <pre className="min-w-full w-max py-2 font-mono text-[11px] leading-5 sm:text-xs" aria-label={`Unified diff for ${file.path}`}>
              {lines.map((line, index) => {
                const added = line.startsWith('+') && !line.startsWith('+++')
                const removed = line.startsWith('-') && !line.startsWith('---')
                const hunk = line.startsWith('@@')
                const header = line.startsWith('diff ') || line.startsWith('index ') || line.startsWith('---') || line.startsWith('+++')
                return (
                  <span
                    key={`${index}-${line}`}
                    className="block min-h-5 whitespace-pre px-3 sm:px-4"
                    style={{
                      color: added ? 'var(--green)' : removed ? 'var(--red)' : hunk ? 'var(--accent)' : header ? 'var(--text-2)' : 'var(--text)',
                      background: added
                        ? 'color-mix(in srgb, var(--green) 9%, transparent)'
                        : removed
                          ? 'color-mix(in srgb, var(--red) 9%, transparent)'
                          : hunk
                            ? 'var(--accent-dim)'
                            : 'transparent',
                    }}
                  >
                    {line || ' '}
                  </span>
                )
              })}
            </pre>
          )}
        </div>
      </section>
    </div>,
    document.body
  )
}

function ProviderHandoffDialog({
  sourceName,
  targetName,
  blockedReason,
  onCancel,
  onConfirm,
}: {
  sourceName: string
  targetName: string
  blockedReason?: string
  onCancel: () => void
  onConfirm: () => Promise<boolean>
}) {
  const dialogRef = useRef<HTMLElement>(null)
  const [switching, setSwitching] = useState(false)
  const [error, setError] = useState('')

  useEffect(() => {
    const previouslyFocused = document.activeElement as HTMLElement | null
    dialogRef.current?.focus()
    return () => previouslyFocused?.focus?.()
  }, [])

  useEffect(() => {
    const closeOnEscape = (event: globalThis.KeyboardEvent) => {
      if (event.key === 'Escape' && !switching) onCancel()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [onCancel, switching])

  const confirm = async () => {
    if (switching || blockedReason) return
    setSwitching(true)
    setError('')
    if (await onConfirm()) return
    setSwitching(false)
    setError('The provider could not be changed. Finish any active work and try again.')
  }

  return createPortal(
    <div
      className="fixed inset-0 z-[1000] flex items-center justify-center p-4"
      style={{ background: 'rgba(0, 0, 0, 0.52)', backdropFilter: 'blur(3px)' }}
      onMouseDown={() => { if (!switching) onCancel() }}
    >
      <section
        ref={dialogRef}
        role="alertdialog"
        aria-modal="true"
        aria-label={`Switch from ${sourceName} to ${targetName}`}
        aria-describedby="provider-handoff-description"
        tabIndex={-1}
        className="w-full max-w-lg overflow-hidden rounded-xl"
        style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}
        onMouseDown={(event) => event.stopPropagation()}
      >
        <header className="flex items-center gap-3 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <CornerUpRight size={18} aria-hidden style={{ color: 'var(--accent)' }} />
          <div>
            <h2 className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Switch this chat to {targetName}?</h2>
            <p className="mt-0.5 text-xs" style={{ color: 'var(--text-3)' }}>{sourceName} → {targetName}</p>
          </div>
        </header>
        <div id="provider-handoff-description" className="space-y-4 px-5 py-4 text-sm" style={{ color: 'var(--text-2)' }}>
          <p>UAM keeps the recorded conversation, but provider-native state cannot move between providers.</p>
          <div>
            <div className="mb-1 text-xs font-semibold uppercase tracking-wide" style={{ color: 'var(--green)' }}>Kept</div>
            <ul className="list-disc space-y-1 pl-5">
              <li>Chat messages and recorded tool results</li>
              <li>Chat title and workspace</li>
              <li>Selected UAM agent and goals</li>
            </ul>
          </div>
          <div>
            <div className="mb-1 text-xs font-semibold uppercase tracking-wide" style={{ color: 'var(--amber)' }}>Reset for {targetName}</div>
            <ul className="list-disc space-y-1 pl-5">
              <li>Native session and provider-specific live tool state</li>
              <li>Model, reasoning or speed, permission, safety, and memory defaults</li>
            </ul>
          </div>
          <p className="text-xs" style={{ color: 'var(--text-3)' }}>Anything the current provider did not record in UAM will not transfer.</p>
          {blockedReason && <div role="alert" className="rounded-md p-2 text-xs" style={{ border: '1px solid var(--amber)', color: 'var(--amber)' }}>{blockedReason}</div>}
          {error && <div role="alert" className="rounded-md p-2 text-xs" style={{ border: '1px solid var(--red)', color: 'var(--red)' }}>{error}</div>}
        </div>
        <footer className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
          <Button type="button" size="sm" variant="secondary" disabled={switching} onClick={onCancel}>Cancel</Button>
          <Button type="button" size="sm" loading={switching} disabled={Boolean(blockedReason)} onClick={() => void confirm()}>Switch to {targetName}</Button>
        </footer>
      </section>
    </div>,
    document.body
  )
}

export const ChatView = memo(function ChatView({ session, accentColor, onOpenTerminalFallback }: ChatViewProps) {
  const slashListboxId = useId()
  const workspaceMenuId = useId()
  const [draft, setDraft] = useState(() => readChatComposerDraft(session.id).text)
  const [composerSelection, setComposerSelection] = useState({ start: 0, end: 0 })
  const [submitting, setSubmitting] = useState(false)
  const [steering, setSteering] = useState(false)
  const [dictationState, setDictationState] = useState<DictationState>('idle')
  const [dictationElapsedSeconds, setDictationElapsedSeconds] = useState(0)
  const [dictationError, setDictationError] = useState('')
  const [selectedToolCallRef, setSelectedToolCallRef] = useState<SelectedToolCallRef | null>(null)
  const [modelOpen, setModelOpen] = useState(false)
  const [workspaceMenuOpen, setWorkspaceMenuOpen] = useState(false)
  const [claudePlanPrompt, setClaudePlanPrompt] = useState<string | null>(null)
  const [workspaceFeedback, setWorkspaceFeedback] = useState<WorkspaceFeedback | null>(null)
  const [workspaceActionBusy, setWorkspaceActionBusy] = useState(false)
  const [goalError, setGoalError] = useState('')
  const [goalSubmitting, setGoalSubmitting] = useState(false)
  const [goalArmNextMessage, setGoalArmNextMessage] = useState(false)
  const [slashIndex, setSlashIndex] = useState(0)
  const [slashMessage, setSlashMessage] = useState('')
  const [slashGroup, setSlashGroup] = useState('')
  const [slashGroupIndex, setSlashGroupIndex] = useState(0)
  const [permissionMenuOpen, setPermissionMenuOpen] = useState(false)
  const [confirmYolo, setConfirmYolo] = useState(false)
  const [memoryChipExplicit, setMemoryChipExplicit] = useState(false)
  const [composerAttachments, setComposerAttachments] = useState<LocalAttachment[]>(() =>
    readChatComposerDraft(session.id).attachments.map((attachment) => ({ ...attachment, status: 'ready' }))
  )
  const [attachmentError, setAttachmentError] = useState('')
  const [dismissedAcpErrorKey, setDismissedAcpErrorKey] = useState('')
  const [editingMessageIndex, setEditingMessageIndex] = useState<number | null>(null)
  const [editingMessageText, setEditingMessageText] = useState('')
  const [branchingMessageIndex, setBranchingMessageIndex] = useState<number | null>(null)
  const [messageBranchError, setMessageBranchError] = useState('')
  const [rollbackConfirmation, setRollbackConfirmation] = useState<{ messageIndex: number; diff: string } | null>(null)
  const [renderedMessageCount, setRenderedMessageCount] = useState(INITIAL_RENDERED_MESSAGES)
  const [selectedRepositoryFile, setSelectedRepositoryFile] = useState<VcsChangedFile | null>(null)
  const [providerHandoffTargetId, setProviderHandoffTargetId] = useState('')
  const steerTurnSerialRef = useRef(0)
  const steeringTimeoutRef = useRef<number | null>(null)
  const appModalOpen = useAppStore((s) =>
    providerHandoffTargetId !== '' ||
    s.isNewChatModalOpen ||
    s.isSettingsOpen ||
    s.memoryLibraryScope !== null ||
    s.isMemoryScanModalOpen ||
    s.isMarkdownStoreOpen
  )

  useEffect(() => {
    return () => {
      if (steeringTimeoutRef.current !== null) {
        window.clearTimeout(steeringTimeoutRef.current)
      }
    }
  }, [])

  useEffect(() => setMemoryChipExplicit(false), [session.id])
  useEffect(() => {
    writeChatComposerDraft(session.id, {
      text: draft,
      attachments: composerAttachments
        .filter((attachment) => attachment.status === 'ready')
        .map(({ status: _status, error: _error, ...attachment }) => attachment),
    })
  }, [composerAttachments, draft, session.id])
  const slashGroupButtonRefs = useRef<Record<string, HTMLSpanElement | null>>({})
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const workingDisplayMode = useAppStore((s) => s.workingDisplayMode)
  const cli = useAppStore((s) => s.cliBindingBySessionId[session.id])
  const providers = useAppStore((s) => s.providers)
  const cliVersionManager = useAppStore((s) => s.cliVersionManager)
  const stageChatAttachments = useAppStore((s) => s.stageChatAttachments)
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  const getVcsCommitStatus = useAppStore((s) => s.getVcsCommitStatus)
  const getVcsFileDiff = useAppStore((s) => s.getVcsFileDiff)
  const repositoryChanges = useAppStore((s) => s.repositoryReviewBySessionId[session.id] ?? null)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const removeQueuedAcpPrompt = useAppStore((s) => s.removeQueuedAcpPrompt)
  const steerQueuedAcpPrompt = useAppStore((s) => s.steerQueuedAcpPrompt)
  const branchFromMessage = useAppStore((s) => s.branchFromMessage)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const branchRootChatId = session.branchRootChatId || session.parentChatId || session.id
  const branchSessions = useAppStore(useShallow((s) => s.sessions
    .filter((candidate) => (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === branchRootChatId)
    .sort((a, b) => a.createdAt.getTime() - b.createdAt.getTime())))
  useEffect(() => {
    for (let index = 0; index < messages.length; index += 1) {
      if (messages[index]?.role !== 'user') continue
      const parentId = session.parentChatId && session.branchFromMessageIndex === index ? session.parentChatId : session.id
      const candidates = branchSessions.filter((candidate) =>
        candidate.id === parentId || (candidate.parentChatId === parentId && candidate.branchFromMessageIndex === index)
      )
      if (candidates.length < 2) continue
      const preferred = preferredBranch(parentId, index, candidates.map((candidate) => candidate.id))
      if (preferred && preferred !== session.id) {
        setActiveSession(preferred)
        return
      }
    }
  }, [branchSessions, messages, session.branchFromMessageIndex, session.id, session.parentChatId, setActiveSession])
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const stopAcpSession = useAppStore((s) => s.stopAcpSession)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const resolveAcpUserInput = useAppStore((s) => s.resolveAcpUserInput)
  const setSessionProvider = useAppStore((s) => s.setSessionProvider)
  const setSessionModel = useAppStore((s) => s.setSessionModel)
	const setSessionReviewerModel = useAppStore((s) => s.setSessionReviewerModel)
	const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const setSessionCodexOptions = useAppStore((s) => s.setSessionCodexOptions)
  const setAcpConfigOption = useAppStore((s) => s.setAcpConfigOption)
  const setSessionApprovalMode = useAppStore((s) => s.setSessionApprovalMode)
  const setSessionUamAgent = useAppStore((s) => s.setSessionUamAgent)
  const refreshUamAgents = useAppStore((s) => s.refreshUamAgents)
  const catalogUamAgents = useAppStore(useShallow((s) => s.uamAgentsBySessionId[session.id] ?? []))
  const favoriteUamAgentIds = useAppStore(useShallow((s) => s.favoriteUamAgentIds))
  const uamAgentCycleShortcut = useAppStore((s) => s.uamAgentCycleShortcut)
  const setSessionCommandSafetyTier = useAppStore((s) => s.setSessionCommandSafetyTier)
  const setSessionMemoryLevel = useAppStore((s) => s.setSessionMemoryLevel)
  const setSessionSmallModelMode = useAppStore((s) => s.setSessionSmallModelMode)
  const configuredApprovalMode = useAppStore((s) => s.sessions.find((candidate) => candidate.id === session.id)?.approvalMode)
  const configuredUamAgentId = useAppStore((s) => s.sessions.find((candidate) => candidate.id === session.id)?.uamAgentId)

  useEffect(() => {
    void refreshUamAgents(session.id)
  }, [refreshUamAgents, session.id, workspaceDirectory])
  const openSessionWorkspace = useAppStore((s) => s.openSessionWorkspace)
  const openSessionWorkspaceEditor = useAppStore((s) => s.openSessionWorkspaceEditor)
  const openSessionTerminal = useAppStore((s) => s.openSessionTerminal)
  const refreshCliProviderVersion = useAppStore((s) => s.refreshCliProviderVersion)
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const openSubAgentSession = useAppStore((s) => s.openSubAgentSession)
  const createChatWorktree = useAppStore((s) => s.createChatWorktree)
  const discardChatWorktreeChanges = useAppStore((s) => s.discardChatWorktreeChanges)
  const portChatWorktreeChanges = useAppStore((s) => s.portChatWorktreeChanges)
  const previewChatTurnRollback = useAppStore((s) => s.previewChatTurnRollback)
  const rollbackChatTurn = useAppStore((s) => s.rollbackChatTurn)
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const markdownStoreEntries = useAppStore(useShallow((s) => s.markdownStoreEntries))
  const defaultMemoryLevel = useAppStore((s) => s.memoryLevelDefault)
  const refreshMarkdownStore = useAppStore((s) => s.refreshMarkdownStore)
  const attachMarkdownStoreEntry = useAppStore((s) => s.attachMarkdownStoreEntry)
  const markdownStoreAttachments = useAppStore(useShallow((s) => s.markdownStoreAttachedBySessionId[session.id] ?? []))
  const detachMarkdownStoreEntry = useAppStore((s) => s.detachMarkdownStoreEntry)
  const goals = useAppStore((s) => s.goalsByChatId[session.id] ?? EMPTY_GOALS)
  const activeGoalId = useAppStore((s) => s.activeGoalIdByChatId[session.id] ?? null)
  const setGoalStore = useAppStore((s) => s.setGoal)
  const featurePreference = useAppStore((s) => s.providerChatDefaults[session.providerId || acp?.providerId || DEFAULT_PROVIDER_ID]?.featurePreference === 'provider' ? 'provider' : 'uam')
  const defaultReviewerModelId = useAppStore((s) => s.providerChatDefaults[session.providerId || acp?.providerId || DEFAULT_PROVIDER_ID]?.reviewerModelId ?? '')
  const updateGoalStatus = useAppStore((s) => s.updateGoalStatus)
  const updateGoalObjective = useAppStore((s) => s.updateGoalObjective)
  const removeGoal = useAppStore((s) => s.removeGoal)
  const resumeGoal = useAppStore((s) => s.resumeGoal)
  const defaultGoalTokenBudget = useAppStore((s) => s.defaultGoalTokenBudgetByChatId[session.id] ?? 0)
  const setDefaultGoalTokenBudget = useAppStore((s) => s.setDefaultGoalTokenBudget)
  const scrollRef = useRef<HTMLDivElement>(null)
  const bottomRef = useRef<HTMLDivElement>(null)
  const isNearBottomRef = useRef(true)
  const [showScrollToBottom, setShowScrollToBottom] = useState(false)
  const fileInputRef = useRef<HTMLInputElement>(null)
  const composerTextareaRef = useRef<HTMLTextAreaElement>(null)
  const modelMenuRef = useRef<HTMLDivElement>(null)
  const workspaceMenuRef = useRef<HTMLDivElement>(null)
  const dictationActiveRef = useRef(false)
  const dictationBaseDraftRef = useRef('')
  const dictationFinalTextRef = useRef('')
  const dictationInterimTextRef = useRef('')
  const dictationHadErrorRef = useRef(false)
  const dictationSubmitAfterStopRef = useRef(false)
  const submitDictatedPromptRef = useRef<(prompt: string) => void>(() => {})
  const submitInFlightRef = useRef(false)
  const currentSessionIdRef = useRef(session.id)

  useEffect(() => {
    currentSessionIdRef.current = session.id
    isNearBottomRef.current = true
    setShowScrollToBottom(false)
    submitInFlightRef.current = false
    setSubmitting(false)
    setSelectedRepositoryFile(null)
    setProviderHandoffTargetId('')
    setConfirmYolo(false)
    setRollbackConfirmation(null)
  }, [session.id])

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
  const earliestRenderedMessageIndex = Math.max(0, messages.length - renderedMessageCount)
  const latestUserMessageIndex = lastMessageIndexWithRole(messages, 'user')
  const latestAssistantMessageIndex = lastMessageIndexWithRole(messages, 'assistant')
  const turnAssistantMessageMatches =
    turnAssistantMessageIndex >= earliestRenderedMessageIndex &&
    turnAssistantMessageIndex === messages.length - 1 &&
    messages[turnAssistantMessageIndex]?.role === 'assistant'
  const turnUserMessageMatches =
    turnUserMessageIndex >= earliestRenderedMessageIndex &&
    turnUserMessageIndex === latestUserMessageIndex &&
    messages[turnUserMessageIndex]?.role === 'user' &&
    (turnUserMessageIndex === messages.length - 1 ||
      (turnAssistantMessageMatches && turnUserMessageIndex < turnAssistantMessageIndex))
  const completedTurnAssistantText = acp?.processing
    ? ''
    : turnEvents.reduce(
        (text, event) => event.type === 'assistant_text' ? text + event.text : text,
        ''
      ).trim()
  const completedFallbackAlreadyPersisted =
    !acp?.processing &&
    completedTurnAssistantText.length > 0 &&
    latestAssistantMessageIndex > latestUserMessageIndex &&
    messages[latestAssistantMessageIndex]?.content.trim() === completedTurnAssistantText
  const turnWorkedSeconds = acp?.processing
    ? acp.processingStartedAtMs ? (Date.now() - acp.processingStartedAtMs) / 1000 : undefined
    : turnAssistantMessageIndex >= 0
      ? (messages[turnAssistantMessageIndex]?.processingTimeMs ?? 0) / 1000
      : undefined
  const renderTimelineAfterUser =
    turnEvents.length > 0 &&
    turnUserMessageMatches &&
    (!turnAssistantMessageMatches || firstTurnEvent?.type !== 'assistant_text')
  const renderTimelineAtAssistant =
    turnEvents.length > 0 &&
    !renderTimelineAfterUser &&
    turnAssistantMessageMatches
  const visibleMessages = useMemo(
    () => messages.slice(earliestRenderedMessageIndex),
    [earliestRenderedMessageIndex, messages]
  )

  useEffect(() => {
    if (isNearBottomRef.current) {
      bottomRef.current?.scrollIntoView?.({ block: 'end' })
    }
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
    repositoryChanges?.changedFiles.length,
  ])

  const handleScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    const isNearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < SCROLL_NEAR_BOTTOM_THRESHOLD
    isNearBottomRef.current = isNearBottom
    setShowScrollToBottom(!isNearBottom)
  }, [])

  const scrollToBottom = useCallback(() => {
    isNearBottomRef.current = true
    setShowScrollToBottom(false)
    bottomRef.current?.scrollIntoView?.({ block: 'end', behavior: 'smooth' })
  }, [])

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
    setSteering(false)
    setAttachmentError('')
    setGoalError('')
    setWorkspaceFeedback(null)
    setWorkspaceActionBusy(false)
    setWorkspaceMenuOpen(false)
    setGoalArmNextMessage(false)
    setSlashMessage('')
    setPermissionMenuOpen(false)
    setEditingMessageIndex(null)
    setEditingMessageText('')
    setBranchingMessageIndex(null)
    setMessageBranchError('')
    setClaudePlanPrompt(null)
    setRenderedMessageCount(INITIAL_RENDERED_MESSAGES)
    setDictationState('idle')
    setDictationError('')
  }, [session.id])

  useEffect(() => {
    if (!acp?.lastError) setDismissedAcpErrorKey('')
  }, [acp?.lastError])

  useEffect(() => {
    if (dictationState !== 'listening') { setDictationElapsedSeconds(0); return }
    const startedAt = Date.now()
    const timer = window.setInterval(() => setDictationElapsedSeconds(Math.floor((Date.now() - startedAt) / 1000)), 250)
    return () => window.clearInterval(timer)
  }, [dictationState])

  useEffect(() => {
    if (session.workspaceIsolationKind !== 'gitWorktree') {
      setWorkspaceFeedback(null)
    }
  }, [session.workspaceIsolationKind])

  useEffect(() => {
    if (!steering) return
    const nextTurnStarted = (acp?.turnSerial ?? 0) > steerTurnSerialRef.current
    const queuedSteerStillPending = Boolean(acp?.queuedPrompts?.[0]?.prioritySteer)
    if (nextTurnStarted || (!acp?.processing && !queuedSteerStillPending)) {
      setSteering(false)
      if (steeringTimeoutRef.current !== null) {
        window.clearTimeout(steeringTimeoutRef.current)
        steeringTimeoutRef.current = null
      }
      return
    }
    if (steeringTimeoutRef.current !== null) {
      window.clearTimeout(steeringTimeoutRef.current)
    }
    steeringTimeoutRef.current = window.setTimeout(() => setSteering(false), STEERING_TIMEOUT_MS)
  }, [acp?.turnSerial, acp?.processing, acp?.queuedPrompts, steering])

  useEffect(() => {
    if (workspaceFeedback?.tone !== 'success') return
    const timeout = window.setTimeout(() => setWorkspaceFeedback(null), 5000)
    return () => window.clearTimeout(timeout)
  }, [workspaceFeedback])

  const runtimeBlocksControlChanges = acpRuntimeBlocksControlChanges(acp)

  useEffect(() => {
    if (runtimeBlocksControlChanges) {
      setModelOpen(false)
    }
  }, [runtimeBlocksControlChanges])

  useEffect(() => {
    const onMouseDown = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return

      if (modelOpen && modelMenuRef.current && !modelMenuRef.current.contains(target)) {
        setModelOpen(false)
      }

      if (workspaceMenuOpen && workspaceMenuRef.current && !workspaceMenuRef.current.contains(target)) {
        setWorkspaceMenuOpen(false)
      }
    }

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setModelOpen(false)
      setWorkspaceMenuOpen(false)
      setSelectedToolCallRef(null)
    }

    document.addEventListener('mousedown', onMouseDown)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onMouseDown)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [modelOpen, workspaceMenuOpen])

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
    const goalMatch = prompt.match(/^\/goal\s+(.+?)(?:\s+--budget\s+(\d+))?\s*$/s)
    if (!goalMatch) return false

    const objective = goalMatch[1].trim()
    const tokenBudget = goalMatch[2] ? parseInt(goalMatch[2], 10) : defaultGoalTokenBudget

    if (!objective) {
      setGoalError('Goal objective is required.')
      return true
    }

    setGoalSubmitting(true)
	const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
	const providerManaged = featurePreference === 'provider' && Boolean(nativeGoalCommand)
	const goalResult = await setGoalStore(session.id, objective, tokenBudget, providerManaged ? 'provider' : 'uam')
	const goalAttachments = composerAttachments
	  .filter((attachment) => attachment.status === 'ready')
	  .map(({ status, error, ...attachment }) => attachment)
	const sent = goalResult.ok ? await sendAcpPrompt(session.id, providerManaged ? `${nativeGoalCommand} ${objective}` : objective, goalAttachments) : false
    setGoalSubmitting(false)

	if (goalResult.ok && sent) {
      setDraft('')
	  setComposerAttachments([])
	  setAttachmentError('')
      setGoalError('')
    } else {
	  setGoalError(goalResult.ok ? 'Goal was created, but the first prompt failed to send.' : (goalResult.error || 'Failed to create goal.'))
    }
    return true
  }

  const submit = async (event?: FormEvent, promptOverride?: string, steerNow = false) => {
    event?.preventDefault()
    const prompt = (promptOverride ?? draft).trim()
    if (!providerSupported || session.importedReadOnly || !prompt || submitInFlightRef.current || goalSubmitting || composerAttachments.some((attachment) => attachment.status !== 'ready')) return
	if (dictationActiveRef.current && promptOverride === undefined) {
	  await stopDictation(true)
	  return
	}
	const readyAttachments = composerAttachments
	  .filter((attachment) => attachment.status === 'ready')
	  .map(({ status, error, ...attachment }) => attachment)

    // Handle /goal command
    if (prompt.startsWith('/goal ')) {
      const submittedSessionId = session.id
      submitInFlightRef.current = true
      setSubmitting(true)
      try {
        await submitGoal(prompt)
      } finally {
        submitInFlightRef.current = false
        if (currentSessionIdRef.current === submittedSessionId) setSubmitting(false)
      }
      return
    }
	const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
	if (nativeGoalCommand && (prompt === nativeGoalCommand || prompt.startsWith(`${nativeGoalCommand} `))) {
	  const objective = prompt.slice(nativeGoalCommand.length).trim()
	  if (!objective) {
		setGoalError('Goal objective is required.')
		return
	  }
	  const submittedSessionId = session.id
	  submitInFlightRef.current = true
	  setSubmitting(true)
	  setGoalSubmitting(true)
	  try {
		const goalResult = await setGoalStore(session.id, objective, defaultGoalTokenBudget, 'provider')
		const ok = goalResult.ok ? await sendAcpPrompt(session.id, prompt, readyAttachments) : false
		if (!ok) {
		  setGoalError(goalResult.ok ? 'Goal was created, but the provider command failed to send.' : (goalResult.error || 'Failed to create goal.'))
		  return
		}
		setGoalError('')
		setDraft('')
		setComposerAttachments([])
		setAttachmentError('')
	  } finally {
		setGoalSubmitting(false)
		submitInFlightRef.current = false
		if (currentSessionIdRef.current === submittedSessionId) setSubmitting(false)
	  }
	  return
	}

    if (goalArmNextMessage) {
      const submittedSessionId = session.id
      submitInFlightRef.current = true
      setSubmitting(true)
      setGoalSubmitting(true)
	  try {
		const providerGoalCommand = featurePreference === 'provider' ? currentProvider.nativeGoalCommand?.trim() ?? '' : ''
		const goalResult = await setGoalStore(session.id, prompt, defaultGoalTokenBudget, providerGoalCommand ? 'provider' : 'uam')
		if (!goalResult.ok) {
		  setGoalError(goalResult.error || 'Failed to create goal.')
		  return
		}
		const ok = steerNow
		  ? await sendAcpPrompt(session.id, providerGoalCommand ? `${providerGoalCommand} ${prompt}` : prompt, readyAttachments, true)
		  : await sendAcpPrompt(session.id, providerGoalCommand ? `${providerGoalCommand} ${prompt}` : prompt, readyAttachments)
		if (!ok) {
		  setGoalError('Goal was created, but the first prompt failed to send.')
		  return
		}
		setGoalError('')
		setGoalArmNextMessage(false)
		setDraft('')
		setComposerAttachments([])
		setAttachmentError('')
	  } finally {
		setGoalSubmitting(false)
		submitInFlightRef.current = false
		if (currentSessionIdRef.current === submittedSessionId) setSubmitting(false)
	  }
      return
    }

    if (!steerNow && isClaudeProvider(currentProvider, currentProviderId) && currentModeId === 'plan') {
      setClaudePlanPrompt(prompt)
      return
    }
    const submittedSessionId = session.id
    submitInFlightRef.current = true
    setSubmitting(true)
    if (steerNow) {
      if (steeringTimeoutRef.current !== null) {
        window.clearTimeout(steeringTimeoutRef.current)
        steeringTimeoutRef.current = null
      }
      steerTurnSerialRef.current = acp?.turnSerial ?? 0
      setSteering(true)
    }
    let ok = false
    try {
      ok = await (steerNow
        ? sendAcpPrompt(session.id, prompt, readyAttachments, true)
        : sendAcpPrompt(session.id, prompt, readyAttachments))
    } catch {
      ok = false
    }
    if (currentSessionIdRef.current === submittedSessionId) {
      submitInFlightRef.current = false
      setSubmitting(false)
    }
    if (!ok) {
      if (steeringTimeoutRef.current !== null) {
        window.clearTimeout(steeringTimeoutRef.current)
        steeringTimeoutRef.current = null
      }
      setSteering(false)
    }
    if (ok && currentSessionIdRef.current === submittedSessionId) {
      setDraft('')
      setComposerAttachments([])
      setAttachmentError('')
    }
  }


  const pendingPermission = acp?.pendingPermission
  const pendingUserInput = acp?.pendingUserInput
  const latestAssistantMessage = latestAssistantMessageIndex >= 0 ? messages[latestAssistantMessageIndex] : undefined
  const repositoryComparisonRef = session.workspaceIsolationKind === 'gitWorktree' ? session.workspaceBaseRef?.trim() : undefined
  const completedTurnKey = !acp?.processing && !cli?.processing && latestAssistantMessageIndex > latestUserMessageIndex
    ? `${latestAssistantMessage?.id ?? ''}:${turnSerial}:${cli?.terminalId ?? ''}`
    : ''
  useEffect(() => {
    let cancelled = false
    if (!completedTurnKey || !workspaceDirectory || !repositoryComparisonRef) return

    void getVcsCommitStatus(session.id, 'git', {
      includeLineStats: true,
      requestId: `chat-summary:${session.id}:${completedTurnKey}`,
      comparisonRef: repositoryComparisonRef,
    }).then((status) => {
      if (cancelled || !status?.available) return
      setRepositoryReview(session.id, status.changedFiles.length > 0 ? status : null)
    })
    return () => { cancelled = true }
  }, [completedTurnKey, getVcsCommitStatus, repositoryComparisonRef, session.id, workspaceDirectory])
  const isGitWorktree = session.workspaceIsolationKind === 'gitWorktree'
  const sourceWorkspaceDirectory = session.workspaceSourceDirectory?.trim() || (!isGitWorktree ? workspaceDirectory : '')
  const workspaceActionsDisabled = workspaceActionBusy || Boolean(
    acp?.processing ||
    acp?.pendingPermission ||
    acp?.pendingUserInput ||
    (acp?.queuedPrompts?.length ?? 0) > 0 ||
    cli?.running
  )
  const openWorkspace = async () => {
    if (!workspaceDirectory) return
    setWorkspaceFeedback(null)
    const ok = await openSessionWorkspace(session.id)
    setWorkspaceFeedback(ok
      ? { message: 'Opened workspace directory.', tone: 'success' }
      : { message: 'Failed to open workspace directory.', tone: 'error' })
  }
  const openWorkspaceEditor = async () => {
    if (!workspaceDirectory) return
    setWorkspaceFeedback(null)
    const ok = await openSessionWorkspaceEditor(session.id)
    setWorkspaceFeedback(ok
      ? { message: 'Opened workspace editor.', tone: 'success' }
      : { message: 'Failed to open workspace editor.', tone: 'error' })
  }
  const openWorkspaceTerminal = async () => {
    if (!workspaceDirectory) return
    setWorkspaceFeedback(null)
    const ok = await openSessionTerminal(session.id)
    setWorkspaceFeedback(ok
      ? { message: 'Opened terminal at workspace.', tone: 'success' }
      : { message: 'Failed to open terminal.', tone: 'error' })
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
    setWorkspaceFeedback(null)
    setWorkspaceActionBusy(true)
    const result =
      action === 'create'
        ? await createChatWorktree(session.id)
        : action === 'discard'
          ? await discardChatWorktreeChanges(session.id)
          : await portChatWorktreeChanges(session.id)
    setWorkspaceActionBusy(false)
    if (result.ok) {
      setRepositoryReview(session.id, null)
      const warning = result.status?.warning
      setWorkspaceFeedback({
        message: warning || result.message || (action === 'port' ? 'Applied chat changes and returned to the source workspace.' : 'Workspace action complete.'),
        tone: warning ? 'warning' : 'success',
      })
    } else {
      setWorkspaceFeedback({ message: result.status?.error || result.message || 'Workspace action failed.', tone: 'error' })
    }
  }
  const rollbackLatestTurn = async () => {
    if (workspaceActionsDisabled || latestAssistantMessageIndex < 0) return
    setWorkspaceFeedback(null)
    setWorkspaceActionBusy(true)
    const preview = await previewChatTurnRollback(session.id, latestAssistantMessageIndex)
    if (!preview) {
      setWorkspaceActionBusy(false)
      setWorkspaceFeedback({ message: 'This turn can no longer be rolled back safely.', tone: 'error' })
      return
    }
    setWorkspaceActionBusy(false)
    setRollbackConfirmation({
      messageIndex: latestAssistantMessageIndex,
      diff: preview.diff || 'This checkpoint contains repository changes.',
    })
  }
  const confirmLatestTurnRollback = async () => {
    if (!rollbackConfirmation) return
    const { messageIndex } = rollbackConfirmation
    setRollbackConfirmation(null)
    setWorkspaceActionBusy(true)
    const result = await rollbackChatTurn(session.id, messageIndex)
    setWorkspaceActionBusy(false)
    if (!result) {
      setWorkspaceFeedback({ message: 'Rollback failed. Any later or uncommitted work was left untouched.', tone: 'error' })
      return
    }
    setRepositoryReview(session.id, null)
    setWorkspaceFeedback({ message: result.message || 'Rolled back the isolated workspace.', tone: 'success' })
  }
  const currentProviderId = session.providerId || acp?.providerId || DEFAULT_PROVIDER_ID
  const providerSupported = providers.some((candidate) => candidate.id === currentProviderId)
  const currentProvider = useMemo<Provider>(
    () =>
      providers.find((candidate) => candidate.id === currentProviderId) ?? fallbackProviderForId(currentProviderId),
    [currentProviderId, providers]
  )
  const providerAcp = acp?.providerId === currentProviderId ? acp : undefined
  const providerVariants = useMemo(
    () => providerConfigVariantOptions(providerAcp, currentProviderId),
    [providerAcp, currentProviderId]
  )
  const currentProviderName = providerShortName(currentProvider, currentProviderId)
  const providerHandoffTarget = providers.find((candidate) => candidate.id === providerHandoffTargetId)
  const providerHandoffTargetName = providerHandoffTarget
    ? providerShortName(providerHandoffTarget, providerHandoffTarget.id)
    : ''
  const providerHandoffReadiness = providerHandoffTarget
    ? cliVersionManager.providers.find((candidate) => candidate.providerId === providerHandoffTarget.id)
    : undefined
  const providerHandoffBlocked = providerHandoffReadiness?.status === 'checking' ||
    providerHandoffReadiness?.status === 'installing' ||
    providerHandoffReadiness?.status === 'known-incompatible' ||
    providerHandoffReadiness?.status === 'unavailable'
  const providerHandoffBlockedReason = providerHandoffBlocked
    ? providerHandoffReadiness?.message || `${providerHandoffTargetName} cannot start structured chats right now.`
    : ''
  const errorProviderId = acp?.providerId || currentProviderId
  const errorProvider = providers.find((candidate) => candidate.id === errorProviderId) ?? fallbackProviderForId(errorProviderId)
  const currentErrorTitle = `${providerShortName(errorProvider, errorProviderId)} ${providerRuntimeLabel(errorProvider, acp)} error`
  const currentAcpErrorKey = acp?.lastError ? `${session.id}:${acp.lastError}` : ''
  const slashNoticeTone: ComposerNoticeTone = /failed|unsupported/i.test(slashMessage)
    ? 'error'
    : /working|unavailable|provider-managed/i.test(slashMessage)
      ? 'warning'
      : 'success'
  const unsupportedProviderMessage = providerSupported
    ? ''
    : `${currentProviderName} is not supported in this build. Switch this chat to Gemini CLI to continue.`
  const canChangeProvider = !acp?.processing && !(acp?.queuedPrompts?.length) && !acp?.pendingPermission && !acp?.pendingUserInput && !cli?.processing && cli?.turnState !== 'busy'
  const createMessageBranch = async (messageIndex: number, content?: string) => {
    setBranchingMessageIndex(messageIndex)
    setMessageBranchError('')
    const branchId = await branchFromMessage(session.id, messageIndex, content)
    setBranchingMessageIndex(null)
    if (!branchId) {
      setMessageBranchError('Could not create the message branch. Wait for the current turn to finish and try again.')
      return
    }
    setEditingMessageIndex(null)
    setEditingMessageText('')
  }
  const dictationActive = dictationState !== 'idle'
  const canSend = useMemo(
    () => providerSupported && !session.importedReadOnly && draft.trim().length > 0 && !submitting && !goalSubmitting && !composerAttachments.some((attachment) => attachment.status !== 'ready'),
    [providerSupported, session.importedReadOnly, draft, submitting, goalSubmitting, composerAttachments]
  )
  const dictationAvailable = isCefContext()

  submitDictatedPromptRef.current = (prompt) => {
    void submit(undefined, prompt)
  }

  const startDictation = async () => {
    if (!dictationAvailable || dictationActiveRef.current) return
    dictationActiveRef.current = true
    dictationBaseDraftRef.current = draft
    dictationFinalTextRef.current = ''
    dictationInterimTextRef.current = ''
    dictationHadErrorRef.current = false
    dictationSubmitAfterStopRef.current = false
    setDictationError('')
    setDictationState('starting')

    const response = await sendToCEF<{ started: boolean }>({
      action: 'startDictation',
      payload: { locale: navigator.language || '' },
    })
    if (!dictationActiveRef.current) return
    if (!response.ok) {
      dictationActiveRef.current = false
      setDictationState('idle')
      setDictationError(response.error || 'Failed to start dictation.')
      return
    }
    setDictationState('listening')
  }

  async function stopDictation(submitAfterStop = false) {
    if (!dictationActiveRef.current) return
	dictationSubmitAfterStopRef.current = submitAfterStop
    setDictationState('stopping')
    const response = await sendToCEF<{ stopped: boolean }>({ action: 'stopDictation' })
    if (!response.ok && dictationActiveRef.current) {
	  dictationSubmitAfterStopRef.current = false
      setDictationState('listening')
      setDictationError(response.error || 'Failed to stop dictation.')
    }
  }

  function dismissDictationError() {
    const wasActive = dictationActiveRef.current
    dictationActiveRef.current = false
    dictationSubmitAfterStopRef.current = false
    setDictationState('idle')
    setDictationError('')
    if (wasActive) void sendToCEF({ action: 'stopDictation' })
    window.setTimeout(() => composerTextareaRef.current?.focus(), 0)
  }

  useEffect(() => {
    const onDictation = (event: Event) => {
      const message = (event as CustomEvent<DictationPushMessage>).detail
      if (!dictationActiveRef.current || !message || message.type !== 'dictation') return

      if (message.event === 'interim') {
        dictationInterimTextRef.current = message.text
        setDraft(joinedDictationText(dictationBaseDraftRef.current, dictationFinalTextRef.current, message.text))
        return
      }
      if (message.event === 'final') {
        dictationFinalTextRef.current = joinedDictationText(dictationFinalTextRef.current, message.text, '')
        dictationInterimTextRef.current = ''
        setDraft(joinedDictationText(dictationBaseDraftRef.current, dictationFinalTextRef.current, ''))
        return
      }
      if (message.event === 'error') {
        dictationHadErrorRef.current = true
        dictationActiveRef.current = false
        dictationSubmitAfterStopRef.current = false
        setDictationError(message.message || 'Dictation failed.')
        setDictationState('idle')
        return
      }

      const prompt = joinedDictationText(
        dictationBaseDraftRef.current,
        dictationFinalTextRef.current,
        dictationInterimTextRef.current
      )
	  const submitAfterStop = dictationSubmitAfterStopRef.current
	  dictationSubmitAfterStopRef.current = false
      dictationActiveRef.current = false
      setDictationState('idle')
      setDraft(prompt)
      if (submitAfterStop && !dictationHadErrorRef.current && prompt.trim()) {
        submitDictatedPromptRef.current(prompt)
      }
    }

    window.addEventListener('uam-dictation', onDictation)
    return () => {
      window.removeEventListener('uam-dictation', onDictation)
      if (dictationActiveRef.current) {
        dictationActiveRef.current = false
        void sendToCEF({ action: 'stopDictation' })
      }
    }
  }, [session.id])
  // The persisted chat setting is the choice for the next turn. ACP can still
  // report the previous idle model until that turn begins.
  const currentModelId = session.modelId || providerAcp?.currentModelId || ''
  const currentReviewerModelId = session.reviewerModelId || defaultReviewerModelId || currentModelId
  const showUnresolvedDefaultModel = !session.modelId && !providerAcp?.currentModelId
  const currentModel = modelOptionFor(buildModelOptions(providerAcp, currentModelId, currentProvider, currentProviderId, showUnresolvedDefaultModel), currentModelId)
  const currentProviderCapabilities = providerCapabilities(currentProviderId, currentProvider)
  const runtimeSupportsReasoning = (selectedRuntimeModel(providerAcp, currentModel.id)?.supportedReasoningEfforts?.length ?? 0) > 0
  const reasoningOptions = currentProviderCapabilities.hasReasoningEffort || runtimeSupportsReasoning
    ? buildCodexReasoningOptions(
        providerAcp,
        currentModel.id,
        session.reasoningEffort ?? '',
        isCopilotProvider(currentProvider, currentProviderId)
          ? currentProviderCapabilities.reasoningOptions.map((option) => option.id)
          : undefined
      )
    : []
  const serviceTierExplicit = session.serviceTierExplicit ?? (session.serviceTier ?? '') !== ''
  const speedOptions = currentProviderCapabilities.hasServiceTier
    ? buildCodexSpeedOptions(providerAcp, currentModel.id, session.serviceTier ?? '')
    : []
  const currentModeId = configuredApprovalMode || session.approvalMode || providerAcp?.currentModeId || 'default'
  const providerModes = useMemo(() => {
    const offered = providerAcp?.availableModes.filter((mode) => mode.id === 'default' || mode.id === 'plan') ?? []
    return offered.length > 0 ? offered : [
      { id: 'default', name: 'Default', description: 'Use the provider default behaviour.' },
      { id: 'plan', name: 'Plan', description: 'Research and plan before implementation.' },
    ]
  }, [providerAcp?.availableModes, currentProvider, currentProviderId])
  const selectableUamAgents = useMemo<UamAgentSummary[]>(() => {
    const byId = new Map(catalogUamAgents.map((agent) => [agent.id, agent]))
    if (!byId.has('build')) byId.set('build', { id: 'build', description: 'Implement changes while obeying the current UAM permission policy.', builtIn: true })
    if (!byId.has('plan')) byId.set('plan', { id: 'plan', description: 'Inspect and plan under a hard read-only workspace ceiling.', builtIn: true })
    return [byId.get('build')!, byId.get('plan')!, ...Array.from(byId.values())
      .filter((agent) => agent.id !== 'build' && agent.id !== 'plan')
      .sort((left, right) => left.id.localeCompare(right.id))]
  }, [catalogUamAgents])
  const uamAgents = useMemo(() => selectableUamAgents.map((agent) => ({
    id: agent.id,
    name: uamAgentDisplayName(agent.id),
    description: agent.description,
  })), [selectableUamAgents])
  const uamAgentCycle = useMemo(
    () => buildUamAgentCycle(selectableUamAgents, favoriteUamAgentIds),
    [favoriteUamAgentIds, selectableUamAgents],
  )
  const selectedUamAgentId = configuredUamAgentId || session.uamAgentId || 'build'
  const permissionsManagedByUam = currentProviderCapabilities.structuredPermissionControl === 'uam'
  useEffect(() => {
    if (!isClaudeProvider(currentProvider, currentProviderId) || currentModeId !== 'plan') {
      setClaudePlanPrompt(null)
    }
    setSlashMessage('')
  }, [currentModeId, currentProvider, currentProviderCapabilities.structuredPermissionControl, currentProviderId])
  const permissionControlsDisabled = runtimeBlocksControlChanges && !providerAcp?.pendingPermission
  const permissionModes = useMemo(
    () => permissionsManagedByUam
      ? PERMISSION_MODES.filter((mode) => mode.id !== 'acceptEdits' || currentProviderCapabilities.hasAcceptEditsMode)
      : [{ id: 'provider', name: 'Provider managed', description: 'Respond to permission prompts in the provider interface.' }],
    [currentProviderCapabilities.hasAcceptEditsMode, permissionsManagedByUam]
  )
  const selectedPermissionModeId = permissionsManagedByUam ? permissionModeForTier(session.commandSafetyTier ?? 'off') : 'provider'
  const applyPermissionMode = async (modeId: string): Promise<PermissionChangeResult> => {
    if (!providerSupported || !currentProvider.supportsStructured || !permissionsManagedByUam || permissionControlsDisabled) return { ok: false }
    if (modeId === 'default') return normalizePermissionChangeResult(await setSessionCommandSafetyTier(session.id, 'off'))
    if (modeId === 'acceptEdits') return normalizePermissionChangeResult(await setSessionCommandSafetyTier(session.id, 'acceptEdits'))
    if (modeId === 'yolo') return normalizePermissionChangeResult(await setSessionCommandSafetyTier(session.id, 'yolo'))
    if (modeId === 'aiReview') return normalizePermissionChangeResult(await setSessionCommandSafetyTier(session.id, 'aiReview'))
    return { ok: false }
  }
  const requestPermissionMode = async (modeId: string): Promise<PermissionChangeResult> => {
    if (modeId === 'yolo' && selectedPermissionModeId !== 'yolo') {
      setConfirmYolo(true)
      return { ok: false, cancelled: true }
    }
    return applyPermissionMode(modeId)
  }
  const showPermissionResult = (result: PermissionChangeResult, fallback: string) => {
    setSlashMessage(result.ok || result.cancelled ? '' : result.error || fallback)
  }
  const selectPermissionMode = async (modeId: string) => {
    const mode = permissionModes.find((candidate) => candidate.id === modeId)
    if (!mode) return
    const result = await requestPermissionMode(mode.id)
    showPermissionResult(result, `Failed to change permission mode to ${mode.name}.`)
  }
  const runPermissionCommand = async (rawMode?: string) => {
    setSlashIndex(0)
    setPermissionMenuOpen(false)
    if (!providerSupported || !currentProvider.supportsStructured || !permissionsManagedByUam) {
      setSlashMessage(`${currentProviderName} permissions are provider-managed; respond in the provider interface.`)
      return
    }
    if (permissionControlsDisabled) {
      setSlashMessage(`${currentProviderName} is still working; permissions can be changed when the current operation stops.`)
      return
    }

    if (!rawMode) {
      setPermissionMenuOpen(true)
      return
    }

    const normalized = rawMode.trim().toLowerCase()
    const requested = permissionModes.find((mode) =>
      mode.id.toLowerCase() === normalized || mode.name.toLowerCase().replace(/\s+/g, '-') === normalized
    )
    if (!requested) {
      setSlashMessage(`Unsupported permission mode "${rawMode}". Supported: ${permissionModes.map((mode) => mode.id).join(', ')}.`)
      return
    }

    const result = await requestPermissionMode(requested.id)
    showPermissionResult(result, `Failed to change permission mode to ${requested.name}.`)
  }
  const runCodexOptionCommand = async (kind: 'reasoning' | 'speed', rawValue?: string) => {
    setSlashIndex(0)
    if (runtimeBlocksControlChanges) {
      setSlashMessage(`${currentProviderName} is still working.`)
      return
    }
    const options = kind === 'reasoning' ? reasoningOptions : speedOptions
    const label = kind === 'reasoning' ? 'Reasoning' : 'Speed'
    if (options.length === 0) {
      setSlashMessage(`${label} changes are unavailable for ${currentProviderName}.`)
      return
    }
    if (!rawValue) {
      setDraft(`/${kind} `)
      return
    }
    const normalized = rawValue.trim().toLowerCase()
    const requested = options.find((option) =>
      (option.id || 'default').toLowerCase() === normalized ||
      option.label.toLowerCase().replace(/\s+/g, '-') === normalized
    )
    if (!requested) {
      setSlashMessage(`Unsupported ${kind} "${rawValue}". Supported: ${options.map((option) => option.id || 'default').join(', ')}.`)
      return
    }
    const changed = kind === 'reasoning'
      ? await setSessionCodexOptions(session.id, { reasoningEffort: requested.id })
      : await setSessionCodexOptions(session.id, { serviceTier: requested.id === CODEX_SPEED_INHERIT_ID ? '' : requested.id, serviceTierExplicit: requested.id !== CODEX_SPEED_INHERIT_ID })
    setSlashMessage(changed ? `${label} changed to ${requested.label}.` : `Failed to change ${kind} to ${requested.label}.`)
  }
  const runVariantCommand = async (configId: string, value: string, label: string) => {
    setSlashIndex(0)
    if (runtimeBlocksControlChanges) {
      setSlashMessage(`${currentProviderName} is still working.`)
      return
    }
    const changed = await setAcpConfigOption(session.id, configId, value)
    setSlashMessage(changed ? `${label} requested.` : `Failed to change ${label}.`)
  }
  const latestPlanMessageIndex = messages.reduce((latest, message, index) => {
    const hasPlan = message.role === 'assistant' && (Boolean(message.planSummary?.trim()) || (message.planEntries?.length ?? 0) > 0)
    return hasPlan ? index : latest
  }, -1)
  const latestPlanHasLaterUser =
    latestPlanMessageIndex >= 0 && messages.slice(latestPlanMessageIndex + 1).some((message) => message.role === 'user')
  const canShowPlanActions = isCodexProvider(currentProvider, currentProviderId) && currentModeId === 'plan' && latestPlanMessageIndex >= 0 && !latestPlanHasLaterUser
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
  const displayedGoalWorkerLabel = session.smallModelMode && displayedGoal?.workerModelId
    ? modelOptionFor(buildModelOptions(providerAcp, displayedGoal.workerModelId, currentProvider, currentProviderId), displayedGoal.workerModelId).label
    : ''
  const displayedGoalReviewerLabel = session.smallModelMode && displayedGoal?.reviewerModelId
    ? modelOptionFor(buildModelOptions(providerAcp, displayedGoal.reviewerModelId, currentProvider, currentProviderId), displayedGoal.reviewerModelId).label
    : ''
  const goalPaused = displayedGoal?.status === 'paused'
  const runGoalMutation = async (
    mutation: () => Promise<{ ok: boolean; error?: string }>,
    fallbackError: string,
  ) => {
    setGoalError('')
    const result = await mutation()
    if (!result.ok) setGoalError(result.error || fallbackError)
    return result.ok
  }
  const handleCompleteGoal = () => {
    if (displayedGoal) {
      void runGoalMutation(
        () => updateGoalStatus(session.id, displayedGoal.id, 'complete'),
        'Failed to complete goal.',
      )
    }
  }
  const handleResumeGoal = async () => {
    if (!displayedGoal || goalSubmitting) return
    setGoalError('')
    setGoalSubmitting(true)
    const resumed = await resumeGoal(session.id, displayedGoal.id)
    setGoalSubmitting(false)
    if (!resumed.ok) setGoalError(resumed.error || 'Failed to resume goal.')
  }
  const handlePauseGoal = () => {
    if (displayedGoal) {
      void runGoalMutation(
        () => updateGoalStatus(session.id, displayedGoal.id, 'paused'),
        'Failed to pause goal.',
      )
    }
  }
  const handleRemoveGoal = () => {
    if (displayedGoal) {
      void runGoalMutation(
        () => removeGoal(session.id, displayedGoal.id),
        'Failed to remove goal.',
      )
    }
  }
  const handleEditGoal = async (objective: string) => {
    if (!displayedGoal || displayedGoal.executionOwner === 'provider' || displayedGoal.status === 'complete') return false
    return runGoalMutation(
      () => updateGoalObjective(session.id, displayedGoal.id, objective),
      'Failed to update goal.',
    )
  }
  const handleToggleGoal = () => {
    if (activeGoal) {
      void runGoalMutation(
        () => updateGoalStatus(session.id, activeGoal.id, 'paused'),
        'Failed to pause goal.',
      )
      return
    }
    if (displayedGoal && (displayedGoal.status === 'paused' || displayedGoal.status === 'blocked')) {
      void handleResumeGoal()
      return
    }
    setGoalArmNextMessage((value) => !value)
    setGoalError('')
  }

  // Slash command palette: typing "/" at the start of an empty-ish draft opens
  // a traversable menu of UAM actions (Codex-style), filtered by what follows.
  const currentMemoryLevel: MemoryLevel = session.memoryLevel ?? ((session.memoryEnabled ?? true) ? 'strict' : 'off')
  const runMemoryCommand = (level: MemoryLevel) => {
    setSlashIndex(0)
    setMemoryChipExplicit(true)
    void setSessionMemoryLevel(session.id, level)
  }
  const slashCommands = useMemo<SlashCommand[]>(
    () => {
      const favorites = markdownStoreEntries.filter((entry) => entry.favorite)
      const skillCommand = (entry: typeof favorites[number], label = skillCommandName(entry)): SlashCommand => ({
        id: `md:${entry.id}`,
        label: `/${label}`,
        hint: `${entry.title}${entry.sourceProvider ? ` · ${entry.sourceProvider}` : ''}`,
        icon: <FileText size={15} />,
        run: () => attachMarkdownStoreEntry(session.id, entry),
      })
      const groupName = (entry: typeof favorites[number]) => entry.group?.split('/').map((part) => part.trim()).filter(Boolean).join('/') || ''
      const groupedSkills = (entries: typeof favorites, parent = ''): SlashCommand[] => {
        const direct = entries.filter((entry) => groupName(entry) === parent)
        const children = Array.from(new Set(entries.flatMap((entry) => {
          const parts = groupName(entry).split('/').filter(Boolean)
          const parentParts = parent ? parent.split('/') : []
          return parts.length > parentParts.length && parts.slice(0, parentParts.length).join('/') === parent
            ? [parts.slice(0, parentParts.length + 1).join('/')]
            : []
        })))
        const groupSlug = slashName(parent.split('/').pop() || '', 'skills')
        return [
          ...direct.map((entry) => {
            const stem = skillCommandName(entry).replace(/-[0-9a-f]{8}$/i, '')
            return skillCommand(entry, parent && stem.startsWith(`${groupSlug}-`) ? stem.slice(groupSlug.length + 1) : stem)
          }),
          ...children.map((path) => {
            const name = path.split('/').pop()!
            const descendants = entries.filter((entry) => groupName(entry) === path || groupName(entry).startsWith(`${path}/`))
            return {
              id: `md-group:${path}`,
              label: `/${slashName(name, 'skills')}`,
              hint: `${descendants.length} skill${descendants.length === 1 ? '' : 's'}`,
              icon: <BookOpen size={15} />,
              run: () => setSlashGroup(path),
              groupEntries: groupedSkills(descendants, path),
            }
          }),
        ]
      }

      const commands: SlashCommand[] = [
        { id: 'model', label: '/model', hint: 'Change the model', icon: <Cpu size={15} />, run: () => setModelOpen(true) },
        ...(reasoningOptions.length > 0 ? [{ id: 'reasoning', label: '/reasoning', hint: 'Choose Codex reasoning', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('reasoning') }] : []),
        ...(speedOptions.length > 0 ? [{ id: 'speed', label: '/speed', hint: 'Choose Codex speed', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('speed') }] : []),
        ...(providerVariants.length > 0 ? [{ id: 'variants', label: '/variants', hint: 'Choose OpenCode model variants', icon: <Cpu size={15} />, run: () => setDraft('/variants ') }] : []),
        { id: 'permission', label: '/permission', hint: 'Choose the permission mode', icon: <Shield size={15} />, run: () => void runPermissionCommand() },
        { id: 'goal', label: '/goal', hint: 'Use the next message as a goal', icon: <Target size={15} />, run: handleToggleGoal },
        {
          id: 'memory',
          label: '/memory',
          hint: `Choose memory level · current: ${currentMemoryLevel}`,
          icon: <Brain size={15} />,
          run: () => setDraft('/memory '),
        },
        { id: 'attach', label: '/attach', hint: 'Attach files', icon: <Paperclip size={15} />, run: () => fileInputRef.current?.click() },
        { id: 'skills', label: '/skills', hint: 'Open Skills', icon: <BookOpen size={15} />, run: () => void openMarkdownStore() },
        ...groupedSkills(favorites),
      ]
      const usedLabels = new Set(commands.map((command) => command.label.toLowerCase()))
      for (const command of providerAcp?.availableCommands ?? []) {
        const label = `/${command.name}`
        if (usedLabels.has(label.toLowerCase())) continue
        usedLabels.add(label.toLowerCase())
        commands.push({
          id: `acp:${command.name}`,
          label,
          hint: [command.description, command.inputHint].filter(Boolean).join(' · ') || 'Provider command',
          icon: <FileText size={15} />,
          run: () => setDraft(`${label}${command.inputHint ? ' ' : ''}`),
        })
      }
      return commands
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [session.id, currentMemoryLevel, session.commandSafetyTier, session.reasoningEffort, session.serviceTier, session.serviceTierExplicit, markdownStoreEntries, providerAcp?.availableCommands, currentModeId, permissionModes, providerSupported, currentProviderName, reasoningOptions, speedOptions, providerVariants]
  )
  const activeSlashToken = slashActionToken(draft, composerSelection.start, composerSelection.end)
  const slashSubPalette = Boolean(activeSlashToken && activeSlashToken.queryStart > activeSlashToken.commandStart + 1)
  const slashQuery = activeSlashToken && !slashSubPalette ? activeSlashToken.query : null
  const permissionModeQuery = permissionMenuOpen ? '' : slashSubPalette && activeSlashToken?.command === 'permission' ? activeSlashToken.query : undefined
  const memoryLevelQuery = slashSubPalette && activeSlashToken?.command === 'memory' ? activeSlashToken.query : undefined
  const codexOptionKind = slashSubPalette && (activeSlashToken?.command === 'reasoning' || activeSlashToken?.command === 'speed') ? activeSlashToken.command : undefined
  const codexOptionQuery = codexOptionKind ? activeSlashToken?.query : undefined
  const codexOptionQueryOptions = codexOptionKind === 'reasoning' ? reasoningOptions : codexOptionKind === 'speed' ? speedOptions : []
  const variantQuery = slashSubPalette && activeSlashToken?.command === 'variants' ? activeSlashToken.query : undefined
  const variantChoices = providerVariants.flatMap((option) => option.options.map((choice) => ({ option, choice })))
  const slashMatches: SlashCommand[] = permissionModeQuery !== undefined
    ? permissionModes
        .filter((mode) => mode.id.toLowerCase().startsWith(permissionModeQuery) || mode.name.toLowerCase().replace(/\s+/g, '-').startsWith(permissionModeQuery))
        .map((mode) => ({
          id: `permission:${mode.id}`,
          label: mode.name,
          hint: `${mode.id === selectedPermissionModeId ? 'Current · ' : ''}${mode.description}`,
          icon: permissionModeIcon(mode.id, 15),
          run: () => void runPermissionCommand(mode.id),
        }))
    : memoryLevelQuery !== undefined
      ? MEMORY_LEVEL_OPTIONS
          .filter((option) => option.id.startsWith(memoryLevelQuery) || option.label.toLowerCase().startsWith(memoryLevelQuery))
          .map((option) => ({
            id: `memory:${option.id}`,
            label: option.label,
            hint: `${option.id === currentMemoryLevel ? 'Current · ' : ''}${option.detail}`,
            icon: <Brain size={15} />,
            run: () => runMemoryCommand(option.id),
          }))
    : codexOptionKind !== undefined
      ? codexOptionQueryOptions
          .filter((option) =>
            (option.id || 'default').toLowerCase().startsWith(codexOptionQuery ?? '') ||
            option.label.toLowerCase().replace(/\s+/g, '-').startsWith(codexOptionQuery ?? '')
          )
          .map((option) => ({
            id: `${codexOptionKind}:${option.id || 'default'}`,
            label: option.label,
            hint: `${option.id === (codexOptionKind === 'reasoning' ? session.reasoningEffort ?? '' : serviceTierExplicit ? session.serviceTier ?? '' : CODEX_SPEED_INHERIT_ID) ? 'Current · ' : ''}${option.detail}`,
            icon: <Cpu size={15} />,
            run: () => void runCodexOptionCommand(codexOptionKind, option.id || 'default'),
          }))
    : variantQuery !== undefined
      ? variantChoices
          .filter(({ option, choice }) => [option.id, option.name, choice.value, choice.name].some((part) => part.toLowerCase().includes(variantQuery)))
          .map(({ option, choice }) => ({
            id: `variant:${option.id}:${choice.value}`,
            label: providerVariants.length > 1 ? `${option.name || option.id}: ${choice.name || choice.value}` : choice.name || choice.value,
            hint: `${choice.value === option.currentValue ? 'Current · ' : ''}${choice.description || option.description}`,
            icon: <Cpu size={15} />,
            run: () => void runVariantCommand(option.id, choice.value, `${option.name || option.id} ${choice.name || choice.value}`),
          }))
    : slashQuery !== null
      ? slashCommands.filter((command) => command.label.slice(1).toLowerCase().startsWith(slashQuery))
      : []
  const slashOpen = !appModalOpen && slashMatches.length > 0 && (slashQuery !== null || permissionModeQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined || variantQuery !== undefined)
  const slashPaletteVisible = slashQuery !== null || permissionModeQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined || variantQuery !== undefined
  const activeSlashOptionId = slashOpen ? `${slashListboxId}-option-${Math.min(slashIndex, slashMatches.length - 1)}` : undefined
  useEffect(() => {
    if (slashPaletteVisible && markdownStoreEntries.length === 0) {
      void refreshMarkdownStore()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [slashPaletteVisible])
  const runSlashCommand = (command: SlashCommand) => {
    if (command.groupEntries) {
      setSlashGroup(command.id.slice('md-group:'.length))
      setSlashGroupIndex(0)
      return
    }
    if (activeSlashToken) {
      const opensSubPalette = !slashSubPalette && ['permission', 'memory', 'reasoning', 'speed', 'variants'].includes(command.id)
      const insertsProviderCommand = !slashSubPalette && command.id.startsWith('acp:')
      if (opensSubPalette || insertsProviderCommand) {
        const replacement = `${command.label}${opensSubPalette || command.id.startsWith('acp:') ? ' ' : ''}`
        setDraft(replaceSlashAction(draft, activeSlashToken, replacement))
        setComposerSelection({ start: activeSlashToken.commandStart + replacement.length, end: activeSlashToken.commandStart + replacement.length })
        setSlashIndex(0)
        return
      }
      setDraft(replaceSlashAction(draft, activeSlashToken, '', slashSubPalette))
    }
    setSlashIndex(0)
    setSlashGroup('')
    setPermissionMenuOpen(false)
    command.run()
  }
  const activeSlashGroups = slashOpen && slashGroup ? slashGroup.split('/').reduce<SlashCommand[]>((groups, _, index, parts) => {
    const path = parts.slice(0, index + 1).join('/')
    const entries = groups[groups.length - 1]?.groupEntries ?? slashCommands
    const group = entries.find((command) => command.id === `md-group:${path}`)
    if (group) groups.push(group)
    return groups
  }, []) : []
  const activeSlashGroup = activeSlashGroups[activeSlashGroups.length - 1]

  const onComposerKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
	const nativeEvent = event.nativeEvent
	if (matchesUamAgentCycleShortcut(uamAgentCycleShortcut, event) &&
		!event.repeat && !nativeEvent.isComposing && nativeEvent.keyCode !== 229 &&
		!appModalOpen && !selectedToolCallRef && !modelOpen && !workspaceMenuOpen &&
		!slashPaletteVisible && !permissionMenuOpen && !slashGroup) {
	  event.preventDefault()
	  const currentIndex = uamAgentCycle.indexOf(selectedUamAgentId)
	  void setSessionUamAgent(session.id, uamAgentCycle[currentIndex < 0 ? 0 : (currentIndex + 1) % uamAgentCycle.length])
	  return
	}
    if (activeSlashGroup?.groupEntries) {
      if (event.key === 'ArrowDown') {
        event.preventDefault()
        setSlashGroupIndex((i) => (i + 1) % activeSlashGroup.groupEntries!.length)
        return
      }
      if (event.key === 'ArrowUp') {
        event.preventDefault()
        setSlashGroupIndex((i) => (i - 1 + activeSlashGroup.groupEntries!.length) % activeSlashGroup.groupEntries!.length)
        return
      }
      if (event.key === 'Enter' || event.key === 'Tab') {
        event.preventDefault()
        runSlashCommand(activeSlashGroup.groupEntries[Math.min(slashGroupIndex, activeSlashGroup.groupEntries.length - 1)])
        return
      }
      if (event.key === 'ArrowLeft' || event.key === 'Escape') {
        event.preventDefault()
        setSlashGroup(event.key === 'ArrowLeft' ? slashGroup.split('/').slice(0, -1).join('/') : '')
        setSlashGroupIndex(0)
        return
      }
    }
    if (slashOpen) {
      if (event.key === 'ArrowDown') {
        event.preventDefault()
        setSlashIndex((i) => (i + 1) % slashMatches.length)
        return
      }
      if (event.key === 'ArrowUp') {
        event.preventDefault()
        setSlashIndex((i) => (i - 1 + slashMatches.length) % slashMatches.length)
        return
      }
      if (event.key === 'ArrowRight') {
        const command = slashMatches[Math.min(slashIndex, slashMatches.length - 1)]
        if (command.groupEntries) {
          event.preventDefault()
          runSlashCommand(command)
          return
        }
      }
      if (event.key === 'Enter' || event.key === 'Tab') {
        event.preventDefault()
        runSlashCommand(slashMatches[Math.min(slashIndex, slashMatches.length - 1)])
        return
      }
      if (event.key === 'Escape') {
        event.preventDefault()
        if (activeSlashToken) setDraft(replaceSlashAction(draft, activeSlashToken, '', slashSubPalette))
        setPermissionMenuOpen(false)
        return
      }
    }
    const permissionCommand = /^\/permission(?:\s+(\S+))?\s*$/i.exec(draft)
    if (event.key === 'Enter' && !event.shiftKey && permissionCommand) {
      event.preventDefault()
      setDraft('')
      void runPermissionCommand(permissionCommand[1])
      return
    }
    const codexOptionCommand = /^\/(reasoning|speed)(?:\s+(\S+))?\s*$/i.exec(draft)
    if (event.key === 'Enter' && !event.shiftKey && codexOptionCommand) {
      event.preventDefault()
      setDraft('')
      void runCodexOptionCommand(codexOptionCommand[1].toLowerCase() as 'reasoning' | 'speed', codexOptionCommand[2])
      return
    }
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      void submit()
    }
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
    const modeOk = nextModeId === 'plan' ? true : await setSessionApprovalMode(session.id, 'default')
    if (!modeOk) {
      setSlashMessage('Failed to change provider mode.')
      setSubmitting(false)
      return
    }
    const permissionResult = nextModeId === 'acceptEdits'
      ? normalizePermissionChangeResult(await setSessionCommandSafetyTier(session.id, 'acceptEdits'))
      : { ok: true }
    if (!permissionResult.ok) {
      if (!permissionResult.cancelled) setSlashMessage(permissionResult.error || 'Failed to change permission mode to Accept Edits.')
      setSubmitting(false)
      return
    }
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
    setSubmitting(false)
  }

  return (
    <div className="relative h-full flex overflow-hidden" style={{ background: 'var(--bg)' }}>
      {providerHandoffTarget && (
        <ProviderHandoffDialog
          sourceName={currentProviderName}
          targetName={providerHandoffTargetName}
          blockedReason={providerHandoffBlockedReason}
          onCancel={() => setProviderHandoffTargetId('')}
          onConfirm={async () => {
            const changed = await setSessionProvider(session.id, providerHandoffTarget.id)
            if (changed) setProviderHandoffTargetId('')
            return changed
          }}
        />
      )}
      {selectedToolCall && (
        <ToolCallModal
          tool={selectedToolCall}
          chatId={selectedToolCallRef?.messageId ? session.id : undefined}
          onClose={() => setSelectedToolCallRef(null)}
          onOpenSubAgent={selectedToolCall.isSubAgent ? () => void openSelectedSubAgentSession() : undefined}
          accentColor={accentColor}
        />
      )}
      {selectedRepositoryFile && repositoryChanges && (
        <RepositoryDiffDialog
          chatId={session.id}
          file={selectedRepositoryFile}
          vcsType={repositoryChanges.activeVcsType}
          comparisonRef={repositoryComparisonRef}
          getDiff={getVcsFileDiff}
          onClose={() => setSelectedRepositoryFile(null)}
        />
      )}
      <div className="flex-1 flex flex-col min-w-0">
        <div className="relative flex-1 min-h-0">
          <div ref={scrollRef} className="uam-chat-transcript relative z-0 h-full overflow-auto" data-copy-surface="chat" onScroll={handleScroll}>
            <div className="uam-chat-content w-full py-4">
              <div className="uam-message-list space-y-1.5">
              {earliestRenderedMessageIndex > 0 && (
                <div className="flex justify-center">
                  <Button
                    variant="secondary"
                    size="sm"
                    onClick={() => setRenderedMessageCount((current) => current + RENDERED_MESSAGE_BATCH_SIZE)}
                  >
                    Show earlier messages
                  </Button>
                </div>
              )}
              {visibleMessages.map((message, visibleIndex) => {
                const index = earliestRenderedMessageIndex + visibleIndex
                const shouldRenderTimelineAtAssistant = renderTimelineAtAssistant && index === turnAssistantMessageIndex
                const shouldSkipAssistantMessage = renderTimelineAfterUser && turnAssistantMessageMatches && index === turnAssistantMessageIndex
                const isUserMessage = message.role === 'user'
                const isEditingMessage = editingMessageIndex === index
                const branchParentId = session.parentChatId && session.branchFromMessageIndex === index
                  ? session.parentChatId
                  : session.id
                const messageBranchSessions = isUserMessage
                  ? branchSessions.filter((candidate) =>
                    candidate.id === branchParentId ||
                    (candidate.parentChatId === branchParentId && candidate.branchFromMessageIndex === index)
                  )
                  : []
                const messageBranchIndex = messageBranchSessions.findIndex((candidate) => candidate.id === session.id)
                const isBranchPoint = messageBranchSessions.length > 1 && messageBranchIndex >= 0
                const goalReview = goalReviewForMessage(message)
                const messageProviderId = message.providerId?.trim() || currentProviderId
                const messageProviderName = providerShortName(
                  providers.find((candidate) => candidate.id === messageProviderId),
                  messageProviderId
                )
                const branchLabel = session.branchFromMessageIndex === index
                  ? session.branchMessageEdited ? 'Edited branch' : 'Reverted branch'
                  : undefined

                if (shouldSkipAssistantMessage) return null

                return (
                  <div key={message.id} className="space-y-1">
                    <MessageFrame
                      role={message.role}
                      assistantLabel={messageProviderName}
                      copyText={message.content}
                      branchLabel={branchLabel}
                      branchNavigation={isBranchPoint ? {
                        current: messageBranchIndex + 1,
                        total: messageBranchSessions.length,
                        onPrevious: () => {
                          const id = messageBranchSessions[messageBranchIndex - 1].id
                          setPreferredBranch(branchParentId, index, id)
                          setActiveSession(id)
                        },
                        onNext: () => {
                          const id = messageBranchSessions[messageBranchIndex + 1].id
                          setPreferredBranch(branchParentId, index, id)
                          setActiveSession(id)
                        },
                      } : undefined}
                      goalReview={Boolean(goalReview)}
                      streaming={Boolean(shouldRenderTimelineAtAssistant && acp?.processing)}
                      actionsDisabled={!canChangeProvider || branchingMessageIndex !== null}
                      onEdit={isUserMessage ? () => {
                        setEditingMessageIndex(index)
                        setEditingMessageText(message.content)
                        setMessageBranchError('')
                      } : undefined}
                      onRevert={isUserMessage ? () => void createMessageBranch(index) : undefined}
                    >
                      {isEditingMessage ? (
                        <div className="space-y-2">
                          <textarea
                            aria-label="Edit message"
                            autoFocus
                            value={editingMessageText}
                            onChange={(event) => setEditingMessageText(event.target.value)}
                            rows={Math.max(3, editingMessageText.split('\n').length)}
                            className="w-full resize-y rounded-md p-2 text-sm"
                            style={{ border: '1px solid var(--border-bright)', background: 'var(--bg)', color: 'var(--text)' }}
                          />
                          <div className="flex justify-end gap-2">
                            <Button
                              type="button"
                              size="sm"
                              variant="ghost"
                              onClick={() => setEditingMessageIndex(null)}
                            >
                              Cancel
                            </Button>
                            <Button
                              type="button"
                              size="sm"
                              variant="primary"
                              loading={branchingMessageIndex === index}
                              disabled={!editingMessageText.trim() || !canChangeProvider}
                              onClick={() => void createMessageBranch(index, editingMessageText)}
                            >
                              Save to new branch
                            </Button>
                          </div>
                        </div>
                      ) : shouldRenderTimelineAtAssistant ? (
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
                            onResolvePermission={(requestId, optionId) => resolveAcpPermission(session.id, requestId, optionId)}
                            onResolveUserInput={(requestId, answers) => resolveAcpUserInput(session.id, requestId, answers)}
                          onCancelTurn={() => void cancelAcpTurn(session.id)}
                          onStopRuntime={() => void stopAcpSession(session.id)}
                          sourceChatId={session.id}
                          active={Boolean(acp?.processing)}
                          workingMode={workingDisplayMode}
                          workedSeconds={turnWorkedSeconds}
                        />
                      ) : (
                        <PersistedMessageContent
                          message={message}
                          onSelectTool={(messageId, toolId) => setSelectedToolCallRef({ id: toolId, messageId })}
                          planActions={planActionsForMessage(index)}
                          sourceChatId={session.id}
                          workingMode={workingDisplayMode}
                        />
                      )}
                    </MessageFrame>
                    {renderTimelineAfterUser && index === turnUserMessageIndex && (
                      <MessageFrame
                        key={`turn-${turnSerial}-after-user`}
                        role="assistant"
                        assistantLabel={currentProviderName}
                        streaming={Boolean(acp?.processing)}
                      >
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
                            onResolvePermission={(requestId, optionId) => resolveAcpPermission(session.id, requestId, optionId)}
                            onResolveUserInput={(requestId, answers) => resolveAcpUserInput(session.id, requestId, answers)}
                          onCancelTurn={() => void cancelAcpTurn(session.id)}
                          onStopRuntime={() => void stopAcpSession(session.id)}
                          sourceChatId={session.id}
                          active={Boolean(acp?.processing)}
                          workingMode={workingDisplayMode}
                          workedSeconds={turnWorkedSeconds}
                        />
                      </MessageFrame>
                    )}
                  </div>
                )
              })}
              {acp?.processing && turnEvents.length === 0 && (
                <div
                  data-testid="turn-starting"
                  role="status"
                  className="uam-turn-starting flex items-center gap-2 px-4 py-2 text-xs"
                  style={{ color: 'var(--text-3)' }}
                >
                  <span className="h-1.5 w-1.5 animate-pulse rounded-full" style={{ background: 'var(--accent)' }} aria-hidden />
                  Starting…
                </div>
              )}
              {turnEvents.length > 0 && !renderTimelineAfterUser && !renderTimelineAtAssistant && !completedFallbackAlreadyPersisted && (
                <MessageFrame
                  key={`turn-${turnSerial}-fallback`}
                  role="assistant"
                  assistantLabel={currentProviderName}
                  streaming={Boolean(acp?.processing)}
                >
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
                      onResolvePermission={(requestId, optionId) => resolveAcpPermission(session.id, requestId, optionId)}
                      onResolveUserInput={(requestId, answers) => resolveAcpUserInput(session.id, requestId, answers)}
                    onCancelTurn={() => void cancelAcpTurn(session.id)}
                    onStopRuntime={() => void stopAcpSession(session.id)}
                    sourceChatId={session.id}
                    active={Boolean(acp?.processing)}
                    workingMode={workingDisplayMode}
                    workedSeconds={turnWorkedSeconds}
                  />
                </MessageFrame>
              )}
              {(repositoryChanges || (isGitWorktree && latestAssistantMessage?.checkpointSha)) && (
                <section
                  aria-label="Review changes"
                  className="mx-4 overflow-hidden rounded-md text-xs"
                  style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}
                >
                  <div className="flex flex-wrap items-center justify-between gap-2 px-3 py-2 font-medium" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text)' }}>
                    <div className="flex min-w-0 items-baseline gap-2">
                      <span>Review changes</span>
                      {repositoryChanges && (
                        <span className="font-normal" style={{ color: 'var(--text-3)' }}>
                          {repositoryChanges.changedFiles.length} changed file{repositoryChanges.changedFiles.length === 1 ? '' : 's'}
                        </span>
                      )}
                    </div>
                    <Button type="button" size="sm" variant="secondary" aria-label="Open commit panel" onClick={() => setCommitPanelOpen(true)}>
                      Open commit panel
                    </Button>
                  </div>
                  {repositoryChanges && (
                    <div className="max-h-48 overflow-y-auto">
                      {repositoryChanges.changedFiles.map((file) => (
                      <button
                        key={file.path}
                        type="button"
                        aria-label={`Review changes to ${file.path}`}
                        className="uam-choice-button flex min-h-11 w-full items-center gap-2 px-3 py-2 text-left"
                        style={{ borderBottom: '1px solid var(--border)', background: 'transparent', color: 'var(--text-2)' }}
                        onClick={() => setSelectedRepositoryFile(file)}
                      >
                        <span className="w-6 shrink-0 text-center font-mono" style={{ color: 'var(--text-3)' }}>{file.status.trim() || 'M'}</span>
                        <span className="min-w-0 flex-1 truncate" title={file.path}>{file.path}</span>
                        {file.binary ? (
                          <span className="shrink-0 font-mono" style={{ color: 'var(--text-3)' }}>BIN</span>
                        ) : (
                          <span className="flex shrink-0 gap-2 font-mono"><span style={{ color: 'var(--green)' }}>+{file.additions}</span><span style={{ color: 'var(--red)' }}>-{file.deletions}</span></span>
                        )}
                        <ChevronRight size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)' }} />
                      </button>
                      ))}
                    </div>
                  )}
                  {isGitWorktree && latestAssistantMessage?.checkpointSha && (
                    <div className="flex flex-wrap items-center justify-between gap-3 px-3 py-2" style={{ borderTop: repositoryChanges ? '1px solid var(--border)' : undefined, color: 'var(--text-2)' }}>
                      <span>This completed turn has an isolated Git checkpoint.</span>
                      <Button type="button" size="sm" variant="secondary" disabled={workspaceActionsDisabled} loading={workspaceActionBusy} onClick={() => void rollbackLatestTurn()}>
                        Roll back turn
                      </Button>
                    </div>
                  )}
                </section>
              )}
              <div ref={bottomRef} />
              </div>
            </div>
          </div>
          {showScrollToBottom && (
            <div className="pointer-events-none absolute inset-x-0 bottom-3 z-10 flex justify-center">
              <IconButton
                icon={<ArrowDown size={16} />}
                label="Scroll to bottom"
                tooltip="Jump to the latest message"
                variant="solid"
                className="pointer-events-auto shadow-md"
                onClick={scrollToBottom}
              />
            </div>
          )}
        </div>

        {displayedGoal && (
          <GoalBanner
            goal={displayedGoal}
            onComplete={handleCompleteGoal}
            onPause={handlePauseGoal}
            onResume={() => void handleResumeGoal()}
            resumePending={goalSubmitting}
            onRemove={handleRemoveGoal}
            onEdit={displayedGoal.executionOwner !== 'provider' && displayedGoal.status !== 'complete' ? handleEditGoal : undefined}
            workerModelLabel={displayedGoalWorkerLabel}
            reviewerModelLabel={displayedGoalReviewerLabel}
          />
        )}

        <form
          onSubmit={submit}
          className="flex-shrink-0"
          style={{
            background: 'color-mix(in srgb, var(--bg) 94%, var(--surface))',
          }}
        >
            <div className="uam-chat-content uam-composer-region flex flex-col p-3">
            {confirmYolo && (
              <Notice
                tone="warning"
                title="Enable YOLO?"
                dismissLabel="Dismiss YOLO warning"
                onDismiss={() => setConfirmYolo(false)}
                actions={(
                  <>
                    <Button size="sm" variant="secondary" onClick={() => setConfirmYolo(false)}>Cancel</Button>
                    <Button size="sm" variant="danger" onClick={() => {
                      setConfirmYolo(false)
                      void applyPermissionMode('yolo').then((result) => showPermissionResult(result, 'Failed to change permission mode to YOLO.'))
                    }}>Enable YOLO</Button>
                  </>
                )}
              >
                YOLO automatically approves every permission in this chat, including computer use, commands, and file changes.
              </Notice>
            )}
            {rollbackConfirmation && (
              <Notice
                tone="warning"
                title="Roll back workspace?"
                dismissLabel="Dismiss workspace rollback warning"
                onDismiss={() => setRollbackConfirmation(null)}
                actions={(
                  <>
                    <Button size="sm" variant="secondary" onClick={() => setRollbackConfirmation(null)}>Cancel</Button>
                    <Button size="sm" variant="danger" loading={workspaceActionBusy} onClick={() => void confirmLatestTurnRollback()}>Roll back turn</Button>
                  </>
                )}
              >
                This returns the isolated workspace to before the selected turn. {rollbackConfirmation.diff}
              </Notice>
            )}
            {messageBranchError && (
              <Notice key={`branch:${messageBranchError}`} tone="error" title="Message branch failed" dismissLabel="Dismiss message branch error" onDismiss={() => setMessageBranchError('')}>
                {messageBranchError}
              </Notice>
            )}
            {goalError && (
              <Notice key={`goal:${goalError}`} tone="error" title="Goal update failed" dismissLabel="Dismiss goal error" onDismiss={() => setGoalError('')}>
                {goalError}
              </Notice>
            )}
            {workspaceFeedback && (
              <Notice key={`workspace:${workspaceFeedback.tone}:${workspaceFeedback.message}`} tone={workspaceFeedback.tone} title="Workspace" dismissLabel="Dismiss workspace action message" onDismiss={() => setWorkspaceFeedback(null)}>
                {workspaceFeedback.message}
              </Notice>
            )}
            {!providerSupported && (
              <Notice key={`unsupported:${currentProviderId}`} tone="warning" title="Provider unavailable" dismissLabel="Dismiss unsupported provider warning">
                {unsupportedProviderMessage}
              </Notice>
            )}
            {session.importedReadOnly && (
              <Notice tone="warning" title="Imported transcript" dismissLabel="Dismiss imported transcript notice">
                This portable transcript is read-only and cannot start a provider. Create a new chat in a workspace to continue.
              </Notice>
            )}
            {isClaudeProvider(currentProvider, currentProviderId) && (
              <Notice key={`claude-structured:${currentProviderId}`} tone="warning" title="Limited structured support" dismissLabel="Dismiss Claude structured mode warning">
                Claude structured mode cannot surface interactive permission or user-input prompts, and model discovery is limited to the active model. Use the CLI fallback when a turn needs interaction.
              </Notice>
            )}
            {acp?.lastError && currentAcpErrorKey !== dismissedAcpErrorKey && (
              <Notice
                key={`acp:${currentAcpErrorKey}`}
                tone="error"
                title={currentErrorTitle}
                dismissLabel="Dismiss composer error"
                onDismiss={() => setDismissedAcpErrorKey(currentAcpErrorKey)}
                actions={(
                  <>
                    <CopyTextButton text={buildAcpErrorCopyText(acp, currentErrorTitle)} label="Copy error" title="Copy error details" />
                    {acp.lifecycleState === 'error' && (
                      <>
                        <Button
                          size="sm"
                          variant="secondary"
                          aria-label="Check provider CLI"
                          onClick={() => void refreshCliProviderVersion(currentProviderId)}
                        >
                          Check CLI
                        </Button>
                        <Button
                          size="sm"
                          variant="secondary"
                          aria-label="Open CLI settings"
                          onClick={() => setSettingsOpen(true)}
                        >
                          CLI settings
                        </Button>
                        <Button
                          size="sm"
                          variant="secondary"
                          aria-label="Open fallback terminal"
                          disabled={!onOpenTerminalFallback}
                          onClick={onOpenTerminalFallback}
                        >
                          Open terminal
                        </Button>
                      </>
                    )}
                  </>
                )}
              >
                <span style={{ color: 'var(--red)', fontWeight: 600 }}>{currentErrorTitle}</span>
                <span style={{ color: 'var(--text-2)' }}> · </span>
                {acp.lastError}
                <AcpErrorDetails acp={acp} title={currentErrorTitle} />
              </Notice>
              )}
            {claudePlanPrompt !== null && (
              <Notice
                tone="warning"
                title="Claude Plan mode"
                dismissLabel="Dismiss Claude Plan mode warning"
                onDismiss={() => setClaudePlanPrompt(null)}
                actions={(
                  <>
                  <Button
                    size="sm"
                    variant="primary"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('acceptEdits')}
                  >
                    Accept edits and proceed
                  </Button>
                  <Button
                    size="sm"
                    variant="secondary"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('default')}
                  >
                    Review each edit
                  </Button>
                  <Button
                    size="sm"
                    variant="secondary"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('plan')}
                  >
                    Keep planning
                  </Button>
                  </>
                )}
              >
                Claude Plan mode is read-only. Choose how to proceed with this prompt.
              </Notice>
            )}
            {slashMessage && (
              <Notice key={`command:${slashMessage}`} tone={slashNoticeTone} title="Command" dismissLabel="Dismiss command feedback" onDismiss={() => setSlashMessage('')}>
                {slashMessage}
              </Notice>
            )}
            {attachmentError && (
              <Notice key={`attachment:${attachmentError}`} tone="error" title="Attachment failed" dismissLabel="Dismiss attachment error" onDismiss={() => setAttachmentError('')}>
                {attachmentError}
              </Notice>
            )}
            {(acp?.queuedPrompts?.length ?? 0) > 0 && (
              <div className="mb-1.5" role="status" aria-label="Queued prompt">
                {acp!.queuedPrompts!.slice(0, 1).map((prompt, index) => (
                  <div
                    key={`${index}-${prompt.text}`}
                    className="uam-queued-prompt flex items-center gap-2 px-3 py-1.5 text-[11px] animate-fade-in transition-colors duration-150"
                    style={{
                      color: 'var(--text-2)',
                      border: '1px solid color-mix(in srgb, var(--accent) 28%, var(--border))',
                      borderRadius: 8,
                      background: 'color-mix(in srgb, var(--accent) 7%, var(--surface))',
                    }}
                  >
                    <span className="shrink-0 font-medium" style={{ color: 'var(--accent)' }}>Queued</span>
                    <span className="min-w-0 flex-1">
                      <span className="line-clamp-2 whitespace-pre-wrap break-words">{prompt.text}</span>
                      {(prompt.attachments.length > 0 || prompt.markdownStoreFiles.length > 0) && (
                        <span style={{ color: 'var(--text-3)' }}>{prompt.attachments.length + prompt.markdownStoreFiles.length} attachment{prompt.attachments.length + prompt.markdownStoreFiles.length === 1 ? '' : 's'}</span>
                      )}
                    </span>
                    <IconButton
                      icon={<CornerUpRight size={14} />}
                      label="Steer with this prompt now"
                      disabled={steering}
                      onClick={() => {
                        steerTurnSerialRef.current = acp?.turnSerial ?? 0
                        setSteering(true)
                        if (steeringTimeoutRef.current !== null) {
                          window.clearTimeout(steeringTimeoutRef.current)
                          steeringTimeoutRef.current = null
                        }
                        void steerQueuedAcpPrompt(session.id, index)
                          .then((ok) => {
                            if (!ok) setSteering(false)
                          })
                          .catch(() => {
                            setSteering(false)
                          })
                      }}
                    />
                    <IconButton
                      icon={<X size={14} />}
                      label="Remove queued prompt"
                      disabled={steering}
                      onClick={() => { void removeQueuedAcpPrompt(session.id, index) }}
                    />
                  </div>
                ))}
              </div>
            )}
            <div
              className="uam-composer-surface"
              onDragOver={(event) => event.preventDefault()}
              onDrop={onComposerDrop}
              style={{
                border: 'none',
                borderRadius: 10,
                background: 'var(--surface)',
                boxShadow: 'var(--elev-1)',
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
                      border: '1px solid color-mix(in srgb, var(--purple) 45%, var(--border))',
                      borderRadius: 7,
                      background: 'color-mix(in srgb, var(--purple) 10%, var(--surface-up))',
                      color: 'var(--text-2)',
                      padding: '5px 8px',
                    }}
                  >
                    <BookOpen size={13} aria-hidden style={{ color: 'var(--purple)' }} />
                    <span style={{ color: 'var(--purple)' }}>Skills</span>
                    <span className="truncate max-w-[260px]">{entry.title || entry.filePath.split(/[\\/]/).pop()}</span>
                    <button
                      type="button"
                      onClick={() => detachMarkdownStoreEntry(session.id, entry.filePath)}
                      title="Remove skill attachment"
                      aria-label={`Remove ${entry.title || 'skill'} attachment`}
                      className="uam-attachment-remove"
                      style={{ color: 'var(--text-3)' }}
                    >
                      <X size={12} aria-hidden />
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
                      aria-label={`Remove ${attachment.name} attachment`}
                      className="uam-attachment-remove"
                      style={{ color: 'var(--text-3)' }}
                    >
                      <X size={12} aria-hidden />
                    </button>
                  </span>
                ))}
              </div>
            )}
            {slashOpen && (
                <ViewportMenu
                  anchorRef={composerTextareaRef}
                  side="top"
                  id={slashListboxId}
                  className="overflow-hidden rounded-lg animate-fade-in"
                  style={{ width: composerTextareaRef.current?.getBoundingClientRect().width ?? 320, border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}
                  role="listbox"
                  aria-label={permissionModeQuery !== undefined ? 'Permission modes' : codexOptionKind === 'reasoning' ? 'Reasoning options' : codexOptionKind === 'speed' ? 'Speed options' : variantQuery !== undefined ? 'OpenCode variants' : 'Slash commands'}
                >
                  <div className="flex items-center gap-1.5 px-3 py-1.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)', borderBottom: '1px solid var(--border)' }}>
                    {permissionModeQuery !== undefined ? <Shield size={12} aria-hidden /> : codexOptionKind !== undefined || variantQuery !== undefined ? <Cpu size={12} aria-hidden /> : <BookOpen size={12} aria-hidden />}
                    {permissionModeQuery !== undefined ? 'Permission mode' : codexOptionKind === 'reasoning' ? 'Reasoning' : codexOptionKind === 'speed' ? 'Speed' : variantQuery !== undefined ? 'OpenCode variants' : 'Commands'}
                  </div>
                  <div className="overflow-y-auto" style={{ maxHeight: 360 }}>
                  {slashMatches.map((command, index) => {
                    const active = index === Math.min(slashIndex, slashMatches.length - 1)
                    return (
                      <button
                        key={command.id}
                        id={`${slashListboxId}-option-${index}`}
                        type="button"
                        role="option"
                        aria-selected={active}
                        aria-haspopup={command.groupEntries ? 'menu' : undefined}
                        aria-expanded={command.groupEntries ? slashGroup === command.id.slice('md-group:'.length) : undefined}
                        ref={(element) => {
                          if (active) element?.scrollIntoView?.({ block: 'nearest' })
                        }}
                        onMouseEnter={() => {
                          setSlashIndex(index)
                          if (command.id !== `md-group:${slashGroup}`) setSlashGroup('')
                        }}
                        onMouseDown={(e) => { e.preventDefault(); runSlashCommand(command) }}
                        className={`uam-menu-select__option flex w-full items-start gap-2.5 px-3 py-2 text-left${active ? ' is-selected' : ''}`}
                        style={{ color: active ? 'var(--text)' : 'var(--text-2)' }}
                      >
                        <span aria-hidden className="mt-0.5 shrink-0" style={{ color: active ? 'var(--accent)' : 'var(--text-2)' }}>{command.icon}</span>
                        <span className="min-w-0 flex-1">
                          <span className={permissionModeQuery === undefined && codexOptionKind === undefined && variantQuery === undefined ? 'block font-mono text-sm' : 'block text-sm'} style={{ color: active ? 'var(--accent)' : 'var(--text)' }}>{command.label}{command.groupEntries && <span ref={(element) => { slashGroupButtonRefs.current[command.id.slice('md-group:'.length)] = element }} data-slash-group-anchor=""><ChevronRight className="ml-1 inline" size={14} aria-hidden /></span>}</span>
                          <span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{command.hint}</span>
                        </span>
                      </button>
                    )
                  })}
                  </div>
                </ViewportMenu>
            )}
            {activeSlashGroups.map((group) => {
              const path = group.id.slice('md-group:'.length)
              const anchor = { current: slashGroupButtonRefs.current[path] }
              if (!group.groupEntries || !anchor.current) return null
              return <ViewportMenu key={group.id} anchorRef={anchor} side="right" role="menu" manageFocus={false} aria-label={`${path} skills`} className="animate-fade-in" style={{ width: 280, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}>
                <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>{path}</div>
                {group.groupEntries.map((command, index) => {
                  const childPath = command.id.startsWith('md-group:') ? command.id.slice('md-group:'.length) : path
                  return <button key={command.id} type="button" role="menuitem" aria-haspopup={command.groupEntries ? 'menu' : undefined} aria-expanded={command.groupEntries ? slashGroup === childPath : undefined} onMouseEnter={() => { setSlashGroupIndex(index); setSlashGroup(childPath) }} onMouseDown={(event) => { event.preventDefault(); runSlashCommand(command) }} className={`uam-menu-select__option w-full flex items-start gap-2 px-2 py-2 text-left${group === activeSlashGroup && index === slashGroupIndex ? ' is-selected' : ''}`} style={{ borderRadius: 6, color: group === activeSlashGroup && index === slashGroupIndex ? 'var(--text)' : 'var(--text-2)' }}>{command.groupEntries ? <BookOpen size={15} className="mt-0.5 shrink-0" aria-hidden /> : <FileText size={15} className="mt-0.5 shrink-0" aria-hidden />}<span className="min-w-0 flex-1"><span className="block font-mono text-sm" style={{ color: group === activeSlashGroup && index === slashGroupIndex ? 'var(--accent)' : 'var(--text)' }}>{command.label}{command.groupEntries && <span ref={(element) => { slashGroupButtonRefs.current[childPath] = element }}><ChevronRight className="ml-1 inline" size={14} aria-hidden /></span>}</span><span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{command.hint}</span></span></button>
                })}
              </ViewportMenu>
            })}
            {(dictationActive || dictationError) && (
              <div
                id={`dictation-status-${session.id}`}
                role={dictationError ? 'alert' : 'status'}
                aria-live={dictationError ? 'assertive' : 'polite'}
                data-dictation-state={dictationError ? 'error' : dictationState}
                className="flex items-center gap-2 px-3 pt-2 text-[11px]"
                style={{ color: dictationError ? 'var(--red)' : 'var(--accent)' }}
              >
                <span className="min-w-0 flex-1">
                  {dictationError || (
                    dictationState === 'starting'
                      ? 'Starting dictation…'
                      : dictationState === 'stopping'
                        ? 'Finishing dictation…'
                        : 'Listening…'
                  )}
                </span>
                {dictationError && (
                  <IconButton
                    icon={<X size={12} />}
                    label="Dismiss dictation error"
                    size="sm"
                    onClick={dismissDictationError}
                  />
                )}
              </div>
            )}
            <textarea
              ref={composerTextareaRef}
              value={draft}
              onChange={(event) => {
                setDraft(event.target.value)
                setComposerSelection({ start: event.target.selectionStart, end: event.target.selectionEnd })
                setSlashGroup('')
                setPermissionMenuOpen(false)
              }}
              onSelect={(event) => setComposerSelection({ start: event.currentTarget.selectionStart, end: event.currentTarget.selectionEnd })}
              onKeyDown={onComposerKeyDown}
              onPaste={onComposerPaste}
              rows={1}
              placeholder={`Message ${currentProviderName}`}
              disabled={submitting || dictationActive || session.importedReadOnly}
              aria-describedby={dictationActive || dictationError ? `dictation-status-${session.id}` : undefined}
              aria-haspopup="listbox"
              aria-expanded={slashOpen}
              aria-controls={slashOpen ? slashListboxId : undefined}
              aria-activedescendant={activeSlashOptionId}
              aria-autocomplete="list"
              className="uam-composer-textarea w-full resize-none text-sm"
              style={{
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
              acp={providerAcp}
              provider={currentProvider}
              providers={providers}
              providerId={currentProviderId}
              canChangeProvider={canChangeProvider}
              canSend={canSend}
              runtimeStatusLabel={statusLabel(providerAcp)}
              runtimeStatusColor={statusColor(providerAcp)}
              modelId={session.smallModelMode ? activeGoal?.workerModelId || currentModelId : currentModelId}
              reviewerModelId={session.smallModelMode ? activeGoal?.reviewerModelId || currentReviewerModelId : currentReviewerModelId}
              includeDefaultModel={showUnresolvedDefaultModel}
              session={session}
              reasoningEffort={session.reasoningEffort ?? ''}
              serviceTier={session.serviceTier ?? ''}
              serviceTierExplicit={serviceTierExplicit}
              providerModeId={currentModeId}
              featurePreference={featurePreference}
              uamAgentId={selectedUamAgentId}
              uamAgentNextTurn={Boolean(providerAcp?.processing)}
              permissionModeId={selectedPermissionModeId}
              permissionsManagedByUam={permissionsManagedByUam}
              permissionControlsDisabled={permissionControlsDisabled}
              providerModes={providerModes}
              uamAgents={uamAgents}
              memoryLevel={currentMemoryLevel}
              defaultMemoryLevel={defaultMemoryLevel}
              memoryChipVisible={memoryChipExplicit || currentMemoryLevel !== defaultMemoryLevel}
              smallModelMode={session.smallModelMode ?? false}
              modelOpen={modelOpen}
              modelMenuRef={modelMenuRef}
			  onToggleModel={() => {
				if (!modelOpen && isOpenCodeProvider(currentProvider, currentProviderId) && !providerAcp?.modelsLoading && !providerVariants.some((option) => option.id.toLowerCase() === 'effort' || option.id.toLowerCase() === 'thought_level')) {
				  void discoverProviderModels('', currentProviderId, workspaceDirectory)
				}
                setModelOpen((value) => !value)
                setWorkspaceMenuOpen(false)
              }}
              onSelectProvider={(providerId) => {
                setModelOpen(false)
                if (providerId !== currentProviderId) setProviderHandoffTargetId(providerId)
              }}
              onSelectModel={(modelId) => {
                setModelOpen(false)
                void setSessionModel(session.id, modelId)
              }}
              onSelectReviewerModel={(modelId) => void setSessionReviewerModel(session.id, modelId)}
              onSelectReasoning={(reasoningEffort) => {
                void setSessionCodexOptions(session.id, { reasoningEffort })
              }}
              onSelectSpeed={(serviceTier) => {
                void setSessionCodexOptions(session.id, { serviceTier: serviceTier === CODEX_SPEED_INHERIT_ID ? '' : serviceTier, serviceTierExplicit: serviceTier !== CODEX_SPEED_INHERIT_ID })
              }}
              onSelectConfigOption={(configId, value) => void setAcpConfigOption(session.id, configId, value)}
              onSelectProviderMode={(modeId) => void setSessionApprovalMode(session.id, modeId)}
              onSelectUamAgent={(agentId) => void setSessionUamAgent(session.id, agentId)}
              onSelectPermissionMode={(modeId) => void selectPermissionMode(modeId)}
              onSelectMemoryLevel={(level) => {
                setMemoryChipExplicit(true)
                void setSessionMemoryLevel(session.id, level)
              }}
              onClearMemoryLevel={() => {
                setMemoryChipExplicit(false)
                void setSessionMemoryLevel(session.id, defaultMemoryLevel)
              }}
              onToggleSmallModelMode={() => void setSessionSmallModelMode(session.id, !(session.smallModelMode ?? false))}
              goalArmed={goalArmNextMessage}
              goalActive={Boolean(activeGoal)}
              goalPaused={Boolean(goalPaused)}
              defaultGoalTokenBudget={defaultGoalTokenBudget}
              onToggleGoal={handleToggleGoal}
              onSetDefaultGoalTokenBudget={(value) => setDefaultGoalTokenBudget(session.id, value)}
              onStopRuntime={() => void stopAcpSession(session.id)}
              onAttachFile={() => fileInputRef.current?.click()}
              onOpenMarkdownStore={() => void openMarkdownStore()}
              workspaceControl={(
                <div ref={workspaceMenuRef} className="relative shrink-0">
                  <IconButton
                    size="sm"
                    disabled={!workspaceDirectory}
                    onClick={() => {
                      setWorkspaceMenuOpen((open) => !open)
                      setModelOpen(false)
                    }}
                    icon={<ComposerIcon name="folder" size={14} />}
                    label={workspaceDirectory ? 'Workspace actions' : 'Workspace not selected'}
                    aria-haspopup="menu"
                    aria-expanded={workspaceMenuOpen}
                    aria-controls={workspaceMenuOpen ? workspaceMenuId : undefined}
                    tooltipSide="bottom"
                  />
                  {workspaceMenuOpen && workspaceDirectory && (
                    <ViewportMenu
                      anchorRef={workspaceMenuRef}
                      side="top"
                      id={workspaceMenuId}
                      role="menu"
                      aria-label="Workspace actions"
                      onRequestClose={() => setWorkspaceMenuOpen(false)}
                      className="animate-fade-in"
                      style={{ width: 240, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}
                    >
                      <div className="truncate px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }} title={workspaceDirectory}>
                        {workspaceDirectory}
                      </div>
                      {isGitWorktree && (
                        <div className="px-2 pb-1 text-[11px]" style={{ color: 'var(--green)' }}>
                          Git worktree{sourceWorkspaceDirectory ? ` · source ${sourceWorkspaceDirectory}` : ''}
                        </div>
                      )}
                      <button type="button" role="menuitem" className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" onClick={() => { setWorkspaceMenuOpen(false); void openWorkspace() }}><ComposerIcon name="folder" size={14} /><span>Open workspace</span></button>
                      <button type="button" role="menuitem" className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" onClick={() => { setWorkspaceMenuOpen(false); void openWorkspaceEditor() }}><ComposerIcon name="editor" size={14} /><span>Open in editor</span></button>
                      <button type="button" role="menuitem" className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" onClick={() => { setWorkspaceMenuOpen(false); void openWorkspaceTerminal() }}><ComposerIcon name="terminal" size={14} /><span>Open terminal</span></button>
                      <div className="my-1 border-t" style={{ borderColor: 'var(--border)' }} />
                      {!isGitWorktree ? (
                        <button type="button" role="menuitem" disabled={workspaceActionsDisabled} className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" style={{ opacity: workspaceActionsDisabled ? 0.5 : 1 }} onClick={() => { setWorkspaceMenuOpen(false); void runWorkspaceAction('create') }}><ComposerIcon name="git-tree" size={14} /><span>Create worktree</span></button>
                      ) : (
                        <>
                          <button type="button" role="menuitem" disabled={workspaceActionsDisabled} className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" style={{ opacity: workspaceActionsDisabled ? 0.5 : 1 }} onClick={() => { setWorkspaceMenuOpen(false); void runWorkspaceAction('discard') }}><span>Discard &amp; return</span></button>
                          <button type="button" role="menuitem" disabled={workspaceActionsDisabled} className="uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" style={{ opacity: workspaceActionsDisabled ? 0.5 : 1 }} onClick={() => { setWorkspaceMenuOpen(false); void runWorkspaceAction('port') }}><span>Port back</span></button>
                        </>
                      )}
                    </ViewportMenu>
                  )}
                </div>
              )}
              dictationState={dictationState}
              dictationError={dictationError}
              dictationElapsedSeconds={dictationElapsedSeconds}
              dictationAvailable={dictationAvailable}
              onToggleDictation={() => {
                if (dictationActiveRef.current) void stopDictation()
                else void startDictation()
              }}
            />
            </div>
          </div>
        </form>
      </div>
    </div>
  )
})
