import { ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, RefObject, type ReactNode, memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
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
  COMMAND_SAFETY_TIERS,
  PERMISSION_MODES,
  type ComposerIconName,
  ComposerIcon,
  ComposerToolbar,
  permissionModeIcon,
  permissionModeForTier,
  type DictationState,
} from '../chat/Composer'
import { ViewportMenu } from '../ui'
import { Brain, BookOpen, ChevronRight, CornerUpRight, Cpu, FileText, Paperclip, Shield, Target, X } from 'lucide-react'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'
import { Button, IconButton } from '../ui'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'

interface ChatViewProps {
  session: Session
  accentColor?: string
}

type SlashCommand = {
  id: string
  label: string
  hint: string
  icon: ReactNode
  run: () => void
  groupEntries?: SlashCommand[]
}

const INITIAL_RENDERED_MESSAGES = 200
const RENDERED_MESSAGE_BATCH_SIZE = 100
const SCROLL_NEAR_BOTTOM_THRESHOLD = 100
const STEERING_TIMEOUT_MS = 5000

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

function fileUriToPath(uri: string): string {
  if (!uri.startsWith('file://')) return uri
  try {
    return decodeURIComponent(new URL(uri).pathname)
  } catch {
    return uri.replace(/^file:\/\//, '')
  }
}

export const ChatView = memo(function ChatView({ session, accentColor }: ChatViewProps) {
  const [draft, setDraft] = useState('')
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
  const [permissionMenuOpen, setPermissionMenuOpen] = useState(false)
  const [memoryChipExplicit, setMemoryChipExplicit] = useState(false)
  const [composerAttachments, setComposerAttachments] = useState<LocalAttachment[]>([])
  const [attachmentError, setAttachmentError] = useState('')
  const [dismissedAcpErrorKey, setDismissedAcpErrorKey] = useState('')
  const [editingMessageIndex, setEditingMessageIndex] = useState<number | null>(null)
  const [editingMessageText, setEditingMessageText] = useState('')
  const [branchingMessageIndex, setBranchingMessageIndex] = useState<number | null>(null)
  const [messageBranchError, setMessageBranchError] = useState('')
  const [renderedMessageCount, setRenderedMessageCount] = useState(INITIAL_RENDERED_MESSAGES)
  const steerTurnSerialRef = useRef(0)
  const steeringTimeoutRef = useRef<number | null>(null)

  useEffect(() => {
    return () => {
      if (steeringTimeoutRef.current !== null) {
        window.clearTimeout(steeringTimeoutRef.current)
      }
    }
  }, [])

  useEffect(() => setMemoryChipExplicit(false), [session.id])
  const slashGroupButtonRefs = useRef<Record<string, HTMLButtonElement | null>>({})
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const workingDisplayMode = useAppStore((s) => s.workingDisplayMode)
  const cli = useAppStore((s) => s.cliBindingBySessionId[session.id])
  const providers = useAppStore((s) => s.providers)
  const stageChatAttachments = useAppStore((s) => s.stageChatAttachments)
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  const removeQueuedAcpPrompt = useAppStore((s) => s.removeQueuedAcpPrompt)
  const steerQueuedAcpPrompt = useAppStore((s) => s.steerQueuedAcpPrompt)
  const branchFromMessage = useAppStore((s) => s.branchFromMessage)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const branchRootChatId = session.branchRootChatId || session.parentChatId || session.id
  const branchSessions = useAppStore(useShallow((s) => s.sessions
    .filter((candidate) => (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === branchRootChatId)
    .sort((a, b) => a.createdAt.getTime() - b.createdAt.getTime())))
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const stopAcpSession = useAppStore((s) => s.stopAcpSession)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const resolveAcpUserInput = useAppStore((s) => s.resolveAcpUserInput)
  const setSessionProvider = useAppStore((s) => s.setSessionProvider)
  const setSessionModel = useAppStore((s) => s.setSessionModel)
  const setSessionCodexOptions = useAppStore((s) => s.setSessionCodexOptions)
  const setSessionApprovalMode = useAppStore((s) => s.setSessionApprovalMode)
  const setSessionCommandSafetyTier = useAppStore((s) => s.setSessionCommandSafetyTier)
  const setSessionMemoryLevel = useAppStore((s) => s.setSessionMemoryLevel)
  const setSessionSmallModelMode = useAppStore((s) => s.setSessionSmallModelMode)
  const configuredApprovalMode = useAppStore((s) => s.sessions.find((candidate) => candidate.id === session.id)?.approvalMode)
  const openSessionWorkspace = useAppStore((s) => s.openSessionWorkspace)
  const openSessionWorkspaceEditor = useAppStore((s) => s.openSessionWorkspaceEditor)
  const openSessionTerminal = useAppStore((s) => s.openSessionTerminal)
  const openSubAgentSession = useAppStore((s) => s.openSubAgentSession)
  const createChatWorktree = useAppStore((s) => s.createChatWorktree)
  const discardChatWorktreeChanges = useAppStore((s) => s.discardChatWorktreeChanges)
  const portChatWorktreeChanges = useAppStore((s) => s.portChatWorktreeChanges)
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const markdownStoreEntries = useAppStore(useShallow((s) => s.markdownStoreEntries))
  const defaultMemoryLevel = useAppStore((s) => s.memoryLevelDefault)
  const refreshMarkdownStore = useAppStore((s) => s.refreshMarkdownStore)
  const attachMarkdownStoreEntry = useAppStore((s) => s.attachMarkdownStoreEntry)
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
  const scrollRef = useRef<HTMLDivElement>(null)
  const bottomRef = useRef<HTMLDivElement>(null)
  const isNearBottomRef = useRef(true)
  const fileInputRef = useRef<HTMLInputElement>(null)
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
    submitInFlightRef.current = false
    setSubmitting(false)
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
  const turnWorkedSeconds = acp?.processing
    ? acp.processingStartedAtMs ? (Date.now() - acp.processingStartedAtMs) / 1000 : undefined
    : turnAssistantMessageIndex >= 0
      ? (messages[turnAssistantMessageIndex]?.processingTimeMs ?? 0) / 1000
      : undefined
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
  ])

  const handleScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    isNearBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < SCROLL_NEAR_BOTTOM_THRESHOLD
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
    setRenderedMessageCount(INITIAL_RENDERED_MESSAGES)
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
    const goalMatch = prompt.match(/^\/goal\s+(.+?)(?:\s+--budget\s+(\d+))?\s*$/)
    if (!goalMatch) return false

    const objective = goalMatch[1].trim()
    const tokenBudget = goalMatch[2] ? parseInt(goalMatch[2], 10) : defaultGoalTokenBudget

    if (!objective) {
      setGoalError('Goal objective is required.')
      return true
    }

    setGoalSubmitting(true)
	const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
	const providerManaged = Boolean(nativeGoalCommand)
	const goalId = await setGoalStore(session.id, objective, tokenBudget, providerManaged ? 'provider' : 'uam')
	const goalAttachments = composerAttachments
	  .filter((attachment) => attachment.status === 'ready')
	  .map(({ status, error, ...attachment }) => attachment)
	const sent = goalId ? await sendAcpPrompt(session.id, providerManaged ? `${nativeGoalCommand} ${objective}` : objective, goalAttachments) : false
    setGoalSubmitting(false)

	if (goalId && sent) {
      setDraft('')
	  setComposerAttachments([])
	  setAttachmentError('')
      setGoalError('')
    } else {
	  setGoalError(goalId ? 'Goal was created, but the first prompt failed to send.' : 'Failed to create goal.')
    }
    return true
  }

  const submit = async (event?: FormEvent, promptOverride?: string, steerNow = false) => {
    event?.preventDefault()
    const prompt = (promptOverride ?? draft).trim()
    if (!providerSupported || !prompt || submitInFlightRef.current || goalSubmitting || composerAttachments.some((attachment) => attachment.status !== 'ready')) return
	if (dictationActiveRef.current && promptOverride === undefined) {
	  await stopDictation(true)
	  return
	}
	const readyAttachments = composerAttachments
	  .filter((attachment) => attachment.status === 'ready')
	  .map(({ status, error, ...attachment }) => attachment)

    // Handle /goal command
    if (prompt.startsWith('/goal ')) {
      void submitGoal(prompt)
      return
    }
	const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
	if (nativeGoalCommand && (prompt === nativeGoalCommand || prompt.startsWith(`${nativeGoalCommand} `))) {
	  const objective = prompt.slice(nativeGoalCommand.length).trim()
	  if (!objective) {
		setGoalError('Goal objective is required.')
		return
	  }
	  setGoalSubmitting(true)
	  const goalId = await setGoalStore(session.id, objective, defaultGoalTokenBudget, 'provider')
	  const ok = goalId ? await sendAcpPrompt(session.id, prompt, readyAttachments) : false
	  setGoalSubmitting(false)
	  if (!ok) {
		setGoalError(goalId ? 'Goal was created, but the provider command failed to send.' : 'Failed to create goal.')
		return
	  }
	  setGoalError('')
	  setDraft('')
	  setComposerAttachments([])
	  setAttachmentError('')
	  return
	}

    if (goalArmNextMessage) {
      setGoalSubmitting(true)
	  const providerGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
	  const goalId = await setGoalStore(session.id, prompt, defaultGoalTokenBudget, providerGoalCommand ? 'provider' : 'uam')
      if (!goalId) {
        setGoalSubmitting(false)
        setGoalError('Failed to create goal.')
        return
      }
      const ok = steerNow
		? await sendAcpPrompt(session.id, providerGoalCommand ? `${providerGoalCommand} ${prompt}` : prompt, readyAttachments, true)
		: await sendAcpPrompt(session.id, providerGoalCommand ? `${providerGoalCommand} ${prompt}` : prompt, readyAttachments)
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
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
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
      const warning = result.status?.warning
      setWorkspaceFeedback({
        message: warning || result.message || (action === 'port' ? 'Applied chat changes and returned to the source workspace.' : 'Workspace action complete.'),
        tone: warning ? 'warning' : 'success',
      })
    } else {
      setWorkspaceFeedback({ message: result.status?.error || result.message || 'Workspace action failed.', tone: 'error' })
    }
  }
  const currentProviderId = session.providerId || acp?.providerId || DEFAULT_PROVIDER_ID
  const providerSupported = providers.some((candidate) => candidate.id === currentProviderId)
  const currentProvider = useMemo<Provider>(
    () =>
      providers.find((candidate) => candidate.id === currentProviderId) ?? fallbackProviderForId(currentProviderId),
    [currentProviderId, providers]
  )
  const providerAcp = acp?.providerId === currentProviderId ? acp : undefined
  const currentProviderName = providerShortName(currentProvider, currentProviderId)
  const errorProviderId = acp?.providerId || currentProviderId
  const errorProvider = providers.find((candidate) => candidate.id === errorProviderId) ?? fallbackProviderForId(errorProviderId)
  const currentErrorTitle = `${providerShortName(errorProvider, errorProviderId)} ${providerRuntimeLabel(errorProvider, acp)} error`
  const currentAcpErrorKey = acp?.lastError ? `${session.id}:${acp.lastError}` : ''
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
    () => providerSupported && draft.trim().length > 0 && !submitting && !goalSubmitting && !composerAttachments.some((attachment) => attachment.status !== 'ready'),
    [providerSupported, draft, submitting, goalSubmitting, composerAttachments]
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
        setDictationError(message.message || 'Dictation failed.')
        setDictationState('stopping')
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
  const showUnresolvedDefaultModel = !session.modelId && !providerAcp?.currentModelId
  const currentModel = modelOptionFor(buildModelOptions(providerAcp, currentModelId, currentProvider, currentProviderId, showUnresolvedDefaultModel), currentModelId)
  const currentProviderCapabilities = providerCapabilities(currentProviderId, currentProvider)
  const runtimeSupportsReasoning = (selectedRuntimeModel(providerAcp, currentModel.id)?.supportedReasoningEfforts?.length ?? 0) > 0
  const reasoningOptions = currentProviderCapabilities.hasReasoningEffort || runtimeSupportsReasoning
    ? buildCodexReasoningOptions(providerAcp, currentModel.id, session.reasoningEffort ?? '')
    : []
  const speedOptions = currentProviderCapabilities.hasServiceTier
    ? buildCodexSpeedOptions(providerAcp, currentModel.id, session.serviceTier ?? '')
    : []
  const currentModeId = configuredApprovalMode || session.approvalMode || providerAcp?.currentModeId || 'default'
  const agentModes = useMemo(() => {
    const modes = (providerAcp?.availableModes.length ?? 0) > 0
      ? providerAcp!.availableModes.filter((mode) => mode.id === 'default' || mode.id === 'plan')
      : [
          { id: 'default', name: 'Default', description: 'Use the provider default agent.' },
          { id: 'plan', name: 'Plan', description: 'Research and plan before implementation.' },
        ]
    return modes
  }, [providerAcp?.availableModes, currentProvider, currentProviderId])
  const permissionModes = useMemo(
    () => PERMISSION_MODES.filter((mode) => mode.id !== 'acceptEdits' || providerCapabilities(currentProviderId, currentProvider).hasAcceptEditsMode),
    [currentProvider, currentProviderId]
  )
  const selectedPermissionModeId = permissionModeForTier(session.commandSafetyTier ?? 'medium')
  const applyPermissionMode = async (modeId: string) => {
    if (!providerSupported || !currentProvider.supportsStructured) return false
    if (modeId === 'default') return setSessionCommandSafetyTier(session.id, 'off')
    if (modeId === 'acceptEdits') return setSessionCommandSafetyTier(session.id, 'acceptEdits')
    if (modeId === 'yolo') return setSessionCommandSafetyTier(session.id, 'yolo')
    if (modeId === 'auto') {
      const tier = session.commandSafetyTier
      return setSessionCommandSafetyTier(session.id, tier === 'low' || tier === 'high' ? tier : 'medium')
    }
    return false
  }
  const selectPermissionMode = async (modeId: string) => {
    const mode = permissionModes.find((candidate) => candidate.id === modeId)
    if (!mode) return
    const changed = await applyPermissionMode(mode.id)
    setSlashMessage(changed ? '' : `Failed to change permission mode to ${mode.name}.`)
  }
  const runPermissionCommand = async (rawMode?: string) => {
    setDraft('')
    setSlashIndex(0)
    setPermissionMenuOpen(false)
    if (!providerSupported || !currentProvider.supportsStructured) {
      setSlashMessage(`Permission-mode changes are unavailable for ${currentProviderName}.`)
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

    const changed = await applyPermissionMode(requested.id)
    setSlashMessage(changed ? '' : `Failed to change permission mode to ${requested.name}.`)
  }
  const runCommandSafetyCommand = async (rawTier?: string) => {
    setDraft('')
    setSlashIndex(0)
    if (!rawTier) {
      setDraft('/safety ')
      return
    }
    const requested = COMMAND_SAFETY_TIERS.find((tier) => tier.id === rawTier.trim().toLowerCase())
    if (!requested) {
      setSlashMessage(`Unsupported command safety tier "${rawTier}". Supported: ${COMMAND_SAFETY_TIERS.map((tier) => tier.id).join(', ')}.`)
      return
    }
    const changed = await setSessionCommandSafetyTier(session.id, requested.id)
    setSlashMessage(changed ? `Command safety changed to ${requested.label}.` : `Failed to change command safety to ${requested.label}.`)
  }
  const runCodexOptionCommand = async (kind: 'reasoning' | 'speed', rawValue?: string) => {
    setDraft('')
    setSlashIndex(0)
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
      : await setSessionCodexOptions(session.id, { serviceTier: requested.id })
    setSlashMessage(changed ? `${label} changed to ${requested.label}.` : `Failed to change ${kind} to ${requested.label}.`)
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
  const goalPaused = displayedGoal?.status === 'paused'
  const handleCompleteGoal = () => {
    if (displayedGoal) {
      void updateGoalStatus(displayedGoal.id, 'complete')
    }
  }
  const handleResumeGoal = async () => {
    if (!displayedGoal || goalSubmitting) return
    setGoalError('')
    setGoalSubmitting(true)
    const resumed = await resumeGoal(session.id, displayedGoal.id)
    setGoalSubmitting(false)
    if (!resumed) setGoalError('Failed to resume goal.')
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
    setDraft('')
    setSlashIndex(0)
    setMemoryChipExplicit(true)
    void setSessionMemoryLevel(session.id, level)
  }
  const slashCommands = useMemo<SlashCommand[]>(
    () => [
      { id: 'model', label: '/model', hint: 'Change the model', icon: <Cpu size={15} />, run: () => setModelOpen(true) },
      ...(reasoningOptions.length > 0 ? [{ id: 'reasoning', label: '/reasoning', hint: 'Choose Codex reasoning', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('reasoning') }] : []),
      ...(speedOptions.length > 0 ? [{ id: 'speed', label: '/speed', hint: 'Choose Codex speed', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('speed') }] : []),
      { id: 'permission', label: '/permission', hint: 'Choose the permission mode', icon: <Shield size={15} />, run: () => void runPermissionCommand() },
      { id: 'safety', label: '/safety', hint: 'Choose the command safety tier', icon: <Shield size={15} />, run: () => void runCommandSafetyCommand() },
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
      ...markdownStoreEntries.filter((entry) => entry.favorite && !entry.group).map((entry) => ({
        id: `md:${entry.id}`,
        label: '/' + (entry.commandName || entry.title.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'skill'),
        hint: `${entry.title}${entry.sourceProvider ? ` · ${entry.sourceProvider}` : ''}`,
        icon: <FileText size={15} />,
        run: () => attachMarkdownStoreEntry(session.id, entry),
      })),
      ...Object.entries(markdownStoreEntries.filter((entry) => entry.favorite && entry.group).reduce<Record<string, typeof markdownStoreEntries>>((groups, entry) => {
        ;(groups[entry.group!] ??= []).push(entry)
        return groups
      }, {})).map(([group, entries]) => ({
        id: `md-group:${group}`,
        label: '/' + (group.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'skills'),
        hint: `${entries.length} skill${entries.length === 1 ? '' : 's'}`,
        icon: <BookOpen size={15} />,
        run: () => setSlashGroup(group),
        groupEntries: entries.map((entry) => ({
          id: `md:${entry.id}`,
          label: '/' + (entry.commandName || entry.title.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'skill'),
          hint: `${entry.title}${entry.sourceProvider ? ` · ${entry.sourceProvider}` : ''}`,
          icon: <FileText size={15} />,
          run: () => attachMarkdownStoreEntry(session.id, entry),
        })),
      })),
    ],
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [session.id, currentMemoryLevel, session.commandSafetyTier, session.reasoningEffort, session.serviceTier, markdownStoreEntries, currentModeId, permissionModes, providerSupported, currentProviderName, reasoningOptions, speedOptions]
  )
  const permissionModeMatch = /^\/permission\s+([\w-]*)$/i.exec(draft)
  const commandSafetyMatch = /^\/safety\s+([\w-]*)$/i.exec(draft)
  const memoryLevelMatch = /^\/memory\s+([\w-]*)$/i.exec(draft)
  const codexOptionMatch = /^\/(reasoning|speed)\s+([\w-]*)$/i.exec(draft)
  const slashMatch = /^\/([\w-]*)$/.exec(draft)
  const slashQuery = slashMatch ? slashMatch[1].toLowerCase() : null
  const permissionModeQuery = permissionMenuOpen ? '' : permissionModeMatch?.[1].toLowerCase()
  const commandSafetyQuery = commandSafetyMatch?.[1].toLowerCase()
  const memoryLevelQuery = memoryLevelMatch?.[1].toLowerCase()
  const codexOptionKind = codexOptionMatch?.[1].toLowerCase() as 'reasoning' | 'speed' | undefined
  const codexOptionQuery = codexOptionMatch?.[2].toLowerCase()
  const codexOptionQueryOptions = codexOptionKind === 'reasoning' ? reasoningOptions : codexOptionKind === 'speed' ? speedOptions : []
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
    : commandSafetyQuery !== undefined
      ? COMMAND_SAFETY_TIERS
          .filter((tier) => tier.id.startsWith(commandSafetyQuery) || tier.label.toLowerCase().startsWith(commandSafetyQuery))
          .map((tier) => ({
            id: `safety:${tier.id}`,
            label: tier.label,
            hint: `${tier.id === (session.commandSafetyTier ?? 'medium') ? 'Current · ' : ''}${tier.detail}`,
            icon: <Shield size={15} />,
            run: () => void runCommandSafetyCommand(tier.id),
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
            hint: `${option.id === (codexOptionKind === 'reasoning' ? session.reasoningEffort ?? '' : session.serviceTier ?? '') ? 'Current · ' : ''}${option.detail}`,
            icon: <Cpu size={15} />,
            run: () => void runCodexOptionCommand(codexOptionKind, option.id || 'default'),
          }))
    : slashQuery !== null
      ? slashCommands.filter((command) => command.label.slice(1).toLowerCase().startsWith(slashQuery))
      : []
  const slashOpen = slashMatches.length > 0 && (slashQuery !== null || permissionModeQuery !== undefined || commandSafetyQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined)
  const slashPaletteVisible = slashQuery !== null || permissionModeQuery !== undefined || commandSafetyQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined
  useEffect(() => {
    if (slashPaletteVisible && markdownStoreEntries.length === 0) {
      void refreshMarkdownStore()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [slashPaletteVisible])
  const runSlashCommand = (command: { run: () => void }) => {
    setDraft('')
    setSlashIndex(0)
    setPermissionMenuOpen(false)
    command.run()
  }
  const activeSlashGroup = slashCommands.find((command) => command.id === `md-group:${slashGroup}`)
  const activeSlashGroupAnchor = { current: slashGroupButtonRefs.current[slashGroup] }

  const onComposerKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
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
      if (event.key === 'Enter' || event.key === 'Tab') {
        event.preventDefault()
        runSlashCommand(slashMatches[Math.min(slashIndex, slashMatches.length - 1)])
        return
      }
      if (event.key === 'Escape') {
        event.preventDefault()
        setDraft('')
        setPermissionMenuOpen(false)
        return
      }
    }
    const permissionCommand = /^\/permission(?:\s+(\S+))?\s*$/i.exec(draft)
    if (event.key === 'Enter' && !event.shiftKey && permissionCommand) {
      event.preventDefault()
      void runPermissionCommand(permissionCommand[1])
      return
    }
    const commandSafetyCommand = /^\/safety(?:\s+(\S+))?\s*$/i.exec(draft)
    if (event.key === 'Enter' && !event.shiftKey && commandSafetyCommand) {
      event.preventDefault()
      void runCommandSafetyCommand(commandSafetyCommand[1])
      return
    }
    const codexOptionCommand = /^\/(reasoning|speed)(?:\s+(\S+))?\s*$/i.exec(draft)
    if (event.key === 'Enter' && !event.shiftKey && codexOptionCommand) {
      event.preventDefault()
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
    const permissionOk = !modeOk || nextModeId !== 'acceptEdits'
      ? modeOk
      : await setSessionCommandSafetyTier(session.id, 'acceptEdits')
    if (permissionOk) {
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
          chatId={selectedToolCallRef?.messageId ? session.id : undefined}
          onClose={() => setSelectedToolCallRef(null)}
          onOpenSubAgent={selectedToolCall.isSubAgent ? () => void openSelectedSubAgentSession() : undefined}
          accentColor={accentColor}
        />
      )}
      <div className="flex-1 flex flex-col min-w-0">
        <div ref={scrollRef} className="uam-chat-transcript flex-1 overflow-auto" data-copy-surface="chat" onScroll={handleScroll}>
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
                const shouldSkipAssistantMessage = renderTimelineAfterUser && index === turnAssistantMessageIndex
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
                        onPrevious: () => setActiveSession(messageBranchSessions[messageBranchIndex - 1].id),
                        onNext: () => setActiveSession(messageBranchSessions[messageBranchIndex + 1].id),
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
                            onResolvePermission={(requestId, optionId) => {
                              void resolveAcpPermission(session.id, requestId, optionId)
                            }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
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
                            onResolvePermission={(requestId, optionId) => {
                              void resolveAcpPermission(session.id, requestId, optionId)
                            }}
                            onResolveUserInput={(requestId, answers) => {
                              void resolveAcpUserInput(session.id, requestId, answers)
                            }}
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
              {messageBranchError && (
                <div role="alert" className="text-center text-xs" style={{ color: 'var(--error)' }}>
                  {messageBranchError}
                </div>
              )}
              {turnEvents.length > 0 && !renderTimelineAfterUser && !renderTimelineAtAssistant && (
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
                      onResolvePermission={(requestId, optionId) => {
                        void resolveAcpPermission(session.id, requestId, optionId)
                      }}
                      onResolveUserInput={(requestId, answers) => {
                        void resolveAcpUserInput(session.id, requestId, answers)
                      }}
                    onCancelTurn={() => void cancelAcpTurn(session.id)}
                    onStopRuntime={() => void stopAcpSession(session.id)}
                    sourceChatId={session.id}
                    active={Boolean(acp?.processing)}
                    workingMode={workingDisplayMode}
                    workedSeconds={turnWorkedSeconds}
                  />
                </MessageFrame>
              )}
              <div ref={bottomRef} />
            </div>
          </div>
        </div>

        {goalError && (
          <div className="px-4 py-1 text-xs" style={{ background: 'var(--surface)', color: 'var(--error)' }}>
            {goalError}
          </div>
        )}

        {displayedGoal && (
          <GoalBanner
            goal={displayedGoal}
            onComplete={handleCompleteGoal}
            onPause={handlePauseGoal}
            onResume={() => void handleResumeGoal()}
            resumePending={goalSubmitting}
            onRemove={handleRemoveGoal}
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
              {workspaceFeedback && (
                <div
                  role={workspaceFeedback.tone === 'error' ? 'alert' : 'status'}
                  className="order-10 mt-2 flex items-start gap-2 text-xs"
                  style={{
                    border: `1px solid color-mix(in srgb, var(--${workspaceFeedback.tone === 'success' ? 'green' : workspaceFeedback.tone === 'warning' ? 'yellow' : 'red'}) 42%, var(--border))`,
                    borderRadius: 6,
                    padding: '8px 10px',
                    background: `color-mix(in srgb, var(--${workspaceFeedback.tone === 'success' ? 'green' : workspaceFeedback.tone === 'warning' ? 'yellow' : 'red'}) 9%, var(--surface))`,
                    color: 'var(--text)',
                    overflowWrap: 'anywhere',
                  }}
                >
                  <span className="min-w-0 flex-1">{workspaceFeedback.message}</span>
                  <button type="button" aria-label="Dismiss workspace action message" onClick={() => setWorkspaceFeedback(null)} style={{ border: 0, background: 'transparent', color: 'var(--text-3)', padding: 0 }}><X size={13} aria-hidden /></button>
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
            {isClaudeProvider(currentProvider, currentProviderId) && (
              <div className="mb-2 rounded-md border px-2.5 py-2 text-xs" style={{ borderColor: 'color-mix(in srgb, var(--yellow) 45%, var(--border))', background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))', color: 'var(--text-2)' }}>
                Claude structured mode cannot surface interactive permission or user-input prompts, and model discovery is limited to the active model. Use Accept Edits, Plan, or the CLI fallback when a turn needs interaction.
              </div>
            )}
            {acp?.lastError && currentAcpErrorKey !== dismissedAcpErrorKey && (
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
                  <IconButton icon={<X size={13} />} label="Dismiss composer error" onClick={() => setDismissedAcpErrorKey(currentAcpErrorKey)} />
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
                </div>
              </div>
            )}
            {slashMessage && (
              <div role="status" className="mb-2 rounded-md px-3 py-2 text-[11px]" style={{ color: 'var(--text-2)', border: '1px solid var(--border)', background: 'var(--surface-up)' }}>
                {slashMessage}
              </div>
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
            {attachmentError && (
              <div className="px-3 pt-2 text-[11px]" style={{ color: 'var(--red)' }}>
                {attachmentError}
              </div>
            )}
            {slashOpen && (
              <div className="relative">
                <div
                  className="overflow-hidden rounded-lg animate-fade-in"
                  style={{ position: 'absolute', left: 12, right: 12, bottom: 4, zIndex: 1000, border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}
                  role="listbox"
                  aria-label={permissionModeQuery !== undefined ? 'Permission modes' : commandSafetyQuery !== undefined ? 'Command safety tiers' : codexOptionKind === 'reasoning' ? 'Reasoning options' : codexOptionKind === 'speed' ? 'Speed options' : 'Slash commands'}
                >
                  <div className="flex items-center gap-1.5 px-3 py-1.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)', borderBottom: '1px solid var(--border)' }}>
                    {permissionModeQuery !== undefined || commandSafetyQuery !== undefined ? <Shield size={12} aria-hidden /> : codexOptionKind !== undefined ? <Cpu size={12} aria-hidden /> : <BookOpen size={12} aria-hidden />}
                    {permissionModeQuery !== undefined ? 'Permission mode' : commandSafetyQuery !== undefined ? 'Command safety' : codexOptionKind === 'reasoning' ? 'Reasoning' : codexOptionKind === 'speed' ? 'Speed' : 'Commands'}
                  </div>
                  <div className="overflow-y-auto" style={{ maxHeight: 360 }}>
                  {slashMatches.map((command, index) => {
                    const active = index === Math.min(slashIndex, slashMatches.length - 1)
                    return (
                      <button
                        key={command.id}
                        type="button"
                        role="option"
                        aria-selected={active}
                        ref={(element) => {
                          if (command.groupEntries) slashGroupButtonRefs.current[command.id.slice('md-group:'.length)] = element
                          if (active) element?.scrollIntoView?.({ block: 'nearest' })
                        }}
                        onMouseEnter={() => setSlashIndex(index)}
                        onMouseDown={(e) => { e.preventDefault(); runSlashCommand(command) }}
                        className={`uam-menu-select__option flex w-full items-start gap-2.5 px-3 py-2 text-left${active ? ' is-selected' : ''}`}
                        style={{ color: active ? 'var(--text)' : 'var(--text-2)' }}
                      >
                        <span aria-hidden className="mt-0.5 shrink-0" style={{ color: active ? 'var(--accent)' : 'var(--text-2)' }}>{command.icon}</span>
                        <span className="min-w-0 flex-1">
                          <span className={permissionModeQuery === undefined && commandSafetyQuery === undefined && codexOptionKind === undefined ? 'block font-mono text-sm' : 'block text-sm'} style={{ color: active ? 'var(--accent)' : 'var(--text)' }}>{command.label}{command.groupEntries && <ChevronRight className="ml-1 inline" size={14} aria-hidden />}</span>
                          <span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{command.hint}</span>
                        </span>
                      </button>
                    )
                  })}
                  </div>
                </div>
              </div>
            )}
            {activeSlashGroup?.groupEntries && activeSlashGroupAnchor.current && (
              <ViewportMenu anchorRef={activeSlashGroupAnchor} side="right" role="menu" aria-label={`${slashGroup} skills`} className="animate-fade-in" style={{ width: 280, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}>
                <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>{slashGroup}</div>
                {activeSlashGroup.groupEntries.map((command) => <button key={command.id} type="button" role="menuitem" onMouseDown={(event) => { event.preventDefault(); setSlashGroup(''); runSlashCommand(command) }} className="uam-menu-select__option w-full flex items-start gap-2 px-2 py-2 text-left" style={{ borderRadius: 6, color: 'var(--text-2)' }}><FileText size={15} className="mt-0.5 shrink-0" aria-hidden /><span className="min-w-0"><span className="block font-mono text-sm" style={{ color: 'var(--text)' }}>{command.label}</span><span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{command.hint}</span></span></button>)}
              </ViewportMenu>
            )}
            {(dictationActive || dictationError) && (
              <div
                id={`dictation-status-${session.id}`}
                role={dictationError ? 'alert' : 'status'}
                aria-live={dictationError ? 'assertive' : 'polite'}
                data-dictation-state={dictationError ? 'error' : dictationState}
                className="px-3 pt-2 text-[11px]"
                style={{ color: dictationError ? 'var(--red)' : 'var(--accent)' }}
              >
                {dictationError || (
                  dictationState === 'starting'
                    ? 'Starting dictation…'
                    : dictationState === 'stopping'
                      ? 'Finishing dictation…'
                      : 'Listening…'
                )}
              </div>
            )}
            <textarea
              value={draft}
              onChange={(event) => {
                setDraft(event.target.value)
                setPermissionMenuOpen(false)
              }}
              onKeyDown={onComposerKeyDown}
              onPaste={onComposerPaste}
              rows={3}
              placeholder={`Message ${currentProviderName}`}
              disabled={submitting || dictationActive}
              aria-describedby={dictationActive || dictationError ? `dictation-status-${session.id}` : undefined}
              className="uam-composer-textarea w-full resize-none text-sm"
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
              acp={providerAcp}
              provider={currentProvider}
              providers={providers}
              providerId={currentProviderId}
              canChangeProvider={canChangeProvider}
              canSend={canSend}
              runtimeStatusLabel={statusLabel(providerAcp)}
              runtimeStatusColor={statusColor(providerAcp)}
              modelId={currentModelId}
              includeDefaultModel={showUnresolvedDefaultModel}
              session={session}
              reasoningEffort={session.reasoningEffort ?? ''}
              serviceTier={session.serviceTier ?? ''}
              approvalModeId={currentModeId}
              permissionModeId={selectedPermissionModeId}
              agentModes={agentModes}
              commandSafetyTier={session.commandSafetyTier ?? 'medium'}
              memoryLevel={currentMemoryLevel}
              defaultMemoryLevel={defaultMemoryLevel}
              memoryChipVisible={memoryChipExplicit || currentMemoryLevel !== defaultMemoryLevel}
              smallModelMode={session.smallModelMode ?? false}
              modelOpen={modelOpen}
              modelMenuRef={modelMenuRef}
              onToggleModel={() => {
                setModelOpen((value) => !value)
                setWorkspaceMenuOpen(false)
              }}
              onSelectProvider={(providerId) => {
                setModelOpen(false)
                if (providerId !== currentProviderId) void setSessionProvider(session.id, providerId)
              }}
              onSelectModel={(modelId) => {
                setModelOpen(false)
                void setSessionModel(session.id, modelId)
              }}
              onSelectReasoning={(reasoningEffort) => {
                void setSessionCodexOptions(session.id, { reasoningEffort })
              }}
              onSelectSpeed={(serviceTier) => {
                void setSessionCodexOptions(session.id, { serviceTier })
              }}
              onSelectAgentMode={(modeId) => void setSessionApprovalMode(session.id, modeId)}
              onSelectPermissionMode={(modeId) => void selectPermissionMode(modeId)}
              onSetCommandSafetyTier={(tier) => void setSessionCommandSafetyTier(session.id, tier)}
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
                    tooltipSide="bottom"
                  />
                  {workspaceMenuOpen && workspaceDirectory && (
                    <ViewportMenu
                      anchorRef={workspaceMenuRef}
                      side="top"
                      role="menu"
                      aria-label="Workspace actions"
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
