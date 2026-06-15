import { ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, ReactNode, RefObject, useEffect, useMemo, useRef, useState } from 'react'
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
  providerRuntimeKindLabel,
  providerShortName,
} from '../../utils/providerMetadata'
import {
  type ModelOption,
  buildCodexReasoningOptions,
  buildCodexSpeedOptions,
  buildModelOptions,
  CODEX_REASONING_LABELS,
  CODEX_SPEED_LABELS,
  FRIENDLY_MODEL_LABELS,
  GEMINI_FALLBACK_ACP_MODEL_OPTIONS,
  labeledOption,
  modelOptionFor,
  providerRuntimeLabel,
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
  toolStatusColor,
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
} from '../chat/MessageBlocks'
import {
  acpRuntimeBlocksControlChanges,
  type ComposerIconName,
  ComposerIcon,
  ComposerToolbar,
} from '../chat/Composer'
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

const PLAN_APPROVE_PROMPT = 'Proceed with the plan.'
const PLAN_DENY_PROMPT = 'Do not proceed with this plan. Please revise it before making changes.'

type LocalAttachmentStatus = 'ready' | 'staging' | 'failed'
interface LocalAttachment extends Attachment {
  status: LocalAttachmentStatus
  error?: string
}

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


function fileUriToPath(uri: string): string {
  if (!uri.startsWith('file://')) return uri
  try {
    return decodeURIComponent(new URL(uri).pathname)
  } catch {
    return uri.replace(/^file:\/\//, '')
  }
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
