import { browserDictationAvailable } from '../../ipc/companionPlatform'
import { useVoiceMode } from '../../hooks/useVoiceMode'
import { buildChatSearchSessionGroups } from '../sidebar/chatSearch'
import type { AcpPromptPresentationObserver } from '../../store/storeTypes'
import { subagentParentFor, subscribeSubagentNavigation, subagentNavigationRevision } from '../../utils/subagentNavigation'
import type { AcpBinding, AcpQueuedPrompt, ManualCompactionAvailability } from '../../store/cpp/types'
import { attachmentsEquivalent } from '../../store/cpp/reconcile'
import { messageAttachments, sanitizeManualCompaction, sanitizeNativeInputId, sanitizeOriginRequestId, sanitizeManagedOriginRunId } from '../../store/cpp/sanitizers'
import { formatContextReferences, sanitizeContextReferences } from '../../utils/contextReferences'
import { FileInspectionProvider } from '../shared/FileInspectionProvider'
import { chatHasUnfinishedWork } from '../../utils/chatWork'
import { MessageNavigationRail } from '../chat/MessageNavigationRail'
import { ChangedFilesTree } from '../chat/ChangedFilesTree'
import { changedLineTotals } from '../chat/changedFilesTreeModel'
import { SubagentWaitStatus, isWaitingOnSubagents } from '../chat/SubagentWaitStatus'
import { isManagedSubagentTool } from '../chat/managedSubagents'
import { isInlineSubagentLaunch } from '../chat/inlineSubagents'
import { InitialManagedSubagentGroup, InlineSubagentProvider } from '../chat/InlineSubagentGroup'
import { openSubagentView } from '../../utils/subagentNavigation'
import { InlinePromptQueue, type UnsentPrompt } from '../chat/InlinePromptQueue'
import { OutgoingRecoveryActions } from '../chat/OutgoingMessageRow'
import type { ConversationDelivery } from '../chat/ConversationTurn'
import { PersistedConversationMessage, TurnStartTime } from '../chat/PersistedConversationMessage'
import { ALWAYS_OPEN_WORK_SECTION, buildVisibleWorkSections, findWorkSectionStartIndex, dayDividerLabel, hasSubstantiveOutput, previousAssistantMessage, responseFlags, reviewAssistantLabel, turnTimestampLabel } from '../chat/conversationHistoryModel'
import { addOutgoingAttempt, nextOutgoingKey, outgoingScope, readOutgoingAttempts, subscribeOutgoingAttempts, updateOutgoingAttempts, type OutgoingAttempt } from '../../utils/outgoingMessagePresentation'
import { COMPUTER_USE_ENABLED } from '../../config/buildFeatures'
import { assignChatToPane, readChatGridLayout } from '../../utils/chatGridStorage'
import { ClipboardEvent, DragEvent, FormEvent, KeyboardEvent, type ReactNode, memo, useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react'
import { createPortal } from 'react-dom'
import { useShallow } from 'zustand/react/shallow'
import type { ComputerUseActionResult, Session } from '../../types/session'
import { useAppStore, type ChatAttachmentInput, type DictationPushMessage, type VcsChangedFile, type VcsCommitStatus, type UamAgentCycleShortcut, type UamAgentSummary } from '../../store/useAppStore'
import type { Attachment, Message } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { Goal } from '../../types/goal'
import { RemoteDirectoryBrowser } from '../sidebar/RemoteDirectoryBrowser'
import { GoalBanner } from '../shared/GoalBanner'
import { DEFAULT_PROVIDER_ID, fallbackProviderForId, isClaudeProvider, isCodexProvider, isCopilotProvider, isOpenCodeProvider, providerCapabilities, providerShortName } from '../../utils/providerMetadata'
import { buildCodexReasoningOptions, buildCodexSpeedOptions, CODEX_SPEED_INHERIT_ID, buildModelOptions, modelOptionFor, providerRuntimeLabel, selectedRuntimeModel } from '../chat/modelOptions'
import { CompactingScene, SleepingScene } from '../chat/StatusScenes'
import { buildAcpErrorCopyText, ChatStatusStrip, CopyTextButton, statusColor, statusLabel } from '../chat/StatusHelpers'
import { SubAgentDisclosureProvider, WorkSectionContext, ConversationWork, type WorkTraceDisclosureState } from '../chat/ConversationWork'
import { MessageFrame, ToolCallModal, UserInputInlineCard } from '../chat/ToolCallViews'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'
import { attachmentChipKind, AttachmentList, PersistedMessageContent, TurnTimelineContent, formatWorkedDuration, stoppedResponseLabel, attachmentLabel, goalReviewForMessage, type WorkingDisplayMode } from '../chat/MessageBlocks'
import { acpRuntimeBlocksControlChanges, PERMISSION_MODES, ComposerIcon, ComposerToolbar, ComposerAgentSelector, permissionModeIcon, permissionModeForTier, providerConfigVariantOptions, type DictationState } from '../chat/Composer'
import { Notice, ViewportMenu, type NoticeTone } from '../ui'
import { CustomIcon } from '../shared/CustomIcon'
import { ChatBranchPicker } from '../chat/ChatBranchPicker'
import { AlertTriangle, ArrowDown, CornerUpLeft, GitBranch, Brain, BookOpen, ChevronRight, CornerUpRight, Cpu, FileText, LoaderCircle, MessageSquareDashed, MousePointer2, Paperclip, Pencil, Shield, Target, X } from 'lucide-react'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'
import { Button, IconButton, Tooltip } from '../ui'
import { isCompanionContext, isCefContext, sendToCEF, createRequestId } from '../../ipc/cefBridge'
import { setPreferredBranch } from '../../utils/branchPreferenceStorage'
import { ContextChip } from '../chat/ContextChip'
import { replaceSlashAction, slashActionToken } from '../../utils/slashActionToken'
import { acknowledgeChatComposerDraft, readChatComposerDraft, subscribeComposerDraftAcknowledgements, withoutAcknowledgedComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { getTopmostModal } from '../../utils/modalFocus'

interface ChatViewProps {
  session: Session
  accentColor?: string
  navigationKey?: string
  showSubagentLineage?: boolean
  /** Presentation only: retain transcript actions/recovery; hide the standing input and its entry points. */
  showComposer?: boolean
}

type SlashCommand = {
  id: string
  label: string
  hint: string
  icon: ReactNode
  run: () => void
  groupEntries?: SlashCommand[]
}

function manualCompactionForChat(session: Session | undefined, acp: AcpBinding | undefined): ManualCompactionAvailability | undefined {
  const support = sanitizeManualCompaction(acp?.manualCompaction)
  if (!session || !acp || !support?.available || session.importedReadOnly || !acp.running || acp.lifecycleState !== 'ready'
    || acpRuntimeBlocksControlChanges(acp) || acp.compacting || acp.queueHeld || (acp.queuedPrompts?.length ?? 0) > 0
    || acp.pendingPermission || acp.pendingUserInput || support.providerId !== session.providerId || support.providerId !== acp.providerId
    || support.modelId !== (session.modelId ?? '') || support.nativeSessionId !== acp.sessionId) return undefined
  return support
}

function sameManualCompactionTarget(left: ManualCompactionAvailability, right: ManualCompactionAvailability): boolean {
  return left.providerId === right.providerId && left.modelId === right.modelId && left.nativeSessionId === right.nativeSessionId
}

type ComposerNoticeTone = NoticeTone
type PermissionChangeResult = { ok: boolean; cancelled?: boolean; error?: string }

function normalizePermissionChangeResult(result: boolean | PermissionChangeResult): PermissionChangeResult {
  return typeof result === 'boolean' ? { ok: result } : result
}

const INITIAL_RENDERED_MESSAGES = 200
const COMPANION_INITIAL_RENDERED_MESSAGES = 50
const EMPTY_GOALS: Goal[] = []
const RENDERED_MESSAGE_BATCH_SIZE = 100
const SCROLL_NEAR_BOTTOM_THRESHOLD = 100

export { buildVisibleWorkSections, reviewAssistantLabel }

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

function promptReferencesMessage(prompt: AcpQueuedPrompt, nativeIndex: number, message: Message) {
  return Boolean(prompt.id && Number.isSafeInteger(prompt.revision) && prompt.revision! > 0 &&
    !prompt.prepared && prompt.existingUserMessageIndex === nativeIndex && prompt.text === message.content &&
    attachmentsEquivalent(message.attachments ?? [], messageAttachments(prompt)))
}

/** Native certainty owns recovery; pending direct input stays visible without granting Retry authority.
 * Known-unsent user rows keep their transcript slot (only empty trailing output hides); queue-held and discarded rows hide. */
export function projectUnsentPrompts(messages: Message[], historyStartIndex: number, knownIndices: readonly number[], retryIndices: readonly number[], heldPrompts: readonly AcpQueuedPrompt[] = [], pendingDirectPrompts: readonly AcpQueuedPrompt[] = []) {
  const pendingIndices = new Set<number>()
  if (knownIndices.length === 0 && heldPrompts.length === 0 && pendingDirectPrompts.length === 0 && !messages.some((message) => message.acpPromptDiscarded === true)) return { prompts: [] as UnsentPrompt[], hiddenIndices: new Set<number>(), pendingIndices }
  const known = new Set(knownIndices)
  const retryable = new Set(retryIndices)
  const hiddenIndices = new Set<number>()
  const prompts: UnsentPrompt[] = []
  for (let index = 0; index < messages.length; index += 1) {
    const message = messages[index]
    const nativeIndex = historyStartIndex + index
    if (message.role !== 'user' || !message.acpPromptNotSent) continue
    let end = index + 1
    while (end < messages.length && messages[end].role !== 'user') end += 1
    if (messages.slice(index + 1, end).some((output) => output.role !== 'assistant' || hasSubstantiveOutput(output))) continue
    if (!message.acpPromptDiscarded && pendingDirectPrompts.some((prompt) => promptReferencesMessage(prompt, nativeIndex, message))) {
      pendingIndices.add(index)
      continue
    }
    const representedByQueue = heldPrompts.some((prompt) => promptReferencesMessage(prompt, nativeIndex, message))
    if (!representedByQueue && !message.acpPromptDiscarded && !known.has(nativeIndex)) continue
    const inTranscript = !representedByQueue && !message.acpPromptDiscarded
    if (inTranscript) prompts.push({ messageIndex: nativeIndex, text: message.content, attachments: message.attachments ?? [], retryable: retryable.has(nativeIndex) })
    for (let hidden = inTranscript ? index + 1 : index; hidden < end; hidden += 1) hiddenIndices.add(hidden)
  }
  return { prompts, hiddenIndices, pendingIndices }
}

const SENDING_DELIVERY: ConversationDelivery = { state: 'sending', label: 'Waiting for the provider to accept this message. It is never resent automatically.' }
const UNSENT_DELIVERY: ConversationDelivery = { state: 'unsent', label: 'This message was not delivered. Retry, edit or remove it; it is never resent automatically.' }
const HELD_DELIVERY: ConversationDelivery = { state: 'unsent', label: 'This message was not delivered. Edit or remove it; it is never resent automatically.' }
const ACCEPTED_DELIVERY: ConversationDelivery = { state: 'sending', label: 'UAM accepted this message and is waiting for the provider to confirm delivery. It is never resent automatically.' }
const LOCAL_UNCONFIRMED_DELIVERY: ConversationDelivery = { state: 'unconfirmed', label: 'UAM could not confirm whether this message was sent. The submitted message is retained here and is never resent automatically.' }
const attemptDelivery = (attempt: OutgoingAttempt) =>
  attempt.recordedManagedInput ? { state: 'unconfirmed' as const, label: 'This request was saved in the parent chat. Saving it does not confirm provider delivery; it is never resent automatically.' } : attempt.phase === 'rejected' ? LOCAL_UNCONFIRMED_DELIVERY : attempt.phase === 'accepted' ? ACCEPTED_DELIVERY : SENDING_DELIVERY
const RECEIPT_DELIVERY: ConversationDelivery = { state: 'unconfirmed', label: 'UAM could not confirm whether this message reached the provider. Check the chat, then remove it if needed; it is never resent automatically.' }

function canRetryMessageInPlace(messages: Message[], messageIndex: number) {
  const message = messages[messageIndex]
  if (message?.role !== 'user') return false
  for (let index = messages.length - 1; index > messageIndex; index -= 1) {
    const trailing = messages[index]
    if (trailing.role !== 'assistant' || hasSubstantiveOutput(trailing)) return false
  }
  return true
}

function setRepositoryReview(sessionId: string, review: VcsCommitStatus | null) {
  useAppStore.setState((state) => {
    const repositoryReviewBySessionId = { ...state.repositoryReviewBySessionId }
    if (review) repositoryReviewBySessionId[sessionId] = review
    else delete repositoryReviewBySessionId[sessionId]
    return { repositoryReviewBySessionId }
  })
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
    setError('The provider or model could not be changed. Finish any active work and try again.')
  }

  return createPortal(
    <div
      className="uam-overlay fixed inset-0 z-[1000] flex items-center justify-center p-4"
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
            <div className="mb-1 text-xs font-semibold uppercase tracking-wide" style={{ color: 'var(--yellow)' }}>Reset for {targetName}</div>
            <ul className="list-disc space-y-1 pl-5">
              <li>Native session and provider-specific live tool state</li>
              <li>Model, reasoning or speed, permission, safety, and memory defaults</li>
            </ul>
          </div>
          <p className="text-xs" style={{ color: 'var(--text-3)' }}>Anything the current provider did not record in UAM will not transfer.</p>
          {blockedReason && <div role="alert" className="rounded-md p-2 text-xs" style={{ border: '1px solid var(--yellow)', color: 'var(--yellow)' }}>{blockedReason}</div>}
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

type ParentViewportDetails = {
  subagentDisclosures?: Set<string>
  workDisclosures?: Map<string, WorkTraceDisclosureState>
  liveDisclosure?: { serial: number; state: WorkTraceDisclosureState }
  sectionChoices?: { sessionId: string; expanded: Record<string, boolean> }
}
const parentViewportVisits = new Map<string, ParentViewportDetails & { top: number; nearBottom: boolean; renderedCount: number }>()

const receiptRetryAvailable = (prompt: AcpQueuedPrompt) => prompt.retryAllowed !== false && Number.isSafeInteger(prompt.existingUserMessageIndex) && prompt.existingUserMessageIndex! >= 0

export const ChatView = memo(function ChatView({ session, accentColor, navigationKey = '', showSubagentLineage = true, showComposer = true }: ChatViewProps) {
  const showComposerRef = useRef(showComposer)
  showComposerRef.current = showComposer
  const viewportKey = JSON.stringify([session.id, session.createdAt.getTime(), session.providerId, session.executionHostId, navigationKey, isCompanionContext() ? 'companion' : 'desktop'])
  // Retention belongs to an explicit pane visit; standalone mounts keep their normal initial window.
  const restoredViewport = useMemo(() => navigationKey ? parentViewportVisits.get(viewportKey) : undefined, [viewportKey, navigationKey])
  const subagentDisclosures = useMemo(() => restoredViewport?.subagentDisclosures ?? new Set<string>(), [viewportKey])
  const viewportDetails = useRef<ParentViewportDetails>({})
  const slashListboxId = useId()
  const workspaceMenuId = useId()
  const goalsPopupId = useId()
  const [goalsPopupOwner, setGoalsPopupOwner] = useState<string | null>(null)
  const goalsTriggerRef = useRef<HTMLButtonElement>(null)
  const goalsPopupRef = useRef<HTMLDivElement>(null)
  const goalMutationGenerationRef = useRef(0)
  const goalMutationOwnerRef = useRef<string | null>(null)
  const [draft, setDraft] = useState(() => readChatComposerDraft(session.id).text)
  const [composerSelection, setComposerSelection] = useState({ start: 0, end: 0 })
  const [submitting, setSubmitting] = useState(false)
  const [compacting, setCompacting] = useState(false)
  const [chatHistoryRetryingIds, setChatHistoryRetryingIds] = useState<Set<string>>(() => new Set())
  const [steering, setSteering] = useState(false)
  const [dictationState, setDictationState] = useState<DictationState>('idle')
  const [dictationElapsedSeconds, setDictationElapsedSeconds] = useState(0)
  const [dictationError, setDictationError] = useState('')
  const [selectedToolCallRef, setSelectedToolCallRef] = useState<SelectedToolCallRef | null>(null)
  const [modelOpen, setModelOpen] = useState(false)
  const [workspaceMenuOpen, setWorkspaceMenuOpen] = useState(false)
  const [remoteMoveFolderId, setRemoteMoveFolderId] = useState('')
  const [remoteBrowseKey, setRemoteBrowseKey] = useState('')
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
  const [composerAttachments, setComposerAttachments] = useState<LocalAttachment[]>(() =>
    readChatComposerDraft(session.id).attachments.map((attachment) => ({ ...attachment, status: 'ready' }))
  )
  const [attachmentError, setAttachmentError] = useState('')
  const [dismissedAcpErrorKey, setDismissedAcpErrorKey] = useState('')
  const [editingMessageIndex, setEditingMessageIndex] = useState<number | null>(null)
  const [editingMessageText, setEditingMessageText] = useState('')
  const [editingMessageAttachments, setEditingMessageAttachments] = useState<Attachment[]>([])
  const [editingMessageSource, setEditingMessageSource] = useState<{ kind: 'branch'; hadMarkdownStoreAttachments?: boolean } | { kind: 'unsent'; message: Message }>({ kind: 'branch' })
  const unsentEditInFlightRef = useRef<number | null>(null)
  const messageBranchGenerationRef = useRef(0)
  const [branchingMessageIndex, setBranchingMessageIndex] = useState<number | null>(null)
  const [messageBranchError, setMessageBranchError] = useState('')
  const [rollbackConfirmation, setRollbackConfirmation] = useState<{ messageIndex: number; diff: string } | null>(null)
  const [renderedMessageCount, setRenderedMessageCount] = useState(() => restoredViewport?.renderedCount ?? (isCompanionContext() ? COMPANION_INITIAL_RENDERED_MESSAGES : INITIAL_RENDERED_MESSAGES))
  const [olderHistoryLoading, setOlderHistoryLoading] = useState(false)
  const [selectedRepositoryFile, setSelectedRepositoryFile] = useState<VcsChangedFile | null>(null)
  const [providerHandoffTargetId, setProviderHandoffTargetId] = useState('')
  const [providerHandoffModelId, setProviderHandoffModelId] = useState<string | null>(null)
  const goalMutationInFlightRef = useRef(false)
  const appModalOpen = useAppStore((s) =>
    providerHandoffTargetId !== '' ||
    s.isNewChatModalOpen ||
    s.isSettingsOpen ||
    s.memoryLibraryScope !== null ||
    s.isMemoryScanModalOpen ||
    s.isMarkdownStoreOpen
  )

  useEffect(() => setSteering(false), [session.id])

  const composerDraftRef = useRef({ text: draft, attachments: [] as Attachment[] })
  composerDraftRef.current = {
    text: draft,
    attachments: composerAttachments
      .filter((attachment) => attachment.status === 'ready')
      .map(({ status: _status, error: _error, ...attachment }) => attachment),
  }
  useEffect(() => {
    const timer = window.setTimeout(() => writeChatComposerDraft(session.id, composerDraftRef.current), 250)
    return () => window.clearTimeout(timer)
  }, [composerAttachments, draft, session.id])
  useEffect(() => {
	const flush = () => {
	  if (useAppStore.getState().sessions.some((candidate) => candidate.id === session.id)) {
		writeChatComposerDraft(session.id, composerDraftRef.current)
	  }
	}
    window.addEventListener('pagehide', flush)
    return () => {
      window.removeEventListener('pagehide', flush)
      flush()
    }
  }, [session.id])
  const slashGroupButtonRefs = useRef<Record<string, HTMLSpanElement | null>>({})
  const messages = useAppStore(useShallow((s) => s.messages[session.id] ?? []))
  const acp = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const historyStartIndex = useAppStore((s) => s.historyStartIndexBySessionId[session.id] ?? 0)
  const searchNavigation = useAppStore((s) => s.chatSearchNavigation?.chatId === session.id ? s.chatSearchNavigation : null)
  const searchNavigationRef = useRef(searchNavigation)
  searchNavigationRef.current = searchNavigation
  const storedHistoryError = useAppStore((s) => s.chatHistoryErrorBySessionId[session.id] ?? '')
  const historyPageMissing = useAppStore((s) => s.messages[session.id] === undefined)
  const historyRead = useAppStore((s) => s.chatHistoryReadBySessionId?.[session.id])
  const historyReadPending = Boolean(historyRead && historyRead.birth === session.createdAt.getTime())
  const chatHistoryError = storedHistoryError || (historyPageMissing && (session.messageCount ?? 0) > 0 && !historyReadPending && searchNavigation?.phase !== 'loading'
    ? 'Saved chat history has not loaded. Retry to refresh it.' : '')
  const outgoingScopeKey = outgoingScope(session.id, session.createdAt)
  const currentOutgoingBirth = useAppStore((state) => state.sessions.find((chat) => chat.id === session.id)?.createdAt.getTime())
  const readScopedAttempts = useCallback(() => readOutgoingAttempts(currentOutgoingBirth === session.createdAt.getTime() ? outgoingScopeKey : ''),
    [outgoingScopeKey, currentOutgoingBirth, session.createdAt])
  const outgoingAttempts = useSyncExternalStore(subscribeOutgoingAttempts, readScopedAttempts, readScopedAttempts)
  // A missing page is not evidence of work. Retained input stays visible without a replacement skeleton.
  const hasRetainedInput = outgoingAttempts.length > 0 || Boolean(acp?.queuedPrompts?.length ||
    acp?.pendingDirectPrompts?.length || acp?.unsentDirectPrompts?.length)
  const historyLoading = historyReadPending && messages.length === 0 && !chatHistoryError &&
    (session.messageCount ?? 0) > 0 && !hasRetainedInput
  const historyIncomplete = historyLoading || (historyPageMissing && (session.messageCount ?? 0) > 0)
  const unsentProjection = useMemo(() => projectUnsentPrompts(messages, historyStartIndex, acp?.unsentMessageIndices ?? [], acp?.recoverableUnsentMessageIndices ?? [], acp?.queuedPrompts ?? [], acp?.pendingDirectPrompts ?? []),
    [messages, historyStartIndex, acp?.unsentMessageIndices, acp?.recoverableUnsentMessageIndices, acp?.queuedPrompts, acp?.pendingDirectPrompts])
  const unsentByLoadedIndex = useMemo(() => new Map(unsentProjection.prompts.map((prompt) => [prompt.messageIndex - historyStartIndex, prompt])),
    [unsentProjection.prompts, historyStartIndex])
  // Direct inputs with no materialized native message keep receipt id/revision identity at the transcript tail.
  const outgoingReceipts = useMemo<UnsentPrompt[]>(() => [
    ...(acp?.unsentDirectPrompts ?? []).map((prompt, index) => ({
      recoveryPending: true, messageIndex: -1 - index, text: prompt.text, attachments: messageAttachments(prompt), retryable: true,
      receipt: prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined,
    })),
    ...(acp?.pendingDirectPrompts ?? []).filter((prompt) => Boolean(sanitizeNativeInputId(prompt.nativeInputId)) || !messages.some((message, index) =>
      message.role === 'user' && promptReferencesMessage(prompt, historyStartIndex + index, message))).map((prompt, index) => ({
      sending: true, recoveryPending: true, messageIndex: -1 - (acp?.unsentDirectPrompts?.length ?? 0) - index,
      text: prompt.text, attachments: messageAttachments(prompt), retryable: false,
      receipt: prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined,
    })),
  ], [acp?.unsentDirectPrompts, acp?.pendingDirectPrompts, messages, historyStartIndex])
  // New store-correlated actions bind only one valid native input ID with their exact request origin.
  // Legacy attempts retain their existing exact-byte association below; neither path invents delivery or Retry authority.
  // Once a strong input ID is bound, later records follow that ID rather than text, index or timestamp.
  const outgoingPresentation = useMemo(() => {
    const exact = (attempt: OutgoingAttempt, text: string, attachments: Attachment[]) => text === attempt.text && attachmentsEquivalent(attachments, attempt.attachments)
    const unique = (list: OutgoingAttempt[]) => list.length === 1 ? list[0] : undefined
    const byLoadedIndex = new Map<number, OutgoingAttempt>()
    const byReceiptId = new Map<string, OutgoingAttempt>()
    const represented = new Set<string>()
    const patches = new Map<string, Partial<OutgoingAttempt>>()
    const writeCompletedKeys = new Set<string>()
    const retainedUnknownKeys = new Set<string>()
    const receiptKey = (prompt: { id?: string; revision?: number }) => JSON.stringify([prompt.id, prompt.revision])
    const directReceiptByLoadedIndex = new Map<number, { prompt: AcpQueuedPrompt; sending: boolean }>()
    const receiptNativeIndexByKey = new Map<string, number>()
    const bindNative = (attempt: OutgoingAttempt, loadedIndex: number) => {
      const message = messages[loadedIndex]
      byLoadedIndex.set(loadedIndex, attempt)
      represented.add(attempt.key)
      const binding = { kind: 'native' as const, messageId: message.id, nativeIndex: historyStartIndex + loadedIndex }
      const inputId = attempt.actionCorrelationId ? sanitizeNativeInputId(message.nativeInputId) : undefined
      const direct = directReceiptByLoadedIndex.get(loadedIndex)
      if (direct) byReceiptId.set(direct.prompt.id!, attempt)
      if (!attempt.recordedManagedInput && !direct && inputId && message.originRequestId === attempt.actionCorrelationId && message.inputWriteCompleted === true) writeCompletedKeys.add(attempt.key)
      const current = attempt.binding
      if (!attempt.materialized || current?.kind !== 'native' || current.messageId !== message.id || current.nativeIndex !== binding.nativeIndex ||
          attempt.nativeInputId !== inputId || attempt.text !== message.content || !attachmentsEquivalent(message.attachments ?? [], attempt.attachments) ||
          (attempt.recordedManagedInput && !attempt.recordedManagedInput.associated))
        patches.set(attempt.key, { binding, materialized: true, nativeInputId: inputId, text: message.content, attachments: message.attachments ?? [],
          ...(attempt.recordedManagedInput ? { recordedManagedInput: { ...attempt.recordedManagedInput, associated: true } } : {}) })
    }
    const queued = acp?.queuedPrompts ?? []
    const receipts = [...(acp?.pendingDirectPrompts ?? []), ...(acp?.unsentDirectPrompts ?? []), ...queued]
    const bindReceipt = (attempt: OutgoingAttempt, prompt: AcpQueuedPrompt) => {
      represented.add(attempt.key)
      const inputId = attempt.actionCorrelationId ? sanitizeNativeInputId(prompt.nativeInputId) : undefined
      // A real future queue entry stays in the queue; only direct receipts take over the transcript row.
      if (queued.includes(prompt)) {
        if (inputId && attempt.nativeInputId !== inputId) patches.set(attempt.key, { nativeInputId: inputId })
        return
      }
      byReceiptId.set(prompt.id!, attempt)
      const attachments = messageAttachments(prompt)
      if (!attempt.materialized || attempt.binding?.kind !== 'receipt' || attempt.binding.id !== prompt.id || attempt.nativeInputId !== inputId || attempt.text !== prompt.text || !attachmentsEquivalent(attachments, attempt.attachments))
        patches.set(attempt.key, { binding: { kind: 'receipt', id: prompt.id! }, materialized: true, nativeInputId: inputId, text: prompt.text, attachments })
    }
    type InputRecord = { id: string; origin?: string; loadedIndex?: number; prompt?: AcpQueuedPrompt }
    const recordsByInput = new Map<string, InputRecord[]>()
    const inputsByOrigin = new Map<string, Set<string>>()
    const attemptsByOrigin = new Map<string, number>()
    const addInput = (record: InputRecord) => {
      const sameInput = recordsByInput.get(record.id)
      if (sameInput) sameInput.push(record)
      else recordsByInput.set(record.id, [record])
      if (record.origin) {
        const ids = inputsByOrigin.get(record.origin) ?? new Set<string>()
        ids.add(record.id)
        inputsByOrigin.set(record.origin, ids)
      }
    }
    messages.forEach((message, loadedIndex) => {
      const id = sanitizeNativeInputId(message.nativeInputId)
      if (id) addInput({ id, origin: sanitizeOriginRequestId(message.originRequestId), loadedIndex })
    })
    receipts.forEach((prompt) => {
      const id = sanitizeNativeInputId(prompt.nativeInputId)
      if (id) addInput({ id, origin: sanitizeOriginRequestId(prompt.originRequestId), prompt })
    })
    outgoingAttempts.forEach((attempt) => {
      if (attempt.actionCorrelationId && !attempt.retired) attemptsByOrigin.set(attempt.actionCorrelationId, (attemptsByOrigin.get(attempt.actionCorrelationId) ?? 0) + 1)
    })
    // Exact producer identity selects candidates; byte/resource equality is an additional conservation guard,
    // never an alias finder. A future queue entry or conflicting/duplicate carrier keeps its own representation.
    for (const records of recordsByInput.values()) {
      const native = records.filter((record) => record.loadedIndex !== undefined)
      const direct = records.filter((record) => record.prompt && !queued.includes(record.prompt))
      if (records.length !== 2 || native.length !== 1 || direct.length !== 1) continue
      const { loadedIndex } = native[0]
      const prompt = direct[0].prompt!
      const message = messages[loadedIndex!]
      if (message.role !== 'user' || !native[0].origin || native[0].origin !== direct[0].origin ||
          !prompt.id || !Number.isSafeInteger(prompt.revision) || prompt.revision! < 1 ||
          receipts.filter((other) => receiptKey(other) === receiptKey(prompt)).length !== 1 ||
          message.content !== prompt.text || !attachmentsEquivalent(message.attachments ?? [], messageAttachments(prompt))) continue
      directReceiptByLoadedIndex.set(loadedIndex!, { prompt, sending: (acp?.pendingDirectPrompts ?? []).includes(prompt) })
      receiptNativeIndexByKey.set(receiptKey(prompt), loadedIndex!)
    }
    // Legacy receipts have no per-input ID. Only the native producer's validated raw-row reference
    // may share their representation; text, time or an index alone cannot discover an alias.
    for (const prompt of acp?.unsentDirectPrompts ?? []) {
      const nativeIndex = prompt.existingUserMessageIndex
      if (sanitizeNativeInputId(prompt.nativeInputId) || !prompt.id || !Number.isSafeInteger(prompt.revision) || prompt.revision! < 1 ||
          !prompt.referencedMessageCreatedAt || !Number.isSafeInteger(nativeIndex) || nativeIndex! < historyStartIndex ||
          receipts.filter(other => other.existingUserMessageIndex === nativeIndex).length !== 1) continue
      const loadedIndex = nativeIndex! - historyStartIndex
      const message = messages[loadedIndex]
      if (!message || sanitizeNativeInputId(message.nativeInputId) || message.role !== 'user' || !message.acpPromptNotSent || message.acpPromptDiscarded ||
          message.createdAt.getTime() !== Date.parse(prompt.referencedMessageCreatedAt) ||
          message.content !== prompt.text || !attachmentsEquivalent(message.attachments ?? [], messageAttachments(prompt))) continue
      directReceiptByLoadedIndex.set(loadedIndex, { prompt, sending: false })
      receiptNativeIndexByKey.set(receiptKey(prompt), loadedIndex)
    }
    const correlatedInput = (attempt: OutgoingAttempt): InputRecord | undefined => {
      const origin = sanitizeOriginRequestId(attempt.actionCorrelationId)
      if (!origin || attemptsByOrigin.get(origin) !== 1) return
      const ids = inputsByOrigin.get(origin)
      // First alias considers only actual producer IDs not already observed at capture. Correlation is not
      // idempotency: a reused origin on an old record cannot settle this action or veto its one fresh input.
      const eligibleIds = [...(ids ?? [])].filter((id) => !attempt.nativeInputIdsAtCapture?.has(id))
      const id = attempt.nativeInputId ?? (eligibleIds.length === 1 ? eligibleIds[0] : undefined)
      if (!id) return
      const sameInput = recordsByInput.get(id) ?? []
      if (sameInput.some((record) => record.origin !== origin)) return
      const native = sameInput.filter((record) => record.loadedIndex !== undefined)
      const queuedRecords = sameInput.filter((record) => record.prompt !== undefined)
      if (native.length > 1 || queuedRecords.length > 1) return
      if (native.length === 1) return messages[native[0].loadedIndex!].role === 'user' ? native[0] : undefined
      const record = queuedRecords[0]
      if (record?.prompt?.id && record.prompt.revision && !attempt.receiptIds.has(record.prompt.id)) return record
    }
    const unbound: OutgoingAttempt[] = []
    for (const attempt of outgoingAttempts) {
      if (attempt.actionCorrelationId !== undefined) {
        if (attempt.retired) continue
        const recorded = attempt.recordedManagedInput
        const managedRows = recorded?.managedOriginRunId
          ? messages.flatMap((message, index) => message.managedOriginRunId === recorded.managedOriginRunId ? [index] : []) : []
        const managedOwners = recorded?.managedOriginRunId ? outgoingAttempts.filter((candidate) => !candidate.retired &&
          candidate.recordedManagedInput?.managedOriginRunId === recorded.managedOriginRunId).length : 0
        const recordedIndex = recorded && managedRows.length === 1 && managedOwners === 1 && messages[managedRows[0]].role === 'user' &&
          (recorded.associated || messages[managedRows[0]] === recorded.message) ? managedRows[0] : -1
        // First association was the actual validated receipt object. The locked, scoped managed origin then
        // follows only one user record; conflicts never fall back to content, index, time or another input ID.
        const record = recorded ? undefined : correlatedInput(attempt)
        if (recordedIndex >= 0) bindNative(attempt, recordedIndex)
        else if (record?.loadedIndex !== undefined) bindNative(attempt, record.loadedIndex)
        else if (record?.prompt) bindReceipt(attempt, record.prompt)
        else if (attempt.materialized) {
          const lastIndex = attempt.binding?.kind === 'native' ? attempt.binding.nativeIndex : undefined
          const outsideLoadedWindow = lastIndex !== undefined && messages.length > 0 &&
            (lastIndex < historyStartIndex || lastIndex >= historyStartIndex + messages.length) &&
            Number.isSafeInteger(session.messageCount) && session.messageCount! > lastIndex &&
            !(attempt.nativeInputId && recordsByInput.has(attempt.nativeInputId)) &&
            !inputsByOrigin.has(attempt.actionCorrelationId) && managedRows.length === 0
          if (outsideLoadedWindow) represented.add(attempt.key)
          else retainedUnknownKeys.add(attempt.key) // Lost/conflicting identity is not proof of removal or delivery.
        }
        // Correlated actions never enter the legacy text/index/time association path below.
        continue
      }
      const binding = attempt.binding
      if (binding?.kind === 'native') {
        const loadedIndex = binding.nativeIndex - historyStartIndex
        const message = messages[loadedIndex]
        // The store's ACK placeholder (`sent-user-*`) may be replaced by the native row at the same index with the same bytes.
        if (message?.role === 'user' && (message.id === binding.messageId || (binding.messageId.startsWith('sent-user-') && exact(attempt, message.content, message.attachments ?? [])))) bindNative(attempt, loadedIndex)
        else if (loadedIndex >= 0 && loadedIndex < messages.length) patches.set(attempt.key, { binding: undefined, retired: true })
        else represented.add(attempt.key) // Bound row is outside the loaded history window.
        continue
      }
      if (binding?.kind === 'receipt') {
        const prompt = receipts.find((candidate) => candidate.id === binding.id)
        if (prompt) { bindReceipt(attempt, prompt); continue }
        patches.set(attempt.key, { binding: undefined })
      }
      if (!attempt.retired) unbound.push(attempt)
    }
    const boundIndices = new Set(byLoadedIndex.keys())
    const candidates = new Map<number, OutgoingAttempt[]>()
    for (const attempt of unbound) {
      const loadedIndex = attempt.nativeIndex - historyStartIndex
      const message = messages[loadedIndex]
      if (!boundIndices.has(loadedIndex) && message?.role === 'user' && exact(attempt, message.content, message.attachments ?? [])) candidates.set(loadedIndex, [...(candidates.get(loadedIndex) ?? []), attempt])
    }
    for (const [loadedIndex, list] of candidates) {
      const attempt = unique(list)
      if (attempt) bindNative(attempt, loadedIndex)
    }
    const boundReceipts = new Set(byReceiptId.keys())
    const isNewExact = (attempt: OutgoingAttempt, prompt: AcpQueuedPrompt) => Boolean(prompt.id) && !boundReceipts.has(prompt.id!) && !attempt.receiptIds.has(prompt.id!) && exact(attempt, prompt.text, messageAttachments(prompt))
    for (const prompt of receipts) {
      if (!prompt.id) continue
      const attempt = unique(unbound.filter((candidate) => !represented.has(candidate.key) && isNewExact(candidate, prompt)))
      if (!attempt || receipts.some((other) => other !== prompt && isNewExact(attempt, other))) continue
      bindReceipt(attempt, prompt)
    }
    return { byLoadedIndex, byReceiptId, directReceiptByLoadedIndex, receiptNativeIndexByKey, writeCompletedKeys, retainedUnknownKeys, patches,
      tail: outgoingAttempts.filter((attempt) => !represented.has(attempt.key) && (!attempt.materialized || retainedUnknownKeys.has(attempt.key)) && !attempt.retired) }
  }, [outgoingAttempts, messages, historyStartIndex, session.messageCount, acp?.pendingDirectPrompts, acp?.unsentDirectPrompts, acp?.queuedPrompts])
  useEffect(() => {
    if (outgoingPresentation.patches.size) updateOutgoingAttempts(outgoingScopeKey, outgoingPresentation.patches)
  }, [outgoingPresentation, outgoingScopeKey])
  const navigableUserMessageIndices = useMemo(() => {
    if (unsentProjection.hiddenIndices.size === 0) return session.userMessageIndices
    const indices = session.userMessageIndices ?? messages.flatMap((message, index) => message.role === 'user' ? [historyStartIndex + index] : [])
    return indices.filter((index) => {
      const loadedIndex = index - historyStartIndex
      return loadedIndex < 0 || loadedIndex >= messages.length || !unsentProjection.hiddenIndices.has(loadedIndex)
    })
  }, [messages, historyStartIndex, session.userMessageIndices, unsentProjection])
  // Mirrors MessageNavigationRail's own presence rule so the gutter exists exactly when the rail does.
  const messageNavigationVisible = Boolean(session.messagesDigest) &&
    (navigableUserMessageIndices ?? messages.filter((message) => message.role === 'user')).length > 0
  // Queue-held and discarded inputs do not turn an otherwise empty chat into history; outgoing rows do.
  const composerCentred = showComposer && historyStartIndex === 0 && messages.length === unsentProjection.hiddenIndices.size &&
    outgoingReceipts.length === 0 && outgoingPresentation.tail.length === 0 && !chatHistoryError && !historyIncomplete
  const composerFormRef = useRef<HTMLFormElement>(null)
  const composerCentredRef = useRef(composerCentred)
  const composerTopRef = useRef(0)
  const composerSessionRef = useRef(session.id)
  // FLIP: when the first message moves the composer from centred to the bottom, animate the delta from its centred top.
  // Layout is only read while centred (an empty chat), never on commits of a streaming transcript.
  useLayoutEffect(() => {
    const form = composerFormRef.current
    const wasCentred = composerCentredRef.current && composerSessionRef.current === session.id
    composerCentredRef.current = composerCentred
    composerSessionRef.current = session.id
    if (!form || !showComposer) return
    if (composerCentred) { composerTopRef.current = form.getBoundingClientRect().top; return }
    if (!wasCentred) return
    const delta = composerTopRef.current - form.getBoundingClientRect().top
    if (Math.abs(delta) < 1 || (typeof window.matchMedia === 'function' && window.matchMedia('(prefers-reduced-motion: reduce)').matches)) return
    form.style.transition = 'none'
    form.style.transform = `translateY(${delta}px)`
    void form.offsetHeight
    form.style.transition = 'transform var(--dur-base) var(--ease-out-soft)'
    form.style.transform = ''
    form.addEventListener('transitionend', () => { form.style.transition = '' }, { once: true })
  })
  const loadSessionMessages = useAppStore((s) => s.loadSessionMessages)
  const loadOlderSessionMessages = useAppStore((s) => s.loadOlderSessionMessages)
  const retryChatHistory = useCallback(async (chatId: string) => {
    setChatHistoryRetryingIds((ids) => new Set(ids).add(chatId))
    try {
      await loadSessionMessages(chatId, true)
    } finally {
      setChatHistoryRetryingIds((ids) => {
        const next = new Set(ids)
        next.delete(chatId)
        return next
      })
    }
  }, [loadSessionMessages])
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
  const folderIcon = useAppStore((s) => session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.customIcon : undefined)
  const workingDisplayMode = useAppStore((s) => s.workingDisplayMode)
  const cli = useAppStore((s) => s.cliBindingBySessionId[session.id])
  const providers = useAppStore((s) => s.providers)
  const cliVersionManager = useAppStore((s) => s.cliVersionManager)
  const stageChatAttachments = useAppStore((s) => s.stageChatAttachments)
  const sendAcpPrompt = useAppStore((s) => s.sendAcpPrompt)
  // Takes already-formed input: goal, plan and Claude plan actions must not inherit composer references.
  const sendSubmittedPrompt = useCallback(async (chatId: string, submittedText: string, attachments?: Attachment[], steerNow?: boolean) => {
    // Display only: an idle direct send occupies its transcript slot before ACK and keeps it after settling. It never enters
    // messages[] or ACP state; ACK records admission only, and a failed request stays visible as unconfirmed.
    const state = useAppStore.getState()
    const binding = state.acpBindingBySessionId[chatId]
    const owner = state.sessions.find((candidate) => candidate.id === chatId)
    const scope = owner ? outgoingScope(chatId, owner.createdAt) : ''
    const attempt: OutgoingAttempt | null = binding?.processing || !owner ? null : {
      key: nextOutgoingKey(), text: submittedText.trim(),
      nativeIndex: Math.max((state.historyStartIndexBySessionId[chatId] ?? 0) + (state.messages[chatId]?.length ?? 0),
        state.sessions.find((candidate) => candidate.id === chatId)?.messageCount ?? 0),
      attachments: messageAttachments({ markdownStoreFiles: (state.markdownStoreAttachedBySessionId[chatId] ?? []).map((entry) => entry.filePath), attachments: attachments ?? [] }),
      receiptIds: new Set([...(binding?.pendingDirectPrompts ?? []), ...(binding?.unsentDirectPrompts ?? []), ...(binding?.queuedPrompts ?? [])].flatMap((prompt) => prompt.id ? [prompt.id] : [])),
      nativeInputIdsAtCapture: new Set([
        ...(state.messages[chatId] ?? []), ...(binding?.pendingDirectPrompts ?? []), ...(binding?.unsentDirectPrompts ?? []), ...(binding?.queuedPrompts ?? []),
      ].flatMap((record) => { const id = sanitizeNativeInputId(record.nativeInputId); return id ? [id] : [] })),
      createdAt: new Date(), phase: 'pending',
    }
    if (attempt) addOutgoingAttempt(scope, attempt)
    let startedRequestId: string | undefined
    const onStarted: AcpPromptPresentationObserver = (identity) => {
      if (!attempt || startedRequestId !== undefined || identity.sessionId !== chatId ||
          identity.createdAtMs !== owner?.createdAt.getTime() || !identity.requestId || identity.requestId.length > 128) return
      startedRequestId = identity.requestId
      // Existing presentation record only; the store calls this before IPC, never from its ACK callback.
      updateOutgoingAttempts(scope, new Map([[attempt.key, { actionCorrelationId: identity.requestId }]]))
    }
    let recordedInputObserved = false
    onStarted.onInputRecorded = (identity) => {
      const current = useAppStore.getState()
      const retained = attempt && readOutgoingAttempts(scope).find((candidate) => candidate.key === attempt.key)
      const managedOriginRunId = sanitizeManagedOriginRunId(identity.message.managedOriginRunId)
      if (!managedOriginRunId || !attempt || recordedInputObserved || retained?.retired || startedRequestId !== identity.requestId || identity.sessionId !== chatId ||
          identity.createdAtMs !== owner?.createdAt.getTime() || current.sessions.find((chat) => chat.id === chatId)?.createdAt.getTime() !== identity.createdAtMs ||
          identity.message.role !== 'user' || !current.messages[chatId]?.includes(identity.message)) return
      recordedInputObserved = true
      updateOutgoingAttempts(scope, new Map([[attempt.key, { recordedManagedInput: {
        // The guarded observer has already associated this exact projected object with this request.
        // Lock that observation now so canonical replacement need not wait for a React commit.
        message: identity.message, managedOriginRunId, associated: true,
      } }]]))
    }
    let accepted = false
    try {
      accepted = await sendAcpPrompt(chatId, submittedText, attachments, steerNow, onStarted)
    } finally {
      // Scoped outside ChatView: the outcome lands even if this view remounted or switched chats meanwhile.
      if (attempt) updateOutgoingAttempts(scope, new Map([[attempt.key, { phase: accepted ? 'accepted' : 'rejected' }]]))
    }
    return accepted
  }, [sendAcpPrompt])
  const sendComposerPrompt = useCallback(async (chatId: string, text: string, attachments?: Attachment[], steerNow?: boolean) => {
    const references = sanitizeContextReferences(readChatComposerDraft(chatId).references)
    const context = formatContextReferences(references)
    const submittedText = context ? `${text}\n\n${context}` : text
    const accepted = await sendSubmittedPrompt(chatId, submittedText, attachments, steerNow)
    if (accepted && references.length > 0 && useAppStore.getState().sessions.some((candidate) => candidate.id === chatId)) {
      const sentIds = new Set(references.map((reference) => reference.id))
      const current = readChatComposerDraft(chatId)
      writeChatComposerDraft(chatId, { ...current, references: (current.references ?? []).filter((reference) => !sentIds.has(reference.id)) })
    }
    return accepted
  }, [sendSubmittedPrompt])
  const getVcsCommitStatus = useAppStore((s) => s.getVcsCommitStatus)
  const getVcsFileDiff = useAppStore((s) => s.getVcsFileDiff)
  const repositoryChanges = useAppStore((s) => s.repositoryReviewBySessionId[session.id] ?? null)
  // Branch read once when the chat (or its workspace) is opened; see the composer branch effect below.
  const [composerVcsBranch, setComposerVcsBranch] = useState({ scope: '', branch: '' })
  const composerBranchScope = `${session.id}\n${workspaceDirectory}\n${session.executionHostId ?? ''}`
  // The composer's own read (refreshed after a branch switch) wins over the last completed-turn snapshot.
  const composerBranch = (composerVcsBranch.scope === composerBranchScope ? composerVcsBranch.branch : '')
    || (repositoryChanges?.workspaceDirectory === workspaceDirectory ? repositoryChanges.branchOrRevision : '')
    || session.workspaceBranchName
  const branchSwitching = useAppStore((s) => s.branchSwitching === true)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const removeQueuedAcpPrompt = useAppStore((s) => s.removeQueuedAcpPrompt)
  const retryUnsentAcpPrompt = useAppStore((s) => s.retryUnsentAcpPrompt)
  const discardUnsentMessage = useAppStore((s) => s.discardUnsentMessage)
  const editUnsentMessage = useAppStore((s) => s.editUnsentMessage)
  const resumeQueuedAcpPrompts = useAppStore((s) => s.resumeQueuedAcpPrompts)
  const steerQueuedAcpPrompt = useAppStore((s) => s.steerQueuedAcpPrompt)
  const editQueuedAcpPrompt = useAppStore((s) => s.editQueuedAcpPrompt)
  const reorderQueuedAcpPrompts = useAppStore((s) => s.reorderQueuedAcpPrompts)
  const branchFromMessage = useAppStore((s) => s.branchFromMessage)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const branchRootChatId = session.branchRootChatId || session.parentChatId || session.id
  const branchSessions = useAppStore(useShallow((s) => s.sessions
    .filter((candidate) => (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === branchRootChatId)
    .sort((a, b) => a.createdAt.getTime() - b.createdAt.getTime())))
  const retryFailedMessage = useAppStore((s) => s.retryFailedMessage)
  const cancelAcpTurn = useAppStore((s) => s.cancelAcpTurn)
  const stopAcpSession = useAppStore((s) => s.stopAcpSession)
  const resolveAcpPermission = useAppStore((s) => s.resolveAcpPermission)
  const resolveAcpUserInput = useAppStore((s) => s.resolveAcpUserInput)
  const setSessionProvider = useAppStore((s) => s.setSessionProvider)
  const setSessionModel = useAppStore((s) => s.setSessionModel)
	const setSessionReviewerModel = useAppStore((s) => s.setSessionReviewerModel)
	const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const refreshDisplayedModels = useCallback(() => discoverProviderModels(session.id),
    [discoverProviderModels, session.id, session.createdAt.getTime()])
  const setSessionCodexOptions = useAppStore((s) => s.setSessionCodexOptions)
  const setAcpConfigOption = useAppStore((s) => s.setAcpConfigOption)
  const setSessionApprovalMode = useAppStore((s) => s.setSessionApprovalMode)
  const setSessionUamAgent = useAppStore((s) => s.setSessionUamAgent)
  const refreshUamAgents = useAppStore((s) => s.refreshUamAgents)
  const catalogUamAgents = useAppStore(useShallow((s) => s.uamAgentsBySessionId[session.id] ?? []))
  const favoriteUamAgentIds = useAppStore(useShallow((s) => s.favoriteUamAgentIds))
  const uamAgentCycleShortcut = useAppStore((s) => s.uamAgentCycleShortcut)
  const setSessionCommandSafetyTier = useAppStore((s) => s.setSessionCommandSafetyTier)
  const setSessionComputerUseEnabled = useAppStore((s) => s.setSessionComputerUseEnabled)
  const setSessionComputerUseControl = useAppStore((s) => s.setSessionComputerUseControl)
  const computerUseMode = COMPUTER_USE_ENABLED && Boolean(session.computerUseEnabled)
  const remoteComputerUseDisabled = (session.executionHostId ?? 'local') !== 'local'
  const recoveryHost = useAppStore((s) => s.executionHosts.find((host) => host.id === session.executionHostId))
  const workspaceFolders = useAppStore((s) => s.folders)
  const remoteContextKey = JSON.stringify([session.id, session.createdAt.getTime(), session.executionHostId, workspaceDirectory, recoveryHost?.sshAlias, recoveryHost?.platform, recoveryHost?.runnerDirectory, recoveryHost?.runnerVersion, recoveryHost?.runnerProtocolVersion])
  useEffect(() => { setRemoteMoveFolderId(''); setRemoteBrowseKey('') }, [remoteContextKey])
  const setSessionMemoryLevel = useAppStore((s) => s.setSessionMemoryLevel)
  const setSessionSmallModelMode = useAppStore((s) => s.setSessionSmallModelMode)
  const configuredApprovalMode = useAppStore((s) => s.sessions.find((candidate) => candidate.id === session.id)?.approvalMode)
  const configuredUamAgentId = useAppStore((s) => s.sessions.find((candidate) => candidate.id === session.id)?.uamAgentId)

  useEffect(() => {
    void refreshUamAgents(session.id)
  }, [refreshUamAgents, session.id, workspaceDirectory])
  useEffect(() => {
    if (isCompanionContext() && !session.importedReadOnly) {
      void discoverProviderModels(session.id)
    }
  }, [discoverProviderModels, session.id, session.modelId, session.uamAgentId, session.importedReadOnly])

  const openSessionWorkspace = useAppStore((s) => s.openSessionWorkspace)
  const openSessionWorkspaceEditor = useAppStore((s) => s.openSessionWorkspaceEditor)
  const openSessionTerminal = useAppStore((s) => s.openSessionTerminal)
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
  const transcriptContentRef = useRef<HTMLDivElement>(null)
  // Composer floats over the transcript; its measured height pads the transcript end.
  const composerDockRef = useRef<HTMLDivElement>(null)
  const isNearBottomRef = useRef(true)
  const scrollFrameRef = useRef<number | null>(null)
  const showScrollToBottomRef = useRef(false)
  const [showScrollToBottom, setShowScrollToBottom] = useState(false)
  const fileInputRef = useRef<HTMLInputElement>(null)
  const composerTextareaRef = useRef<HTMLTextAreaElement>(null)
  const modelMenuRef = useRef<HTMLDivElement>(null)
  const workspaceMenuRef = useRef<HTMLDivElement>(null)
  const dictationActiveRef = useRef(false)
  const dictationIdRef = useRef('')
  const dictationBaseDraftRef = useRef('')
  const dictationFinalTextRef = useRef('')
  const dictationInterimTextRef = useRef('')
  const dictationHadErrorRef = useRef(false)
  const dictationSubmitAfterStopRef = useRef(false)
  const submitDictatedPromptRef = useRef<(prompt: string) => void>(() => {})
  const submitInFlightRef = useRef(false)
  const compactRequestIdRef = useRef<string | null>(null)
  const currentSessionIdRef = useRef(session.id)
  const mountedRef = useRef(true)
  const composerFocusOwner = JSON.stringify([session.id, session.createdAt.getTime(), navigationKey])
  const previousComposerVisibility = useRef({ owner: composerFocusOwner, visible: showComposer })
  useLayoutEffect(() => {
    const previous = previousComposerVisibility.current
    previousComposerVisibility.current = { owner: composerFocusOwner, visible: showComposer }
    // Use only this ChatView's existing scoped ref; global focus cannot distinguish nested child panes.
    // Mounting a normal main chat (default true) does not gain autofocus.
    if (previous.owner === composerFocusOwner && !previous.visible && showComposer && currentSessionIdRef.current === session.id) {
      const input = composerTextareaRef.current
      if (input && !input.disabled && !input.readOnly) input.focus({ preventScroll: true })
    }
    if (!showComposer && editingMessageIndex !== null) composerTextareaRef.current?.focus({ preventScroll: true })
  }, [showComposer, editingMessageIndex, composerFocusOwner])
  const removedAttachmentIdsRef = useRef(new Set<string>())

  useEffect(() => subscribeComposerDraftAcknowledgements((chatId, createdAt, submitted) => {
    if (chatId !== session.id || createdAt !== session.createdAt.getTime()) return
    const next = withoutAcknowledgedComposerDraft(composerDraftRef.current, submitted)
    // Timers and pagehide read this same ref, so they cannot restore an acknowledged draft.
    composerDraftRef.current = next
    setDraft((current) => current === submitted.text ? '' : current)
    setComposerAttachments((current) => current.filter((attachment) => !submitted.attachments.some((sent) =>
      attachmentsEquivalent([attachment], [sent]))))
  }), [session.id, session.createdAt])

  useEffect(() => {
    mountedRef.current = true
    return () => {
      mountedRef.current = false
      if (scrollFrameRef.current !== null) window.cancelAnimationFrame(scrollFrameRef.current)
    }
  }, [])

  useEffect(() => {
    currentSessionIdRef.current = session.id
    isNearBottomRef.current = restoredViewport?.nearBottom ?? true
    showScrollToBottomRef.current = false
    setShowScrollToBottom(false)
    submitInFlightRef.current = false
    setSubmitting(false)
    setCompacting(false)
    compactRequestIdRef.current = null
    setSelectedRepositoryFile(null)
    setProviderHandoffTargetId('')
    setConfirmYolo(false)
    setRollbackConfirmation(null)
  }, [session.id])

  const renderedCountRef = useRef(renderedMessageCount)
  renderedCountRef.current = renderedMessageCount
  useLayoutEffect(() => {
    const region = scrollRef.current
    const saved = restoredViewport
    if (region && saved) {
      region.scrollTop = saved.top
      isNearBottomRef.current = saved.nearBottom
    }
    return () => {
      if (!region || !navigationKey) return
      parentViewportVisits.delete(viewportKey)
      parentViewportVisits.set(viewportKey, { ...viewportDetails.current, top: region.scrollTop, nearBottom: isNearBottomRef.current, renderedCount: renderedCountRef.current })
      if (parentViewportVisits.size > 64) parentViewportVisits.delete(parentViewportVisits.keys().next().value!)
    }
  }, [viewportKey])

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

  const turnEvents = useMemo(() => acp?.turnEvents ?? [], [acp?.turnEvents])
  const liveTools = useMemo(() => acp?.toolCalls ?? [], [acp?.toolCalls])
  const livePlanEntries = useMemo(() => acp?.planEntries ?? [], [acp?.planEntries])
  const selectLiveTool = useCallback((toolId: string) => setSelectedToolCallRef({ id: toolId }), [])
  const resolveLivePermission = useCallback((requestId: string, optionId: string) => resolveAcpPermission(session.id, requestId, optionId), [resolveAcpPermission, session.id])
  const cancelLiveTurn = useCallback(() => { void cancelAcpTurn(session.id) }, [cancelAcpTurn, session.id])
  const stopLiveRuntime = useCallback(() => { void stopAcpSession(session.id) }, [stopAcpSession, session.id])
  const firstTurnEvent = turnEvents.find((event) => event.type === 'assistant_text' ? event.text.length > 0 : true)
  const turnAssistantMessageIndex = acp?.turnAssistantMessageIndex === undefined || acp.turnAssistantMessageIndex < 0
    ? -1 : acp.turnAssistantMessageIndex - historyStartIndex
  const turnUserMessageIndex = acp?.turnUserMessageIndex === undefined || acp.turnUserMessageIndex < 0
    ? -1 : acp.turnUserMessageIndex - historyStartIndex
  const turnSerial = acp?.turnSerial ?? 0
  const liveWorkDisclosure = useMemo<WorkTraceDisclosureState>(() => restoredViewport?.liveDisclosure?.serial === turnSerial ? restoredViewport.liveDisclosure.state : {}, [viewportKey, turnSerial])
  const savedWorkDisclosures = useMemo(() => restoredViewport?.workDisclosures ?? new Map<string, WorkTraceDisclosureState>(), [viewportKey])
  const expandWorkTraces = useAppStore((state) => state.expandWorkTraces)
  const collapsibleWorkSections = useAppStore((state) => state.collapsibleWorkSections)
  const [sectionChoices, setSectionChoices] = useState<{ sessionId: string; expanded: Record<string, boolean> }>(() => restoredViewport?.sectionChoices ?? { sessionId: session.id, expanded: {} })
  viewportDetails.current = { subagentDisclosures, workDisclosures: savedWorkDisclosures, liveDisclosure: { serial: turnSerial, state: liveWorkDisclosure }, sectionChoices }
  const earliestRenderedMessageIndex = Math.max(0, messages.length - renderedMessageCount)
  const workSections = useMemo(
    () => buildVisibleWorkSections(messages, earliestRenderedMessageIndex),
    [earliestRenderedMessageIndex, messages]
  )
  const sectionControls = useMemo(() => {
    const controls = new Map<string, { expanded: boolean; toggle: () => void }>()
    return (id: string) => {
      if (!controls.has(id)) {
        const expanded = (sectionChoices.sessionId === session.id ? sectionChoices.expanded[id] : undefined) ?? expandWorkTraces
        controls.set(id, { expanded, toggle: () => setSectionChoices((previous) => ({ sessionId: session.id, expanded: { ...(previous.sessionId === session.id ? previous.expanded : {}), [id]: !expanded } })) })
      }
      return controls.get(id)!
    }
  }, [session.id, sectionChoices, expandWorkTraces])
  const { latestUserMessageIndex, retryInPlaceMessageIndex, latestAssistantMessageIndex } = useMemo(() => {
    const latestUserMessageIndex = lastMessageIndexWithRole(messages, 'user')
    return {
      latestUserMessageIndex,
      retryInPlaceMessageIndex: canRetryMessageInPlace(messages, latestUserMessageIndex) ? latestUserMessageIndex : -1,
      latestAssistantMessageIndex: lastMessageIndexWithRole(messages, 'assistant'),
    }
  }, [messages])
  const activeWorkMessageIndex = turnUserMessageIndex >= 0 ? turnUserMessageIndex : latestUserMessageIndex
  const activeWorkSectionFirstIndex = activeWorkMessageIndex >= 0 && activeWorkMessageIndex < messages.length
    ? workSections.get(activeWorkMessageIndex)?.firstIndex ?? findWorkSectionStartIndex(messages, activeWorkMessageIndex)
    : -1
  const activeWorkSectionId = messages[activeWorkSectionFirstIndex]?.id
  const redundantStreamingMessageIndex =
    turnEvents.length > 0 &&
    messages[messages.length - 1]?.isStreaming &&
    turnAssistantMessageIndex === messages.length - 2
      ? messages.length - 1
      : -1
  const turnAssistantMessageMatches =
    turnAssistantMessageIndex >= earliestRenderedMessageIndex &&
    (turnAssistantMessageIndex === messages.length - 1 || redundantStreamingMessageIndex >= 0) &&
    messages[turnAssistantMessageIndex]?.role === 'assistant'
  const turnUserMessageMatches =
    turnUserMessageIndex >= earliestRenderedMessageIndex &&
    turnUserMessageIndex === latestUserMessageIndex &&
    messages[turnUserMessageIndex]?.role === 'user' &&
    (turnUserMessageIndex === messages.length - 1 ||
      (turnAssistantMessageMatches && turnUserMessageIndex < turnAssistantMessageIndex))
  const completedTurnAssistantText = useMemo(() => acp?.processing
    ? ''
    : turnEvents.reduce(
        (text, event) => event.type === 'assistant_text' ? text + event.text : text,
        ''
      ).trim(), [acp?.processing, turnEvents])
  const completedFallbackAlreadyPersisted =
    !acp?.processing &&
    completedTurnAssistantText.length > 0 &&
    latestAssistantMessageIndex > latestUserMessageIndex &&
    messages[latestAssistantMessageIndex]?.content.trim() === completedTurnAssistantText
  // Bind only a confirmed assistant; steering can put user messages between turn rows.
  const disclosureAssistant = turnAssistantMessageMatches
    ? messages[turnAssistantMessageIndex]
    : completedFallbackAlreadyPersisted ? messages[latestAssistantMessageIndex] : undefined
  if (disclosureAssistant) savedWorkDisclosures.set(disclosureAssistant.id, liveWorkDisclosure)
  const turnClockStart = useMemo(() => acp?.processingStartedAtMs || Date.now(), [session.id, turnSerial, acp?.processingStartedAtMs, acp?.processing])
  const turnWorkedSeconds = acp?.processing
    ? Math.max(0, (Date.now() - turnClockStart) / 1000)
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
    if (isNearBottomRef.current && !searchNavigation) {
      bottomRef.current?.scrollIntoView?.({ block: 'end' })
    }
  }, [
    searchNavigation,
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

  useEffect(() => {
    const dock = composerDockRef.current
    if (typeof ResizeObserver === 'undefined' || (showComposer && !dock)) return
    // Written as a CSS variable, not React state, so composer growth never re-renders the transcript.
    let lastHeight = -1
    const observer = new ResizeObserver(() => {
      const height = dock?.offsetHeight ?? 0
      if (height !== lastHeight) {
        lastHeight = height
        dock?.parentElement?.style.setProperty('--composer-dock-h', `${height}px`)
      }
      const el = scrollRef.current
      if (el && isNearBottomRef.current && !searchNavigationRef.current) el.scrollTop = el.scrollHeight
    })
    if (dock) observer.observe(dock)
    // Content and viewport changes also move the end, even when the dock height is unchanged.
    if (transcriptContentRef.current) observer.observe(transcriptContentRef.current)
    if (scrollRef.current) observer.observe(scrollRef.current)
    return () => observer.disconnect()
  }, [showComposer])

  const handleScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    // Pause following immediately so a resize in this frame cannot override an upward scroll.
    isNearBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < SCROLL_NEAR_BOTTOM_THRESHOLD
	if (scrollFrameRef.current !== null) return
	scrollFrameRef.current = window.requestAnimationFrame(() => {
	  scrollFrameRef.current = null
	  const showButton = !isNearBottomRef.current
	  if (showScrollToBottomRef.current === showButton) return
	  showScrollToBottomRef.current = showButton
	  setShowScrollToBottom(showButton)
	})
  }, [])

  useEffect(() => {
    if (!searchNavigation || searchNavigation.phase !== 'ready' || searchNavigation.anchored) return
    const state = useAppStore.getState()
    if (state.activeSessionId !== session.id || state.chatSearchNavigation?.generation !== searchNavigation.generation ||
        state.chatSearchNavigation.chatId !== session.id) return
    const target = transcriptContentRef.current?.querySelector<HTMLElement>(`[data-message-index="${searchNavigation.messageIndex}"]`)
    if (!target) return
    isNearBottomRef.current = false
    target.scrollIntoView?.({ block: 'center' })
    useAppStore.getState().consumeChatSearchJump(session.id, searchNavigation.generation)
  }, [searchNavigation, messages, historyStartIndex, session.id, unsentProjection])

  const scrollToBottom = useCallback(() => {
    const followLatest = () => {
      if (currentSessionIdRef.current !== session.id || useAppStore.getState().chatSearchNavigation) return
      isNearBottomRef.current = true
      showScrollToBottomRef.current = false
      setShowScrollToBottom(false)
      bottomRef.current?.scrollIntoView?.({ block: 'end', behavior: 'smooth' })
    }
    if (searchNavigationRef.current) {
      void useAppStore.getState().clearChatSearchNavigation().then(() => {
        const state = useAppStore.getState()
        const count = state.sessions.find((candidate) => candidate.id === session.id)?.messageCount
        if (state.activeSessionId === session.id && count === (state.historyStartIndexBySessionId[session.id] ?? 0) + (state.messages[session.id]?.length ?? 0)) followLatest()
      })
    } else followLatest()
  }, [session.id])

  useEffect(() => {
    if (selectedToolCallRef && !selectedToolCall) {
      setSelectedToolCallRef(null)
    }
  }, [selectedToolCall, selectedToolCallRef])

  useEffect(() => {
    setSelectedToolCallRef(null)
  }, [turnSerial])

  useEffect(() => {
    setSteering(false)
    setAttachmentError('')
    setGoalError('')
    goalMutationGenerationRef.current += 1
    goalMutationOwnerRef.current = null
    goalMutationInFlightRef.current = false
    setGoalSubmitting(false)
    setWorkspaceFeedback(null)
    setWorkspaceActionBusy(false)
    setWorkspaceMenuOpen(false)
    setGoalArmNextMessage(false)
    setSlashMessage('')
    setPermissionMenuOpen(false)
    messageBranchGenerationRef.current += 1
    setEditingMessageIndex(null)
    setEditingMessageText('')
    setEditingMessageAttachments([])
    setEditingMessageSource({ kind: 'branch' })
    unsentEditInFlightRef.current = null
    setBranchingMessageIndex(null)
    setMessageBranchError('')
    setClaudePlanPrompt(null)
    setRenderedMessageCount(restoredViewport?.renderedCount ?? (isCompanionContext() ? COMPANION_INITIAL_RENDERED_MESSAGES : INITIAL_RENDERED_MESSAGES))
    setOlderHistoryLoading(false)
    setDictationState('idle')
    setDictationError('')
  }, [session.id])

  useEffect(() => {
    if (!acp?.lastError && !acp?.promptActionError) setDismissedAcpErrorKey('')
  }, [acp?.lastError, acp?.promptActionError])

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
      if (target instanceof Element && target.closest('[aria-modal="true"]')) return
      if (!goalsTriggerRef.current?.contains(target)) setGoalsPopupOwner(null)

      if (modelOpen && modelMenuRef.current && !modelMenuRef.current.contains(target)) {
        setModelOpen(false)
      }

      if (workspaceMenuOpen && workspaceMenuRef.current && !workspaceMenuRef.current.contains(target)) {
        setWorkspaceMenuOpen(false)
      }
    }

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key !== 'Escape' || event.defaultPrevented || getTopmostModal()) return
      setModelOpen(false)
      setWorkspaceMenuOpen(false)
      setGoalsPopupOwner(null)
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
    if (!showComposerRef.current) return
    if (isCompanionContext()) { setAttachmentError('Add attachments from the desktop app.'); return }
    const stagingSessionId = session.id
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

      const staged = await stageChatAttachments(stagingSessionId, items)
      if (!mountedRef.current || currentSessionIdRef.current !== stagingSessionId) {
        if (!useAppStore.getState().sessions.some((candidate) => candidate.id === stagingSessionId)) return
        const stored = readChatComposerDraft(stagingSessionId)
        writeChatComposerDraft(stagingSessionId, {
          ...stored,
          attachments: [...stored.attachments, ...staged.filter((candidate) =>
            !removedAttachmentIdsRef.current.has(candidate.id) &&
            !stored.attachments.some((attachment) => attachment.id === candidate.id)
          )],
        })
        return
      }
      setComposerAttachments((current) =>
        current.map((attachment) => {
          const replacement = staged.find((candidate) => candidate.id === attachment.id)
          return replacement
            ? { ...replacement, status: 'ready' as LocalAttachmentStatus }
            : attachment
        })
      )
    } catch (err) {
      if (!mountedRef.current || currentSessionIdRef.current !== stagingSessionId) return
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
    if (!showComposerRef.current) return
    const stagingSessionId = session.id
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
      const staged = await stageChatAttachments(stagingSessionId, pending.map((attachment) => ({
        id: attachment.id,
        name: attachment.name,
        kind: 'directory',
        path: attachment.path,
      })))
      if (!mountedRef.current || currentSessionIdRef.current !== stagingSessionId) {
        if (!useAppStore.getState().sessions.some((candidate) => candidate.id === stagingSessionId)) return
        const stored = readChatComposerDraft(stagingSessionId)
        writeChatComposerDraft(stagingSessionId, {
          ...stored,
          attachments: [...stored.attachments, ...staged.filter((candidate) =>
            !removedAttachmentIdsRef.current.has(candidate.id) &&
            !stored.attachments.some((attachment) => attachment.id === candidate.id)
          )],
        })
        return
      }
      setComposerAttachments((current) =>
        current.map((attachment) => {
          const replacement = staged.find((candidate) => candidate.id === attachment.id)
          return replacement
            ? { ...replacement, status: 'ready' as LocalAttachmentStatus }
            : attachment
        })
      )
    } catch (err) {
      if (!mountedRef.current || currentSessionIdRef.current !== stagingSessionId) return
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

  const createGoalAndSend = async (request: {
    objective: string; tokenBudget: number; executionOwner: 'uam' | 'provider'; prompt: string;
    attachments: Attachment[]; sendFailure: string; steerNow?: boolean; disarm?: boolean;
  }) => {
    const submittedSessionId = session.id
    const submittedDraft = draft
    const generation = ++goalMutationGenerationRef.current
    const ownsFeedback = () => mountedRef.current && currentSessionIdRef.current === submittedSessionId && goalMutationGenerationRef.current === generation
    submitInFlightRef.current = true
    goalMutationInFlightRef.current = true
    setSubmitting(true)
    setGoalSubmitting(true)
    try {
      const result = await setGoalStore(submittedSessionId, request.objective, request.tokenBudget, request.executionOwner)
      // Creation already admitted this prompt; changing panes must not cancel its native operation.
      const sent = result.ok ? await (request.steerNow
        ? sendSubmittedPrompt(submittedSessionId, request.prompt, request.attachments, true)
        : sendSubmittedPrompt(submittedSessionId, request.prompt, request.attachments)) : false
      const acknowledgedSession = useAppStore.getState().sessions.find((candidate) => candidate.id === submittedSessionId)
      if (sent && acknowledgedSession?.createdAt.getTime() === session.createdAt.getTime()) {
        acknowledgeChatComposerDraft(submittedSessionId, session.createdAt.getTime(), { text: submittedDraft, attachments: request.attachments })
      }
      if (!ownsFeedback()) return
      if (!sent) { setGoalError(result.ok ? request.sendFailure : result.error || 'Failed to create goal.'); return }
      setGoalError('')
      if (request.disarm) setGoalArmNextMessage(false)
      setDraft((current) => current === submittedDraft ? '' : current)
      setComposerAttachments((current) => current.filter((attachment) => !request.attachments.some((sentAttachment) => attachmentsEquivalent([attachment], [sentAttachment]))))
      setAttachmentError('')
    } catch {
      if (ownsFeedback()) setGoalError('Failed to create goal.')
    } finally {
      if (ownsFeedback()) {
        submitInFlightRef.current = false
        goalMutationInFlightRef.current = false
        setSubmitting(false)
        setGoalSubmitting(false)
      }
    }
  }

  const submitGoal = async (prompt: string) => {
    const goalMatch = prompt.match(/^\/goal\s+(.+?)(?:\s+--budget\s+(\d+))?\s*$/s)
    if (!goalMatch) return false
    const objective = goalMatch[1].trim()
    if (!objective) { setGoalError('Goal objective is required.'); return true }
    const tokenBudget = goalMatch[2] ? parseInt(goalMatch[2], 10) : defaultGoalTokenBudget
    const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
    const providerManaged = featurePreference === 'provider' && Boolean(nativeGoalCommand)
    await createGoalAndSend({ objective, tokenBudget, executionOwner: providerManaged ? 'provider' : 'uam',
      prompt: providerManaged ? `${nativeGoalCommand} ${objective}` : objective,
      attachments: composerAttachments.filter((attachment) => attachment.status === 'ready').map(({ status, error, ...attachment }) => attachment),
      sendFailure: 'Goal was created, but the first prompt failed to send.' })
    return true
  }

  const runCompact = async (commandDraft = draft, closePalette = false) => {
    if (!mountedRef.current || currentSessionIdRef.current !== session.id || submitInFlightRef.current || goalSubmitting) return
    if (!isCefContext()) {
      setSlashMessage('Manual compaction is unavailable without a connected native runtime.')
      return
    }
    const state = useAppStore.getState()
    const currentChat = state.sessions.find(chat => chat.id === session.id)
    const currentAcp = state.acpBindingBySessionId[session.id]
    const renderedTarget = manualCompactionForChat(session, acp)
    const target = manualCompactionForChat(currentChat, currentAcp)
    if (!mountedRef.current || currentSessionIdRef.current !== session.id || !renderedTarget || !target
      || !sameManualCompactionTarget(renderedTarget, target) || state.cliBindingBySessionId[session.id]?.running
      || !state.providers.some(provider => provider.id === target.providerId && provider.supportsStructured)) {
      setSlashMessage('Manual compaction is unavailable for this chat.')
      return
    }
    // Revalidate before clearing a palette token; an old option cannot erase a newer draft.
    if (closePalette) {
      if (activeSlashToken) setDraft(current => current === commandDraft ? replaceSlashAction(current, activeSlashToken, '') : current)
      setSlashIndex(0)
      setSlashGroup('')
      setPermissionMenuOpen(false)
    }
    const submittedSessionId = session.id
    const requestId = createRequestId('compactAcpSession')
    const ownsRequest = () => mountedRef.current && currentSessionIdRef.current === submittedSessionId && compactRequestIdRef.current === requestId
    const ownsFeedback = () => {
      if (!ownsRequest()) return false
      const current = useAppStore.getState()
      const chat = current.sessions.find(candidate => candidate.id === submittedSessionId)
      const binding = current.acpBindingBySessionId[submittedSessionId]
      return chat?.providerId === target.providerId && (chat?.modelId ?? '') === target.modelId
        && binding?.providerId === target.providerId && binding?.sessionId === target.nativeSessionId
    }
    compactRequestIdRef.current = requestId
    submitInFlightRef.current = true
    setSubmitting(true)
    setCompacting(true)
    setSlashMessage('')
    try {
      const response = await sendToCEF<{ accepted?: boolean }>({
        action: 'compactAcpSession',
        payload: { chatId: submittedSessionId },
        requestId,
      })
      if (!ownsFeedback()) return
      if (!response.ok || response.data?.accepted !== true) {
        // "failed" keeps this on the error tone; a plain message would show green.
        setSlashMessage(`Compaction failed: ${response.error || 'the request was not accepted.'}`)
        return
      }
      // Acceptance is not completion; the "Compacting context" strip and the transcript show progress.
      setDraft((current) => current === commandDraft && current.trim() === '/compact' ? '' : current)
    } catch {
      if (ownsFeedback()) setSlashMessage('Failed to request compaction.')
    } finally {
      if (ownsRequest()) {
        compactRequestIdRef.current = null
        submitInFlightRef.current = false
        setSubmitting(false)
        setCompacting(false)
      }
    }
  }

  const submit = async (event?: FormEvent, promptOverride?: string, steerNow = false) => {
    event?.preventDefault()
    if (editingMessageIndex !== null) {
      if (branchingMessageIndex === null && canChangeProvider && (editingMessageText.trim() || editingMessageAttachments.length > 0)) {
        if (editingMessageSource.kind === 'unsent') await saveEditedUnsentMessage()
        else {
          const nativeIds = editingMessageAttachments.filter((attachment) => attachment.type !== 'markdown-store').map((attachment) => attachment.id)
          const skillFiles = editingMessageAttachments.filter((attachment) => attachment.type === 'markdown-store').map((attachment) => attachment.id)
          await createMessageBranch(editingMessageIndex, editingMessageText, nativeIds,
            editingMessageSource.hadMarkdownStoreAttachments || skillFiles.length > 0 ? skillFiles : undefined)
        }
      }
      return
    }
    // Explicit row editing above remains available; a hidden ordinary draft cannot submit.
    if (!showComposerRef.current) return
    const prompt = (promptOverride ?? draft).trim()
    if (prompt === '/compact') { await runCompact(promptOverride ?? draft); return }
    if (!providerSupported || session.importedReadOnly || (!prompt && composerAttachments.length === 0) || submitInFlightRef.current || goalSubmitting || composerAttachments.some((attachment) => attachment.status !== 'ready')) return
	if (dictationActiveRef.current && promptOverride === undefined) {
	  await stopDictation(true)
	  return
	}
	const readyAttachments = composerAttachments
	  .filter((attachment) => attachment.status === 'ready')
	  .map(({ status, error, ...attachment }) => attachment)

    if (prompt === '/side' && !session.temporaryParentChatId && !isCompanionContext()) { setDraft(''); await navigateSide(); return }
    if ((prompt === '/side-return' || prompt === '/side-dismiss') && session.temporaryParentChatId && !isCompanionContext()) { setDraft(''); await navigateSide(prompt === '/side-return' ? 'return' : 'dismiss'); return }
    // Handle /goal command
    if (prompt.startsWith('/goal ')) { await submitGoal(prompt); return }
    const nativeGoalCommand = currentProvider.nativeGoalCommand?.trim() ?? ''
    if (nativeGoalCommand && (prompt === nativeGoalCommand || prompt.startsWith(`${nativeGoalCommand} `))) {
      const objective = prompt.slice(nativeGoalCommand.length).trim()
      if (!objective) { setGoalError('Goal objective is required.'); return }
      await createGoalAndSend({ objective, tokenBudget: defaultGoalTokenBudget, executionOwner: 'provider',
        prompt, attachments: readyAttachments, sendFailure: 'Goal was created, but the provider command failed to send.' })
      return
    }
    if (goalArmNextMessage) {
      const providerGoalCommand = featurePreference === 'provider' ? currentProvider.nativeGoalCommand?.trim() ?? '' : ''
      await createGoalAndSend({ objective: prompt, tokenBudget: defaultGoalTokenBudget, executionOwner: providerGoalCommand ? 'provider' : 'uam',
        prompt: providerGoalCommand ? `${providerGoalCommand} ${prompt}` : prompt, attachments: readyAttachments,
        sendFailure: 'Goal was created, but the first prompt failed to send.', steerNow, disarm: true })
      return
    }

    if (!steerNow && isClaudeProvider(currentProvider, currentProviderId) && currentModeId === 'plan') {
      setClaudePlanPrompt(prompt)
      return
    }
    const submittedSessionId = session.id
    submitInFlightRef.current = true
    setSubmitting(true)
    if (steerNow) setSteering(true)
    let ok = false
    try {
      ok = await (steerNow
        ? sendComposerPrompt(session.id, prompt, readyAttachments, true)
        : sendComposerPrompt(session.id, prompt, readyAttachments))
    } catch {
      ok = false
    }
    const acknowledgedSession = useAppStore.getState().sessions.find((candidate) => candidate.id === submittedSessionId)
    if (ok && acknowledgedSession?.createdAt.getTime() === session.createdAt.getTime()) {
      acknowledgeChatComposerDraft(submittedSessionId, session.createdAt.getTime(), { text: draft, attachments: readyAttachments })
    }
    if (currentSessionIdRef.current === submittedSessionId) {
      submitInFlightRef.current = false
      setSubmitting(false)
      if (steerNow) setSteering(false)
    }
    if (ok && currentSessionIdRef.current === submittedSessionId) {
      setDraft((current) => current === draft ? '' : current)
      setComposerAttachments((current) => current.filter((attachment) => !readyAttachments.some((sentAttachment) =>
        attachmentsEquivalent([attachment], [sentAttachment]))))
      setAttachmentError('')
    }
  }


  const pendingPermission = acp?.pendingPermission
  const pendingUserInput = acp?.pendingUserInput
  const latestAssistantMessage = latestAssistantMessageIndex >= 0 ? messages[latestAssistantMessageIndex] : undefined
  const repositoryComparisonRef = !isCompanionContext() && session.workspaceIsolationKind === 'gitWorktree' && session.workspaceBaseRef?.trim()
    ? session.workspaceBaseRef.trim()
    : 'chat'
  const reviewGoal = activeGoalId ? goals.find(goal => goal.id === activeGoalId) : null
  const chatWorking = chatHasUnfinishedWork(acp, cli, reviewGoal)
  useEffect(() => {
    if (chatWorking || historyIncomplete) setSelectedRepositoryFile(null)
  }, [chatWorking, historyIncomplete])
  const completedTurnKey = !chatWorking && latestAssistantMessageIndex > latestUserMessageIndex
    ? `${latestAssistantMessage?.id ?? ''}:${turnSerial}:${cli?.terminalId ?? ''}`
    : ''
  useEffect(() => {
    let cancelled = false
    if (!completedTurnKey || !workspaceDirectory) return

    void getVcsCommitStatus(session.id, 'git', {
      includeLineStats: true,
      requestId: `chat-summary:${session.id}:${completedTurnKey}`,
      comparisonRef: repositoryComparisonRef,
    }).then((status) => {
      if (cancelled) return
      setRepositoryReview(session.id, status?.available && !status.error && status.changedFiles.length > 0 ? status : null)
    })
    return () => { cancelled = true }
  }, [completedTurnKey, getVcsCommitStatus, repositoryComparisonRef, session.id, workspaceDirectory])
  useEffect(() => {
    let cancelled = false
    if (isCompanionContext() || remoteComputerUseDisabled || !workspaceDirectory) return
    // One light read per chat/workspace open (no polling): feeds the composer's repository branch.
    void getVcsCommitStatus(session.id, 'git', { includeLineStats: false, contextOnly: true, requestId: `composer-branch:${composerBranchScope}` }).then((status) => {
      if (cancelled) return
      setComposerVcsBranch({ scope: composerBranchScope, branch: status?.available && status.activeVcsType !== 'svn' ? status.branchOrRevision : '' })
    })
    return () => { cancelled = true }
  }, [composerBranchScope, getVcsCommitStatus, remoteComputerUseDisabled, session.id, workspaceDirectory])
  const handleBranchSwitched = (branch: string) => {
    setComposerVcsBranch({ scope: composerBranchScope, branch })
    void getVcsCommitStatus(session.id, 'git', { includeLineStats: false, contextOnly: true, requestId: `composer-branch:${composerBranchScope}` }).then((status) => {
      if (status?.available && status.activeVcsType !== 'svn') setComposerVcsBranch({ scope: composerBranchScope, branch: status.branchOrRevision })
    })
  }
  const sideBusyRef = useRef(false)
  const [sideBusy, setSideBusy] = useState(false)
  // A main chat keeps at most one live side chat; reopening it is a local switch,
  // so it never re-copies the parent context through the backend.
  const liveSideChatId = useAppStore((s) => session.temporaryParentChatId ? undefined : s.sessions.find((candidate) => candidate.temporaryParentChatId === session.id && !candidate.sideCleanupRequested)?.id)
  const showChatInPane = (id: string) => {
    if (useAppStore.getState().activeSessionId !== session.id) return
    assignChatToPane(id, readChatGridLayout().activeLeafId)
    useAppStore.getState().setActiveSession(id)
  }
  const navigateSide = async (action: 'open' | 'return' | 'dismiss' = 'open') => {
    if (action === 'open' && liveSideChatId) { showChatInPane(liveSideChatId); return }
    if (action === 'return') { if (session.temporaryParentChatId) showChatInPane(session.temporaryParentChatId); return }
    if (sideBusyRef.current) return
    sideBusyRef.current = true
    setSideBusy(true)
    try {
      const dismiss = action === 'dismiss'
      const response = await sendToCEF<{ chatId?: string; parentChatId?: string }>({ action: dismiss ? 'dismissSideChat' : 'createSideChat', payload: { chatId: session.id }, requestId: createRequestId('side') })
      if (!response.ok) { setSlashMessage(response.error || 'Side chat could not be changed.'); return }
      const id = dismiss ? response.data?.parentChatId : response.data?.chatId
      if (id) showChatInPane(id)
    } finally { sideBusyRef.current = false; setSideBusy(false) }
  }
  useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  const subagentParent = useAppStore(state => subagentParentFor(session, state.sessions))
  const showSideChatControls = !isCompanionContext() && Boolean(session.temporaryParentChatId || liveSideChatId)
  const isGitWorktree = session.workspaceIsolationKind === 'gitWorktree'
  const workspaceActionsDisabled = remoteComputerUseDisabled || workspaceActionBusy || Boolean(
    acp?.processing ||
    acp?.pendingPermission ||
    acp?.pendingUserInput ||
    (acp?.queuedPrompts?.length ?? 0) > 0 ||
    (acp?.pendingDirectPrompts?.length ?? 0) > 0 ||
    (acp?.unsentDirectPrompts?.length ?? 0) > 0 ||
    cli?.running
  )
  const remoteMoveDisabled = workspaceActionBusy || !recoveryHost || recoveryHost.runnerStatus !== 'ready' ||
    session.importedReadOnly || Boolean(acp?.running || cli?.running || acp?.processing || acp?.pendingPermission ||
      acp?.pendingUserInput || (acp?.queuedPrompts?.length ?? 0) > 0 ||
      (acp?.pendingDirectPrompts?.length ?? 0) > 0 || (acp?.unsentDirectPrompts?.length ?? 0) > 0)
  const moveRemoteWorkspace = async () => {
    const destination = useAppStore.getState().folders.find((folder) => folder.id === remoteMoveFolderId)
    if (remoteMoveDisabled || !destination || destination.id === session.folderId || destination.executionHostId !== session.executionHostId) return
    const movingChatId = session.id
    setWorkspaceActionBusy(true)
    setWorkspaceFeedback(null)
    try {
      const response = await sendToCEF({ action: 'moveChatWorkspace', payload: { chatId: movingChatId, folderId: destination.id } })
      if (!mountedRef.current || currentSessionIdRef.current !== movingChatId) return
      setWorkspaceFeedback(response.ok
        ? { tone: 'success', message: 'Moved chat. The next provider session uses the selected workspace.' }
        : { tone: 'error', message: response.error || 'The workspace move was not confirmed. Check this chat before retrying.' })
      if (response.ok) { setRemoteMoveFolderId(''); setWorkspaceMenuOpen(false) }
    } catch (error) {
      if (mountedRef.current && currentSessionIdRef.current === movingChatId) setWorkspaceFeedback({ tone: 'error', message: error instanceof Error ? error.message : 'The workspace move was not confirmed. Check this chat before retrying.' })
    } finally {
      if (mountedRef.current) setWorkspaceActionBusy(false)
    }
  }
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
    if (!selectedToolCall?.isSubAgent || !selectedToolCall.subAgentId || isManagedSubagentTool(selectedToolCall)) return
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
    const preview = await previewChatTurnRollback(session.id, latestAssistantMessageIndex + historyStartIndex)
    if (!preview) {
      setWorkspaceActionBusy(false)
      setWorkspaceFeedback({ message: 'This turn can no longer be rolled back safely.', tone: 'error' })
      return
    }
    setWorkspaceActionBusy(false)
    setRollbackConfirmation({
      messageIndex: latestAssistantMessageIndex + historyStartIndex,
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
  const manualCompactionAvailable = Boolean(providerSupported && currentProvider.supportsStructured && !cli?.running
    && isCefContext() && manualCompactionForChat(session, providerAcp))
  const providerRuntime = cli?.running ? cli : providerAcp
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
  const currentErrorSource = `Universal Agent Manager · ${providerShortName(errorProvider, errorProviderId)} ${providerRuntimeLabel(errorProvider, acp)}`
  const currentErrorTitle = `${currentErrorSource} error`
  const currentAcpError = acp?.promptActionError?.message || acp?.lastError || ''
  const currentAcpErrorKey = acp?.promptActionError ? `${session.id}:prompt:${acp.promptActionError.id}` : currentAcpError ? `${session.id}:${currentAcpError}` : ''
  const stoppedStatusLabel = acp && !acp.running && !currentAcpError && acp.lastStopReason &&
    !messages.some((message) => message.stopReason === acp.lastStopReason)
    ? stoppedResponseLabel(acp.lastStopReason, acp.lastStopReason !== 'timeout' && acp.lastStopReason !== 'provider-update')
    : ''
  const liveDividerOpen = Boolean(acp?.compacting) || isWaitingOnSubagents(acp) || Boolean(stoppedStatusLabel)
  useEffect(() => {
    // Live dividers sit at the end of the transcript; bring them into view as a new message would.
    if (liveDividerOpen && isNearBottomRef.current) bottomRef.current?.scrollIntoView?.({ block: 'end' })
  }, [liveDividerOpen])
  const slashNoticeTone: ComposerNoticeTone = /failed|unsupported/i.test(slashMessage)
    ? 'error'
    : /working|unavailable|provider-managed/i.test(slashMessage)
      ? 'warning'
      : 'success'
  const unsupportedProviderMessage = providerSupported
    ? ''
    : `${currentProviderName} is not supported in this build. Switch this chat to Gemini CLI to continue.`
  const canDeleteUnsentReceipt = !acp?.processing && !(acp?.pendingDirectPrompts?.length) && !acp?.pendingPermission && !acp?.pendingUserInput && !cli?.processing && cli?.turnState !== 'busy' && ['ready', 'stopped', 'error'].includes(acp?.lifecycleState ?? 'stopped')
  const canManageUnsent = !acp?.processing && !(acp?.queuedPrompts?.length) && !(acp?.pendingDirectPrompts?.length) && !acp?.pendingPermission && !acp?.pendingUserInput && !cli?.processing && cli?.turnState !== 'busy'
  const canChangeProvider = canManageUnsent && !(acp?.unsentDirectPrompts?.length)
  const unsentActionsDisabled = session.importedReadOnly || !canManageUnsent || branchingMessageIndex !== null || editingMessageIndex !== null
  const unsentReceiptDeleteDisabled = session.importedReadOnly || !canDeleteUnsentReceipt || branchingMessageIndex !== null || editingMessageIndex !== null
  const removeUnsent = async (index: number, receipt?: { id: string; revision: number }) => {
    if (session.importedReadOnly || (receipt ? !canDeleteUnsentReceipt : !canManageUnsent) || branchingMessageIndex !== null || editingMessageIndex !== null) return false
    const alias = receipt ? outgoingPresentation.byReceiptId.get(receipt.id) : outgoingPresentation.byLoadedIndex.get(index - historyStartIndex)
    const captured = alias && readOutgoingAttempts(outgoingScopeKey).find((attempt) => attempt.key === alias.key)
    const observed = useAppStore.getState()
    const observedMessages = observed.messages[session.id]
    const observedBinding = observed.acpBindingBySessionId[session.id]
    const removed = receipt ? await removeQueuedAcpPrompt(session.id, 0, receipt)
      : await discardUnsentMessage(session.id, index, messages[index - historyStartIndex]?.id ?? '')
    const state = useAppStore.getState()
    const current = captured && readOutgoingAttempts(outgoingScopeKey).find((attempt) => attempt.key === captured.key)
    const currentBinding = state.acpBindingBySessionId[session.id]
    const receiptChanged = receipt && [...(currentBinding?.pendingDirectPrompts ?? []), ...(currentBinding?.unsentDirectPrompts ?? []), ...(currentBinding?.queuedPrompts ?? [])]
      .some((prompt) => prompt.id === receipt.id && prompt.revision !== receipt.revision)
    const receiptLists = (binding: typeof observedBinding) => [...(binding?.pendingDirectPrompts ?? []), ...(binding?.unsentDirectPrompts ?? []), ...(binding?.queuedPrompts ?? [])]
    const withoutTargetReceipt = (binding: typeof observedBinding) => ({ ...binding,
      pendingDirectPrompts: (binding?.pendingDirectPrompts ?? []).filter((prompt) => !receipt || prompt.id !== receipt.id || prompt.revision !== receipt.revision),
      unsentDirectPrompts: (binding?.unsentDirectPrompts ?? []).filter((prompt) => !receipt || prompt.id !== receipt.id || prompt.revision !== receipt.revision),
      queuedPrompts: (binding?.queuedPrompts ?? []).filter((prompt) => !receipt || prompt.id !== receipt.id || prompt.revision !== receipt.revision),
    })
    const onlyExactReceiptRemoved = receipt && receiptLists(observedBinding).filter((prompt) => prompt.id === receipt.id && prompt.revision === receipt.revision).length === 1 &&
      !receiptLists(currentBinding).some((prompt) => prompt.id === receipt.id) &&
      JSON.stringify(withoutTargetReceipt(observedBinding)) === JSON.stringify(withoutTargetReceipt(currentBinding))
    const currentRows = state.messages[session.id]
    const observedIndex = index - historyStartIndex
    const observedRow = observedMessages?.[observedIndex]
    const currentRow = currentRows?.[observedIndex]
    const onlyExactNativeDiscarded = !receipt && captured?.binding?.kind === 'native' &&
      captured.binding.nativeIndex === index && captured.binding.messageId === observedRow?.id &&
      observedRow?.role === 'user' && observedRow.acpPromptNotSent === true && observedRow.acpPromptDiscarded !== true &&
      currentRow?.acpPromptDiscarded === true && currentRows?.length === observedMessages?.length &&
      observedMessages?.every((row, rowIndex) => rowIndex === observedIndex || currentRows?.[rowIndex] === row) &&
      [...new Set([...Object.keys(observedRow), ...Object.keys(currentRow)])].every((key) =>
        key === 'acpPromptDiscarded' || Object.is(observedRow[key as keyof Message], currentRow[key as keyof Message]))
    // The existing discard action changes exactly its captured row's discarded flag on guarded success.
    // All other row objects/fields must remain identical; neither trimming nor absence proves removal.
    // A receipt-only removal push may precede this exact guarded success. Absence alone never settles the
    // action, and every other field/carrier must remain unchanged; no push-causality or delivery inference.
    // A stale callback, remount, new birth, revised receipt or changed alias cannot settle a display record.
    if (removed === true && captured?.actionCorrelationId && current === captured && !receiptChanged && (currentRows === observedMessages || onlyExactNativeDiscarded) &&
        (currentBinding === observedBinding || onlyExactReceiptRemoved) && mountedRef.current &&
        currentSessionIdRef.current === session.id && state.sessions.find((chat) => chat.id === session.id)?.createdAt.getTime() === session.createdAt.getTime())
      updateOutgoingAttempts(outgoingScopeKey, new Map([[captured.key, { retired: true, binding: undefined }]]))
    return removed
  }
  const retryUnsentReceipt = async (receipt: { id: string; revision: number }) => {
    if (session.importedReadOnly || !canDeleteUnsentReceipt || branchingMessageIndex !== null || editingMessageIndex !== null) return false
    return retryUnsentAcpPrompt(session.id, receipt)
  }
  const retryUnsent = async (index: number) => {
    if (session.importedReadOnly || !canChangeProvider || branchingMessageIndex !== null || editingMessageIndex !== null) return false
    const generation = ++messageBranchGenerationRef.current
    const ownsFeedback = () => mountedRef.current && currentSessionIdRef.current === session.id && messageBranchGenerationRef.current === generation
    setBranchingMessageIndex(index)
    setMessageBranchError('')
    try {
      const result = await retryFailedMessage(session.id, index)
      if (!result.ok) throw new Error(result.error || 'Could not retry the failed message.')
      return true
    } finally {
      if (ownsFeedback()) setBranchingMessageIndex(null)
    }
  }
  const saveEditedUnsentMessage = async () => {
    if (editingMessageSource.kind !== 'unsent' || editingMessageIndex === null || unsentEditInFlightRef.current !== null) return
    const generation = ++messageBranchGenerationRef.current
    unsentEditInFlightRef.current = generation
    const ownsFeedback = () => mountedRef.current && currentSessionIdRef.current === session.id && messageBranchGenerationRef.current === generation
    setBranchingMessageIndex(editingMessageIndex)
    setMessageBranchError('')
    try {
      const result = await editUnsentMessage(session.id, editingMessageIndex, editingMessageSource.message, editingMessageText, editingMessageAttachments)
      if (!ownsFeedback()) return
      if (!result.ok) {
        setMessageBranchError(result.error || 'Could not save the unsent message. Your edit is kept.')
        return
      }
      setEditingMessageIndex(null)
      setEditingMessageText('')
      setEditingMessageAttachments([])
      setEditingMessageSource({ kind: 'branch' })
    } catch {
      if (ownsFeedback()) setMessageBranchError('Could not save the unsent message. Your edit is kept; check the chat before retrying.')
    } finally {
      if (unsentEditInFlightRef.current === generation) unsentEditInFlightRef.current = null
      if (ownsFeedback()) {
        setBranchingMessageIndex(null)
        window.requestAnimationFrame(() => {
          if (ownsFeedback()) composerTextareaRef.current?.focus()
        })
      }
    }
  }
  const createMessageBranch = useCallback(async (messageIndex: number, content?: string, retainedAttachmentIds?: string[], retainedMarkdownStoreFiles?: string[]) => {
    const generation = ++messageBranchGenerationRef.current
    const ownsFeedback = () => mountedRef.current && currentSessionIdRef.current === session.id && messageBranchGenerationRef.current === generation
    setBranchingMessageIndex(messageIndex)
    setMessageBranchError('')
    const chatMessages = useAppStore.getState().messages[session.id] ?? []
    if (content === undefined && canRetryMessageInPlace(chatMessages, messageIndex - historyStartIndex)) {
      try {
        const result = await retryFailedMessage(session.id, messageIndex)
        if (ownsFeedback() && !result.ok) setMessageBranchError(result.error || 'Could not retry the failed message.')
      } catch {
        if (ownsFeedback()) setMessageBranchError('Could not retry the failed message. Check the chat before trying again.')
      } finally {
        if (ownsFeedback()) setBranchingMessageIndex(null)
      }
      return
    }
    let branchId: string | null = null
    let branchError = 'Could not create the message branch.'
    try {
      branchId = retainedMarkdownStoreFiles !== undefined
        ? await branchFromMessage(session.id, messageIndex, content, retainedAttachmentIds, retainedMarkdownStoreFiles)
        : retainedAttachmentIds === undefined
          ? await branchFromMessage(session.id, messageIndex, content)
          : await branchFromMessage(session.id, messageIndex, content, retainedAttachmentIds)
    } catch (error) {
      if (error instanceof Error && error.message.trim()) branchError = error.message
    }
    if (!ownsFeedback()) return
    setBranchingMessageIndex(null)
    if (!branchId) {
      setMessageBranchError(branchError)
      return
    }
    setEditingMessageIndex(null)
    setEditingMessageText('')
  }, [branchFromMessage, historyStartIndex, retryFailedMessage, session.id])
  const beginEditingMessage = useCallback((messageIndex: number, content: string) => {
    messageBranchGenerationRef.current += 1
    setBranchingMessageIndex(null)
    setEditingMessageIndex(messageIndex)
    setEditingMessageText(content)
    const message = useAppStore.getState().messages[session.id]?.[messageIndex - historyStartIndex]
    setEditingMessageSource({ kind: 'branch', hadMarkdownStoreAttachments: Boolean(message?.attachments?.some((attachment) => attachment.type === 'markdown-store')) })
    setEditingMessageAttachments([...(message?.attachments ?? [])])
    setMessageBranchError('')
    composerTextareaRef.current?.focus()
  }, [session.id, historyStartIndex])
  const beginEditingUnsentMessage = useCallback((messageIndex: number, content: string) => {
    // Anchor the row/resources that produced this rendered action, never a newer same-ID store row.
    const message = messages[messageIndex - historyStartIndex]
    if (!message) return
    messageBranchGenerationRef.current += 1
    setBranchingMessageIndex(null)
    setEditingMessageIndex(messageIndex)
    setEditingMessageSource({ kind: 'unsent', message })
    setEditingMessageText(content)
    setEditingMessageAttachments([...(message.attachments ?? [])])
    setMessageBranchError('')
    composerTextareaRef.current?.focus()
  }, [historyStartIndex, messages, session.id])
  const removeEditingAttachment = useCallback((id: string, markdownStore: boolean) => {
    setEditingMessageAttachments((current) => current.filter((attachment) => attachment.id !== id || (attachment.type === 'markdown-store') !== markdownStore))
  }, [])
  const cancelEditingMessage = useCallback(() => {
    messageBranchGenerationRef.current += 1
    setEditingMessageIndex(null)
    setBranchingMessageIndex(null)
  }, [])
  const selectMessageBranch = useCallback((parentId: string, messageIndex: number, branchId: string) => {
    setPreferredBranch(parentId, messageIndex, branchId)
    setActiveSession(branchId)
  }, [setActiveSession])
  const selectPersistedTool = useCallback((messageId: string, toolId: string) => {
    setSelectedToolCallRef({ id: toolId, messageId })
  }, [])
  const dictationActive = dictationState !== 'idle'
  const canSend = useMemo(
    () => editingMessageIndex === null && providerSupported && !session.importedReadOnly && (draft.trim().length > 0 || composerAttachments.length > 0) && !submitting && !goalSubmitting && !composerAttachments.some((attachment) => attachment.status !== 'ready'),
    [editingMessageIndex, providerSupported, session.importedReadOnly, draft, submitting, goalSubmitting, composerAttachments]
  )
  const dictationAvailable = isCompanionContext() ? browserDictationAvailable() : isCefContext()
  const selectedChatId = useAppStore(s => s.activeSessionId)
  const voiceContextKey = JSON.stringify([session.id, session.createdAt?.getTime(), session.providerId, session.viewMode, session.executionHostId])
  const voice = useVoiceMode({
    contextKey: voiceContextKey,
    enabled: showComposer && providerSupported && !session.importedReadOnly && selectedChatId === session.id,
    blocked: dictationActive,
    messages,
    processing: Boolean(acp?.processing || cli?.processing || cli?.turnState === 'busy'),
    append: text => setDraft(previous => joinedDictationText(previous, text, '')),
    send: () => { if (canSend) void submit(undefined, draft) },
    stopResponse: () => { void cancelAcpTurn(session.id) },
    navigate: direction => {
      const state = useAppStore.getState()
      const eligible = state.sessions.filter(chat => !chat.importedReadOnly)
      const groups = buildChatSearchSessionGroups(eligible, {}, [])
      const ids = [...new Set([...groups.pinnedSessionIds, ...Array.from(groups.sessionIdsByFolderId.values()).flat(), ...groups.unfolderedSessionIds])]
      const index = ids.indexOf(session.branchRootChatId || session.parentChatId || session.id)
      const next = index >= 0 ? state.sessions.find(chat => chat.id === ids[index + direction]) : undefined
      if (next) setActiveSession(next.id)
    },
  })

  const previousVoiceRuntime = useRef({ key: voiceContextKey, acp, cli })
  useEffect(() => {
    const previous = previousVoiceRuntime.current
    previousVoiceRuntime.current = { key: voiceContextKey, acp, cli }
    if (previous.key !== voiceContextKey) return
    // Initial startup and native ID assignment keep the same voice conversation.
    // A runtime shutdown or a new failure ends its owned input and playback.
    const acpStopped = acp?.lifecycleState === 'stopped' && previous.acp && previous.acp.lifecycleState !== 'stopped'
    const cliStopped = cli?.lifecycleState === 'stopped' && previous.cli && previous.cli.lifecycleState !== 'stopped' && previous.cli.lifecycleState !== 'disabled'
    if (acpStopped || cliStopped ||
      (previous.acp?.running && !acp?.running && acp?.lifecycleState !== 'starting') ||
      (previous.cli?.running && !cli?.running) ||
      (acp?.lifecycleState === 'error' && previous.acp?.lifecycleState !== 'error') ||
      (cli?.lifecycleState === 'shuttingDown' && previous.cli?.lifecycleState !== 'shuttingDown') ||
      (acp?.lastError && acp.lastError !== previous.acp?.lastError) ||
      (cli?.lastError && cli.lastError !== previous.cli?.lastError)) voice.stop()
  }, [voiceContextKey, acp, cli])

  submitDictatedPromptRef.current = (prompt) => {
    void submit(undefined, prompt)
  }

  const startDictation = async () => {
    if (!showComposerRef.current) return
    if (!dictationAvailable || dictationActiveRef.current || voice.active || voice.speaking) return
    const dictationId = createRequestId('dictation')
    dictationIdRef.current = dictationId
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
      payload: { locale: navigator.language || '', dictationId },
    })
    if (!dictationActiveRef.current || dictationIdRef.current !== dictationId) return
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
    const dictationId = dictationIdRef.current
    const response = await sendToCEF<{ stopped: boolean }>({ action: 'stopDictation', payload: { dictationId: dictationIdRef.current } })
    if (!response.ok && dictationActiveRef.current && dictationIdRef.current === dictationId) {
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
    if (wasActive) void sendToCEF({ action: 'stopDictation', payload: { dictationId: dictationIdRef.current } })
    window.setTimeout(() => { if (showComposerRef.current) composerTextareaRef.current?.focus() }, 0)
  }

  useEffect(() => {
    if (!showComposer) {
      dictationSubmitAfterStopRef.current = false
      setDictationState('idle')
      return
    }
    const onDictation = (event: Event) => {
      if (!showComposerRef.current) return
      const message = (event as CustomEvent<DictationPushMessage>).detail
      if (!dictationActiveRef.current || !message || message.type !== 'dictation' || message.dictationId !== dictationIdRef.current) return

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
        void sendToCEF({ action: 'stopDictation', payload: { dictationId: dictationIdRef.current } })
      }
    }
  }, [session.id, showComposer])
  // The persisted chat setting is the choice for the next turn. ACP can still
  // report the previous idle model until that turn begins.
  const currentModelId = session.modelId || providerAcp?.currentModelId || ''
  const currentReviewerModelId = session.reviewerModelId || defaultReviewerModelId || currentModelId
  const showUnresolvedDefaultModel = !session.modelId && !providerAcp?.currentModelId
  const currentModel = modelOptionFor(buildModelOptions(providerAcp, currentModelId, currentProvider, currentProviderId, showUnresolvedDefaultModel), currentModelId)
  const currentProviderCapabilities = useMemo(
    () => providerCapabilities(currentProviderId, currentProvider),
    [currentProvider, currentProviderId]
  )
  const runtimeSupportsReasoning = (selectedRuntimeModel(providerAcp, currentModel.id)?.supportedReasoningEfforts?.length ?? 0) > 0
  const reasoningOptions = useMemo(() => currentProviderCapabilities.hasReasoningEffort || runtimeSupportsReasoning
    ? buildCodexReasoningOptions(
        providerAcp,
        currentModel.id,
        session.reasoningEffort ?? '',
        isCopilotProvider(currentProvider, currentProviderId)
          ? currentProviderCapabilities.reasoningOptions.map((option) => option.id)
          : currentProviderId === 'codex-cli' ? undefined : []
      )
    : [], [currentModel.id, currentProvider, currentProviderId, currentProviderCapabilities, providerAcp, runtimeSupportsReasoning, session.reasoningEffort])
  const serviceTierExplicit = session.serviceTierExplicit ?? (session.serviceTier ?? '') !== ''
  const speedOptions = useMemo(() => currentProviderCapabilities.hasServiceTier
    ? buildCodexSpeedOptions(providerAcp, currentModel.id, session.serviceTier ?? '', currentProviderId === 'codex-cli')
    : [], [currentModel.id, currentProviderId, currentProviderCapabilities.hasServiceTier, providerAcp, session.serviceTier])
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
  const supportsPlanActions = isCodexProvider(currentProvider, currentProviderId) && currentModeId === 'plan'
  const latestPlanMessageIndex = useMemo(() => supportsPlanActions ? messages.reduce((latest, message, index) => {
    const hasPlan = message.role === 'assistant' && (Boolean(message.planSummary?.trim()) || (message.planEntries?.length ?? 0) > 0)
    return hasPlan ? index : latest
  }, -1) : -1, [messages, supportsPlanActions])
  const latestPlanHasLaterUser = useMemo(
    () => latestPlanMessageIndex >= 0 && messages.slice(latestPlanMessageIndex + 1).some((message) => message.role === 'user'),
    [latestPlanMessageIndex, messages]
  )
  const canShowPlanActions = supportsPlanActions && latestPlanMessageIndex >= 0 && !latestPlanHasLaterUser
  const planActionBlockedByRuntime = runtimeBlocksControlChanges
  const planActionsDisabled = Boolean(submitting || planActionBlockedByRuntime)
  const planActionsDisabledTitle = planActionBlockedByRuntime
    ? 'Codex is still working.'
    : 'Plan action is unavailable.'
  const sendPlanAction = useCallback(async (prompt: string, nextModeId: 'default' | 'plan') => {
    if (planActionsDisabled) return
    setSubmitting(true)
    const modeOk = await setSessionApprovalMode(session.id, nextModeId)
    if (modeOk) {
      await sendSubmittedPrompt(session.id, prompt)
    }
    setSubmitting(false)
  }, [planActionsDisabled, sendSubmittedPrompt, session.id, setSessionApprovalMode])
  const activeGoal = activeGoalId ? goals.find((g) => g.id === activeGoalId) ?? null : null
  const displayedGoal = activeGoal ?? (goals.length > 0 ? goals[goals.length - 1] : null)
  const goalOwner = displayedGoal ? `${session.id}:${displayedGoal.id}` : ''
  const goalOwnerRef = useRef(goalOwner); goalOwnerRef.current = goalOwner
  const goalsPopupOpen = Boolean(goalOwner && goalsPopupOwner === goalOwner)
  useEffect(() => {
    setGoalsPopupOwner(null)
    if (goalMutationOwnerRef.current !== null && goalMutationOwnerRef.current !== goalOwner) {
      goalMutationGenerationRef.current += 1
      goalMutationOwnerRef.current = null
      goalMutationInFlightRef.current = false
      setGoalSubmitting(false)
      setGoalError('')
    }
  }, [goalOwner])
  useEffect(() => {
    if (goalsPopupOpen) goalsPopupRef.current?.querySelector<HTMLElement>('summary')?.focus()
  }, [goalsPopupOpen])
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
    if (goalSubmitting || goalMutationInFlightRef.current) return false
    const mutationGeneration = ++goalMutationGenerationRef.current
    const owner = goalOwner
    goalMutationOwnerRef.current = owner
    const ownsFeedback = () => mountedRef.current && currentSessionIdRef.current === session.id &&
      goalMutationGenerationRef.current === mutationGeneration && goalOwnerRef.current === owner
    goalMutationInFlightRef.current = true
    setGoalError('')
    setGoalSubmitting(true)
    try {
      const result = await mutation()
      if (!result.ok && ownsFeedback()) setGoalError(result.error || fallbackError)
      return result.ok
    } catch {
      if (ownsFeedback()) setGoalError(fallbackError)
      return false
    } finally {
      if (ownsFeedback()) {
        goalMutationInFlightRef.current = false
        goalMutationOwnerRef.current = null
        setGoalSubmitting(false)
      }
    }
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
    if (!displayedGoal) return
    await runGoalMutation(
      () => resumeGoal(session.id, displayedGoal.id),
      'Failed to resume goal.',
    )
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

  const setComputerUseActive = async (active: boolean): Promise<ComputerUseActionResult> => {
    const result = await setSessionComputerUseEnabled(session.id, active)
    if (!result.ok) setSlashMessage(result.error || 'Computer use could not be changed.')
    return result
  }

  // Slash command palette: typing "/" at the start of an empty-ish draft opens
  // a traversable menu of UAM actions (Codex-style), filtered by what follows.
  const currentMemoryLevel: MemoryLevel = session.memoryLevel ?? ((session.memoryEnabled ?? true) ? 'strict' : 'off')
  const runMemoryCommand = (level: MemoryLevel) => {
    setSlashIndex(0)
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
        ...(!session.temporaryParentChatId && !isCompanionContext() ? [{ id: 'side', label: '/side', hint: liveSideChatId ? 'Resume the open side chat' : 'Open a temporary side chat', icon: <MessageSquareDashed size={15} />, run: () => void navigateSide() }] : []),
        ...(session.temporaryParentChatId && !isCompanionContext() ? [
          { id: 'side-return', label: '/side-return', hint: 'Back to the main chat; this side chat stays open', icon: <CornerUpLeft size={15} />, run: () => void navigateSide('return') },
          { id: 'side-dismiss', label: '/side-dismiss', hint: 'Close and discard this side chat', icon: <X size={15} />, run: () => void navigateSide('dismiss') },
        ] : []),
        ...(manualCompactionAvailable ? [{ id: 'compact', label: '/compact', hint: 'Request native context compaction', icon: <FileText size={15} />, run: () => void runCompact() }] : []),
        { id: 'model', label: '/model', hint: 'Change the model', icon: <Cpu size={15} />, run: () => setModelOpen(true) },
        ...(reasoningOptions.length > 0 ? [{ id: 'reasoning', label: '/reasoning', hint: 'Choose Codex reasoning', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('reasoning') }] : []),
        ...(speedOptions.length > 0 ? [{ id: 'speed', label: '/speed', hint: 'Choose Codex speed', icon: <Cpu size={15} />, run: () => void runCodexOptionCommand('speed') }] : []),
        ...(providerVariants.length > 0 ? [{ id: 'variants', label: '/variants', hint: 'Choose OpenCode model variants', icon: <Cpu size={15} />, run: () => setDraft('/variants ') }] : []),
        { id: 'permission', label: '/permission', hint: 'Choose the permission mode', icon: <Shield size={15} />, run: () => void runPermissionCommand() },
        { id: 'goal', label: '/goal', hint: 'Use the next message as a goal', icon: <Target size={15} />, run: handleToggleGoal },
        ...(COMPUTER_USE_ENABLED ? [{ id: 'computer', label: '/computer', hint: `${computerUseMode ? 'Turn off' : 'Turn on'} computer use`, icon: <MousePointer2 size={15} />, run: () => void setComputerUseActive(!computerUseMode) }] : []),
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
        if (label.toLowerCase() === '/compact' || usedLabels.has(label.toLowerCase())) continue
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
    [session.id, session.providerId, session.modelId, manualCompactionAvailable, providerAcp?.sessionId, providerAcp?.manualCompaction, session.temporaryParentChatId, liveSideChatId, currentMemoryLevel, session.commandSafetyTier, session.reasoningEffort, session.serviceTier, session.serviceTierExplicit, session.computerUseEnabled, session.computerUseTargetId, markdownStoreEntries, providerAcp?.availableCommands, currentModeId, permissionModes, providerSupported, currentProviderName, reasoningOptions, speedOptions, providerVariants, activeGoal?.id, displayedGoal?.id, displayedGoal?.status]
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
  const slashOpen = showComposer && editingMessageIndex === null && !appModalOpen && slashMatches.length > 0 && (slashQuery !== null || permissionModeQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined || variantQuery !== undefined)
  const slashPaletteVisible = showComposer && (slashQuery !== null || permissionModeQuery !== undefined || memoryLevelQuery !== undefined || codexOptionKind !== undefined || variantQuery !== undefined)
  useEffect(() => {
    if (slashPaletteVisible && markdownStoreEntries.length === 0) {
      void refreshMarkdownStore()
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [slashPaletteVisible])
  const runSlashCommand = (command: SlashCommand) => {
    if (command.id === 'compact') {
      void runCompact(draft, true)
      return
    }
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
  const activeSlashOptionId = !slashOpen
    ? undefined
    : activeSlashGroup?.groupEntries
      ? `${slashListboxId}-group-${activeSlashGroups.length - 1}-option-${Math.min(slashGroupIndex, activeSlashGroup.groupEntries.length - 1)}`
      : `${slashListboxId}-option-${Math.min(slashIndex, slashMatches.length - 1)}`
  const controlledSlashMenuIds = slashOpen
    ? [slashListboxId, ...activeSlashGroups.map((_, index) => `${slashListboxId}-group-${index}`)].join(' ')
    : undefined

  const onComposerKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (!showComposerRef.current && editingMessageIndex === null) return
    if (editingMessageIndex !== null) {
      if (event.key === 'Escape') { event.preventDefault(); cancelEditingMessage() }
      else if (event.key === 'Enter' && !event.shiftKey && !event.nativeEvent.isComposing && event.nativeEvent.keyCode !== 229) { event.preventDefault(); void submit() }
      return
    }
	const nativeEvent = event.nativeEvent
	if (nativeEvent.isComposing || nativeEvent.keyCode === 229) return
	if (matchesUamAgentCycleShortcut(uamAgentCycleShortcut, event) &&
		!event.repeat &&
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

  const activePlanActions = useMemo(() => canShowPlanActions && providerSupported
    ? {
        show: true,
        disabled: planActionsDisabled,
        disabledTitle: planActionsDisabledTitle,
        onApprove: () => void sendPlanAction(PLAN_APPROVE_PROMPT, 'default'),
        onDeny: () => void sendPlanAction(PLAN_DENY_PROMPT, 'plan'),
      }
    : undefined, [canShowPlanActions, planActionsDisabled, planActionsDisabledTitle, providerSupported, sendPlanAction])
  const resolveClaudePlanPrompt = async (nextModeId: 'acceptEdits' | 'default' | 'plan') => {
    const prompt = claudePlanPrompt?.trim()
    if (!prompt) {
      setClaudePlanPrompt(null)
      return
    }

    const submittedSessionId = session.id
    const submittedDraft = draft
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
      ? await sendSubmittedPrompt(session.id, prompt, readyAttachments)
      : await sendSubmittedPrompt(session.id, prompt)
    const acknowledgedSession = useAppStore.getState().sessions.find((candidate) => candidate.id === submittedSessionId)
    if (ok && acknowledgedSession?.createdAt.getTime() === session.createdAt.getTime()) {
      acknowledgeChatComposerDraft(submittedSessionId, session.createdAt.getTime(), { text: submittedDraft, attachments: readyAttachments })
    }
    if (ok && currentSessionIdRef.current === submittedSessionId) {
      setDraft((current) => current === submittedDraft ? '' : current)
      setComposerAttachments((current) => current.filter((attachment) => !readyAttachments.some((sentAttachment) => attachmentsEquivalent([attachment], [sentAttachment]))))
      setAttachmentError('')
      setClaudePlanPrompt(null)
    }
    setSubmitting(false)
  }

  // Draft keystrokes re-render ChatView; the transcript only depends on conversation state, so keep its rows.
  // Unsent actions run at click time and read the latest render through this ref.
  const unsentActionsRef = useRef({ removeUnsent, retryUnsent, retryUnsentReceipt })
  unsentActionsRef.current = { removeUnsent, retryUnsent, retryUnsentReceipt }
  // formatWorkedDuration rounds to whole seconds, so rounding here keeps the label identical.
  const turnWorkedDisplaySeconds = turnWorkedSeconds === undefined ? undefined : Math.round(turnWorkedSeconds)
  const transcriptRows = useMemo(() => {
                // One keyed list: messages, unmaterialized receipts and display-only attempts share the same row shape,
                // so an attempt's alias key keeps the same outer row and article from click through ACK and native push.
                const outgoingSlot = (key: string, message: Message, delivery: ConversationDelivery, actions?: ReactNode, receiptId?: string) => (
                  <WorkSectionContext.Provider key={key} value={ALWAYS_OPEN_WORK_SECTION}>
                  <div className="space-y-1" data-outgoing-message="" data-unsent-receipt-id={receiptId}>
                    {null}{null}{null}
                    <PersistedConversationMessage message={message} hasDeliveredResponse={false} hasStoppedResponse={false} pendingDirectAdmission={false}
                      delivery={delivery} deliveryActions={actions} disclosureState={liveWorkDisclosure} index={-1}
                      assistantLabel="" sessionId={session.id} branchSessions={[]} actionsDisabled planActions={undefined}
                      workingMode={workingDisplayMode} retryInPlace={false} onBeginEdit={beginEditingMessage}
                      onCreateBranch={createMessageBranch} onSelectBranch={selectMessageBranch} onSelectTool={selectPersistedTool} />
                    {null}{null}{null}
                  </div>
                  </WorkSectionContext.Provider>
                )
                const outgoingMessage = (id: string, text: string, attachments: readonly Attachment[], createdAt = new Date(0)): Message =>
                  ({ id, sessionId: session.id, role: 'user', content: text, attachments: [...attachments], createdAt })
                const nativeRows = visibleMessages.map((message, visibleIndex) => {
                const index = earliestRenderedMessageIndex + visibleIndex
                if (index === redundantStreamingMessageIndex || unsentProjection.hiddenIndices.has(index)) return null
                const shouldRenderTimelineAtAssistant = renderTimelineAtAssistant && index === turnAssistantMessageIndex
                const shouldSkipAssistantMessage = renderTimelineAfterUser && turnAssistantMessageMatches && index === turnAssistantMessageIndex
                const { followingResponse, hasDeliveredResponse, hasStoppedResponse } = responseFlags(messages, index)
                const messageProviderId = message.providerId?.trim() || followingResponse?.providerId?.trim() || currentProviderId
                const messageProviderName = providerShortName(
                  providers.find((candidate) => candidate.id === messageProviderId),
                  messageProviderId
                )

                if (shouldSkipAssistantMessage) return null
                let disclosureState = savedWorkDisclosures.get(message.id)
                if (!disclosureState) {
                  disclosureState = {}
                  savedWorkDisclosures.set(message.id, disclosureState)
                }

                const outgoingAlias = message.role === 'user' ? outgoingPresentation.byLoadedIndex.get(index) : undefined
                // Admission/materialization/output are not write witnesses. Only this exact input's positive native
                // OS-sink write fact can settle its presentation; explicit queue/recovery authority remains above.
                const outgoingUnresolved = Boolean(outgoingAlias && !outgoingPresentation.writeCompletedKeys.has(outgoingAlias.key))
                const nativeDirectReceipt = outgoingPresentation.directReceiptByLoadedIndex.get(index)
                const section = workSections.get(index)!
                const sectionActive = Boolean(acp?.processing && section.firstIndex === activeWorkSectionFirstIndex)
                const containsSearchTarget = searchNavigation && workSections.get(searchNavigation.messageIndex - historyStartIndex)?.firstIndex === section.firstIndex
                const sectionControl = sectionActive || containsSearchTarget || !collapsibleWorkSections ? ALWAYS_OPEN_WORK_SECTION : sectionControls(section.id)
                const sectionCollapsed = !sectionActive && !sectionControl.expanded
                const hideCollapsedMessage = sectionCollapsed && (
                  (message.role === 'user' && message.continuesTurn) ||
                  (message.role === 'assistant' && index !== section.lastAssistantIndex)
                )
                if (hideCollapsedMessage) return message.role === 'assistant' && message.toolCalls?.some(isInlineSubagentLaunch) ? (
                  <div key={message.id} data-message-index={index + historyStartIndex}>
                    <PersistedMessageContent message={message} disclosureState={disclosureState} sourceChatId={session.id}
                      workingMode={workingDisplayMode} onSelectTool={selectPersistedTool} launchesOnly />
                  </div>
                ) : null
                const firstVisibleInSection = index === Math.max(section.firstIndex, earliestRenderedMessageIndex) ||
                  (sectionCollapsed && section.firstIndex < earliestRenderedMessageIndex && index === section.lastAssistantIndex)
                const sectionStart = messages[section.firstIndex]
                const sectionResponse = messages[section.firstIndex + 1]?.role === 'assistant' ? messages[section.firstIndex + 1] : undefined
                const sectionProviderId = sectionStart.providerId?.trim() || sectionResponse?.providerId?.trim() || currentProviderId
                const sectionProviderName = providerShortName(providers.find((candidate) => candidate.id === sectionProviderId), sectionProviderId)
                // Brief turns with no tool or reasoning activity drop the "Worked" toggle; live turns always show it.
                const briefTurn = !sectionActive && !section.hasTrace && section.workedSeconds < 2
                const heading = firstVisibleInSection && (section.hasAssistant || sectionActive) && !briefTurn ? <div data-testid={sectionActive && turnEvents.length === 0 ? "turn-starting" : undefined}><ConversationWork
                  sectionHeading headerOnly active={sectionActive} startedAt={sectionActive ? turnClockStart : undefined}
                  duration={formatWorkedDuration(section.workedSeconds)} events={[]} tools={[]} collapsible={collapsibleWorkSections}
                  onSelectTool={selectLiveTool}
                /></div> : null
                const turnStart = firstVisibleInSection && sectionStart.role === 'user' && Number.isFinite(sectionStart.createdAt.getTime()) ? sectionStart : undefined
                const turnModelId = sectionStart.modelId?.trim() || sectionResponse?.modelId?.trim() || (section.firstIndex === activeWorkSectionFirstIndex ? acp?.currentModelId || session.modelId : '') || ''
                const turnIdentity = [sectionProviderName, turnModelId].filter(Boolean).join(' · ')
                const previousAssistant = turnStart ? previousAssistantMessage(messages, section.firstIndex) : undefined
                const previousProviderId = previousAssistant ? previousAssistant.providerId?.trim() || currentProviderId : ''
                const previousIdentity = previousAssistant
                  ? [providerShortName(providers.find((candidate) => candidate.id === previousProviderId), previousProviderId), previousAssistant.modelId?.trim()].filter(Boolean).join(' · ')
                  : ''
                const showTurnIdentity = Boolean(turnStart && turnIdentity && (!previousAssistant || turnIdentity !== previousIdentity))
                const priorMessage = section.firstIndex > 0 ? messages[section.firstIndex - 1] : undefined
                const dayChanged = Boolean(turnStart) && (!priorMessage || priorMessage.createdAt.toDateString() !== sectionStart.createdAt.toDateString())
                return (
                  <WorkSectionContext.Provider key={outgoingAlias?.key ?? message.id} value={sectionControl}>
                  <div className="space-y-1" data-message-index={index + historyStartIndex} data-unsent-message-index={unsentByLoadedIndex.has(index) ? index + historyStartIndex : undefined} title={turnStart ? turnTimestampLabel(turnStart.createdAt) : undefined}>
                    {dayChanged && turnStart && <div className="conversation-day-divider"><span>{dayDividerLabel(turnStart.createdAt)}</span></div>}
                    {turnStart && <TurnStartTime
                      timestamp={turnStart.createdAt.getTime()}
                      identity={turnIdentity}
                      showIdentity={showTurnIdentity}
                    />}
                    {message.role !== 'user' && heading}
                    {shouldRenderTimelineAtAssistant ? (
                      <MessageFrame
                        role={message.role}
                        assistantLabel={reviewAssistantLabel(messageProviderName, message)}
                        goalReview={Boolean(goalReviewForMessage(message))}
                        streaming={Boolean(acp?.processing)}
                        actionsDisabled={session.importedReadOnly || !canChangeProvider || branchingMessageIndex !== null}
                      >
                        <TurnTimelineContent
                          disclosureState={liveWorkDisclosure}
                          key={`turn-${turnSerial}-assistant`}
                          startedAt={turnClockStart}
                          interrupted={Boolean(messages[turnAssistantMessageIndex]?.interrupted)}
                          stopReason={messages[turnAssistantMessageIndex]?.stopReason}
                          events={turnEvents}
                            tools={liveTools}
                            planSummary={acp?.planSummary ?? ''}
                            planEntries={livePlanEntries}
                            planActions={activePlanActions}
                            pendingPermission={pendingPermission ?? null}
                            pendingUserInput={pendingUserInput ?? null}
                            waitIsStale={acp?.waitIsStale}
                            waitStaleReason={acp?.waitStaleReason}
                            waitSeconds={acp?.waitSeconds}
                            onSelectTool={selectLiveTool}
                            onResolvePermission={resolveLivePermission}
                          onCancelTurn={cancelLiveTurn}
                          onStopRuntime={stopLiveRuntime}
                          sourceChatId={session.id}
                          active={Boolean(acp?.processing)}
                          workingMode={workingDisplayMode}
                          workedSeconds={turnWorkedDisplaySeconds}
                        />
                      </MessageFrame>
                    ) : (
                      <PersistedConversationMessage
                        message={message}
                        hasDeliveredResponse={hasDeliveredResponse}
                        hasStoppedResponse={hasStoppedResponse}
                        pendingDirectAdmission={unsentProjection.pendingIndices.has(index) || nativeDirectReceipt?.sending === true}
                        delivery={unsentByLoadedIndex.has(index) ? (unsentByLoadedIndex.get(index)!.retryable ? UNSENT_DELIVERY : HELD_DELIVERY)
                          : nativeDirectReceipt ? (nativeDirectReceipt.sending ? SENDING_DELIVERY : RECEIPT_DELIVERY)
                          : unsentProjection.pendingIndices.has(index) ? SENDING_DELIVERY : outgoingUnresolved ? attemptDelivery(outgoingAlias!) : undefined}
                        deliveryActions={unsentByLoadedIndex.has(index) && !isCompanionContext() ? <OutgoingRecoveryActions
                          onEdit={() => beginEditingUnsentMessage(index + historyStartIndex, message.content)}
                          onRetry={unsentByLoadedIndex.get(index)!.retryable ? () => unsentActionsRef.current.retryUnsent(index + historyStartIndex) : undefined}
                          onRemove={() => unsentActionsRef.current.removeUnsent(index + historyStartIndex)}
                          editDisabled={unsentActionsDisabled} retryDisabled={unsentActionsDisabled} removeDisabled={unsentActionsDisabled} />
                          : nativeDirectReceipt && !nativeDirectReceipt.sending && !isCompanionContext() ? <OutgoingRecoveryActions
                            retryUnavailable={!receiptRetryAvailable(nativeDirectReceipt.prompt)}
                            onRetry={receiptRetryAvailable(nativeDirectReceipt.prompt) ? () => unsentActionsRef.current.retryUnsentReceipt({ id: nativeDirectReceipt.prompt.id!, revision: nativeDirectReceipt.prompt.revision! }) : undefined}
                            retryDisabled={unsentReceiptDeleteDisabled}
                            onRemove={() => unsentActionsRef.current.removeUnsent(-1, { id: nativeDirectReceipt.prompt.id!, revision: nativeDirectReceipt.prompt.revision! })}
                            removeDisabled={unsentReceiptDeleteDisabled} /> : undefined}
                        disclosureState={disclosureState}
                        index={index + historyStartIndex}
                        assistantLabel={reviewAssistantLabel(messageProviderName, message)}
                        sessionId={session.id}
                        sessionParentChatId={session.parentChatId}
                        sessionBranchFromMessageIndex={session.branchFromMessageIndex}
                        sessionBranchMessageEdited={session.branchMessageEdited}
                        branchSessions={branchSessions}
                        actionsDisabled={session.importedReadOnly || !canChangeProvider || branchingMessageIndex !== null || unsentProjection.pendingIndices.has(index) || Boolean(nativeDirectReceipt)}
                        planActions={index === latestPlanMessageIndex ? activePlanActions : undefined}
                        workingMode={workingDisplayMode}
                        retryInPlace={index === retryInPlaceMessageIndex}
                        onBeginEdit={beginEditingMessage}
                        onCreateBranch={createMessageBranch}
                        onSelectBranch={selectMessageBranch}
                        onSelectTool={selectPersistedTool}
                      />
                    )}
                    {message.role === 'user' && message.managedOriginRunId && <InitialManagedSubagentGroup runId={message.managedOriginRunId} sourceChatId={session.id} />}
                    {message.role === 'user' && heading}
                    {renderTimelineAfterUser && index === turnUserMessageIndex && (
                      <div data-message-index={turnAssistantMessageMatches ? turnAssistantMessageIndex + historyStartIndex : undefined}>
                      <MessageFrame
                        key={`turn-${turnSerial}-after-user`}
                        role="assistant"
                        assistantLabel={reviewAssistantLabel(currentProviderName, messages[turnAssistantMessageIndex] ?? messages[turnUserMessageIndex])}
                        streaming={Boolean(acp?.processing)}
                      >
                        <TurnTimelineContent
                          disclosureState={liveWorkDisclosure}
                          key={`turn-${turnSerial}-after-user-content`}
                          startedAt={turnClockStart}
                          interrupted={Boolean(messages[turnAssistantMessageIndex]?.interrupted)}
                          stopReason={messages[turnAssistantMessageIndex]?.stopReason}
                          events={turnEvents}
                            tools={liveTools}
                            planSummary={acp?.planSummary ?? ''}
                            planEntries={livePlanEntries}
                            planActions={activePlanActions}
                            pendingPermission={pendingPermission ?? null}
                            pendingUserInput={pendingUserInput ?? null}
                            waitIsStale={acp?.waitIsStale}
                            waitStaleReason={acp?.waitStaleReason}
                            waitSeconds={acp?.waitSeconds}
                            onSelectTool={selectLiveTool}
                            onResolvePermission={resolveLivePermission}
                          onCancelTurn={cancelLiveTurn}
                          onStopRuntime={stopLiveRuntime}
                          sourceChatId={session.id}
                          active={Boolean(acp?.processing)}
                          workingMode={workingDisplayMode}
                          workedSeconds={turnWorkedDisplaySeconds}
                        />
                      </MessageFrame>
                      </div>
                    )}
                  </div>
                  </WorkSectionContext.Provider>
                )
              })
                const outgoingRows = [...outgoingReceipts.filter((prompt) => {
                  const index = prompt.receipt ? outgoingPresentation.receiptNativeIndexByKey.get(JSON.stringify([prompt.receipt.id, prompt.receipt.revision])) : undefined
                  // A bound but hidden/off-page native row cannot stand in for the direct receipt's bytes/actions.
                  return index === undefined || nativeRows[index - earliestRenderedMessageIndex] == null
                }).map((prompt) => {
                  const alias = prompt.receipt ? outgoingPresentation.byReceiptId.get(prompt.receipt.id) : undefined
                  const nativeReceipt = (acp?.unsentDirectPrompts ?? []).find(entry => entry.id === prompt.receipt?.id && entry.revision === prompt.receipt?.revision)
                  // Explicit Retry prepares the exact native receipt; nothing here infers delivery or replays on mount.
                  return { attempt: alias, node: outgoingSlot(alias?.key ?? prompt.receipt?.id ?? String(prompt.messageIndex),
                    outgoingMessage(prompt.receipt?.id ?? String(prompt.messageIndex), prompt.text, prompt.attachments, alias?.createdAt),
                    prompt.sending ? SENDING_DELIVERY : RECEIPT_DELIVERY,
                    !prompt.sending && prompt.receipt && !isCompanionContext() ? <OutgoingRecoveryActions
                      retryUnavailable={!nativeReceipt || !receiptRetryAvailable(nativeReceipt)}
                      onRetry={nativeReceipt && receiptRetryAvailable(nativeReceipt) ? () => unsentActionsRef.current.retryUnsentReceipt(prompt.receipt!) : undefined}
                      retryDisabled={unsentReceiptDeleteDisabled}
                      onRemove={() => unsentActionsRef.current.removeUnsent(prompt.messageIndex, prompt.receipt)} removeDisabled={unsentReceiptDeleteDisabled} /> : undefined,
                    prompt.receipt?.id) }
                }), ...outgoingPresentation.tail.map((attempt) => ({ attempt,
                  node: outgoingSlot(attempt.key, outgoingMessage(attempt.key, attempt.text, attempt.attachments, attempt.createdAt),
                    outgoingPresentation.retainedUnknownKeys.has(attempt.key) ? LOCAL_UNCONFIRMED_DELIVERY : attemptDelivery(attempt)),
                }))]
                // A discarded native row may be hidden before an action can safely retire its display alias.
                // Keep that unresolved alias rendered now, rather than unmounting it and reviving it after trim.
                // Exact receipts already rendered above keep their own precedence; this adds no recovery authority.
                const outgoingKeys = new Set(outgoingRows.flatMap(({ attempt }) => attempt ? [attempt.key] : []))
                for (const [index, attempt] of outgoingPresentation.byLoadedIndex) {
                  if (messages[index]?.acpPromptDiscarded !== true || nativeRows[index - earliestRenderedMessageIndex] != null || outgoingKeys.has(attempt.key)) continue
                  outgoingKeys.add(attempt.key)
                  outgoingRows.push({ attempt, node: outgoingSlot(attempt.key,
                    outgoingMessage(attempt.key, attempt.text, attempt.attachments, attempt.createdAt), LOCAL_UNCONFIRMED_DELIVERY) })
                }
                // Captured indices place display rows only; they never identify or confirm a native message.
                // Keep one flat keyed list so a receipt/native alias retains the same outer row and article.
                const attemptOrder = new Map(outgoingAttempts.map((attempt, ordinal) => [attempt.key, ordinal]))
                const anchoredRows = outgoingRows.flatMap(({ attempt, node }, sourceIndex) => attempt ? [{
                  boundary: attempt.nativeIndex, ordinal: attemptOrder.get(attempt.key)!, sourceIndex, node,
                }] : []).sort((left, right) => left.boundary - right.boundary || left.ordinal - right.ordinal)
                const rows: ReactNode[] = []
                const placed = new Set<number>()
                let nextAnchor = 0
                nativeRows.forEach((row, visibleIndex) => {
                  const boundary = historyStartIndex + earliestRenderedMessageIndex + visibleIndex
                  while (nextAnchor < anchoredRows.length && anchoredRows[nextAnchor].boundary <= boundary) {
                    const anchored = anchoredRows[nextAnchor++]
                    rows.push(anchored.node)
                    placed.add(anchored.sourceIndex)
                  }
                  // Hidden/null native slots still preserve their original raw-index boundary.
                  rows.push(row)
                })
                // Cold receipts retain their fallback slots. Unreached local rows keep capture order in the remaining slots.
                const trailingRows = outgoingRows.filter((_, sourceIndex) => !placed.has(sourceIndex))
                return [...rows, ...trailingRows.map(({ attempt, node }) => attempt ? anchoredRows[nextAnchor++].node : node)]
  }, [acp, activePlanActions, activeWorkSectionFirstIndex, beginEditingMessage, beginEditingUnsentMessage, branchSessions, branchingMessageIndex, canChangeProvider, cancelLiveTurn, collapsibleWorkSections, createMessageBranch, currentProviderId, currentProviderName, earliestRenderedMessageIndex, historyStartIndex, latestPlanMessageIndex, livePlanEntries, liveTools, liveWorkDisclosure, messages, outgoingAttempts, outgoingPresentation, outgoingReceipts, pendingPermission, pendingUserInput, providers, redundantStreamingMessageIndex, renderTimelineAfterUser, renderTimelineAtAssistant, resolveLivePermission, retryInPlaceMessageIndex, savedWorkDisclosures, searchNavigation, sectionControls, selectLiveTool, selectMessageBranch, selectPersistedTool, session, stopLiveRuntime, turnAssistantMessageIndex, turnAssistantMessageMatches, turnClockStart, turnEvents, turnSerial, turnUserMessageIndex, turnWorkedDisplaySeconds, unsentActionsDisabled, unsentByLoadedIndex, unsentProjection, unsentReceiptDeleteDisabled, visibleMessages, workSections, workingDisplayMode])

  return (
    <FileInspectionProvider chatId={session.id}>
    <div className="relative h-full flex overflow-hidden"
      onPointerDownCapture={() => {
        const revision = session.attentionRevision
        if (revision) void useAppStore.getState().acknowledgeChatAttention(session.id, revision)
      }}
      onKeyDownCapture={() => {
        const revision = session.attentionRevision
        if (revision) void useAppStore.getState().acknowledgeChatAttention(session.id, revision)
      }}
      style={{ background: 'var(--bg)' }}>
      {providerHandoffTarget && (
        <ProviderHandoffDialog
          sourceName={currentProviderName}
          targetName={providerHandoffTargetName}
          blockedReason={providerHandoffBlockedReason}
          onCancel={() => setProviderHandoffTargetId('')}
          onConfirm={async () => {
            const changed = await setSessionProvider(session.id, providerHandoffTarget.id)
            if (!changed) return false
            if (currentSessionIdRef.current !== session.id) return false
            if (providerHandoffModelId !== null && !await setSessionModel(session.id, providerHandoffModelId)) return false
            setProviderHandoffTargetId('')
            setProviderHandoffModelId(null)
            return true
          }}
        />
      )}
      {selectedToolCall && (
        <ToolCallModal
          tool={selectedToolCall}
          chatId={session.id}
          messageIndex={selectedToolCallRef?.messageId ? messages.findIndex((message) => message.id === selectedToolCallRef.messageId) + historyStartIndex : undefined}
          onClose={() => setSelectedToolCallRef(null)}
          onOpenSubAgent={selectedToolCall.isSubAgent && !isManagedSubagentTool(selectedToolCall) ? () => void openSelectedSubAgentSession() : undefined}
          accentColor={accentColor}
        />
      )}
      {!chatWorking && !historyIncomplete && selectedRepositoryFile && repositoryChanges && (
        <RepositoryDiffDialog
          chatId={session.id}
          file={selectedRepositoryFile}
          vcsType={repositoryChanges.activeVcsType}
          comparisonRef={repositoryComparisonRef}
          getDiff={getVcsFileDiff}
          onClose={() => setSelectedRepositoryFile(null)}
        />
      )}
      <div className="relative flex-1 flex flex-col min-w-0"
        style={showComposer ? undefined : { '--composer-dock-h': '0px' } as React.CSSProperties}>
        {showSubagentLineage && subagentParent && <nav aria-label="Subagent lineage" className="px-4 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)' }}>
          <button type="button" className="uam-btn uam-btn--ghost uam-btn--sm" onClick={() => showChatInPane(subagentParent.id)}>
            <CornerUpLeft size={14} aria-hidden /><span>Subagent of {subagentParent.name}</span>
          </button>
        </nav>}
        <div className="uam-chat-viewport relative flex-1 min-h-0" data-message-navigation={messageNavigationVisible || undefined}>
          <div ref={scrollRef} className="uam-chat-transcript relative z-0 h-full overflow-auto" data-copy-surface="chat" onScroll={handleScroll}>
            <div ref={transcriptContentRef} className="uam-chat-content w-full py-4" style={{ paddingBottom: 'calc(var(--composer-dock-h, 0px) + 16px)' }}>
              {chatHistoryError && (
                <Notice
                  key={`chat-history:${session.id}:${chatHistoryError}`}
                  tone="error"
                  title="Chat history unavailable"
                  dismissLabel="Dismiss chat history error"
                  actions={(
                    <Button size="sm" variant="secondary" loading={chatHistoryRetryingIds.has(session.id)} onClick={() => void retryChatHistory(session.id)}>
                      Retry
                    </Button>
                  )}
                >
                  {chatHistoryError}
                </Notice>
              )}
              <InlineSubagentProvider key={session.id} sourceChatId={session.id} onOpenManagedRun={runId => {
                const parent = useAppStore.getState().sessions.find(candidate => candidate.id === session.id)
                if (parent) {
                  useAppStore.getState().setActiveSession(parent.id)
                  openSubagentView(parent, { kind: 'managed', runId })
                }
              }}>
              <SubAgentDisclosureProvider expandedTools={subagentDisclosures}>
              <div className="uam-message-list space-y-1.5" aria-busy={historyLoading}>
              {historyLoading && <div role="status" className="conversation-empty">
                <div className="conversation-skeleton" aria-hidden>
                  <span className="conversation-skeleton__bubble" />
                  <div className="conversation-skeleton__text">
                    <span className="conversation-skeleton__line" />
                    <span className="conversation-skeleton__line" />
                    <span className="conversation-skeleton__line conversation-skeleton__line--mid" />
                    <span className="conversation-skeleton__line conversation-skeleton__line--short" />
                  </div>
                  <span className="conversation-skeleton__bubble conversation-skeleton__bubble--short" />
                  <div className="conversation-skeleton__text">
                    <span className="conversation-skeleton__line" />
                    <span className="conversation-skeleton__line conversation-skeleton__line--short" />
                  </div>
                </div>
                <span>Loading chat history…</span>
              </div>}
              {(earliestRenderedMessageIndex > 0 || historyStartIndex > 0) && (
                <div className="flex justify-center">
                  <Button
                    variant="secondary"
                    size="sm"
                    loading={olderHistoryLoading}
                    onClick={() => {
                      if (earliestRenderedMessageIndex > 0) {
                        setRenderedMessageCount((current) => current + RENDERED_MESSAGE_BATCH_SIZE)
                      } else if (!olderHistoryLoading) {
                        setOlderHistoryLoading(true)
                        void loadOlderSessionMessages(session.id).then((loaded) => {
                          if (loaded) setRenderedMessageCount((current) => current + RENDERED_MESSAGE_BATCH_SIZE)
                        }).finally(() => {
                          setOlderHistoryLoading(false)
                        })
                      }
                    }}
                  >
                    Show earlier messages
                  </Button>
                </div>
              )}
              {transcriptRows}
              {acp?.processing && turnEvents.length === 0 && (activeWorkMessageIndex < 0 || unsentProjection.hiddenIndices.has(activeWorkMessageIndex)) && (
                <div
                  data-testid="turn-starting"
                  role="status"
                  className="uam-turn-starting px-4 py-2 text-xs"
                  style={{ color: 'var(--text-3)' }}
                >
                  <ConversationWork
                    headerOnly active startedAt={turnClockStart} duration="0s"
                    disclosureState={liveWorkDisclosure} events={[]} tools={[]}
                    onSelectTool={selectLiveTool}
                  />
                </div>
              )}
              {(turnEvents.length > 0 || pendingPermission || pendingUserInput) &&
                !renderTimelineAfterUser && !renderTimelineAtAssistant &&
                (!completedFallbackAlreadyPersisted || pendingPermission || pendingUserInput) && (
                <MessageFrame
                  key={`turn-${turnSerial}-fallback`}
                  role="assistant"
                  assistantLabel={reviewAssistantLabel(currentProviderName, messages[turnAssistantMessageIndex] ?? messages[turnUserMessageIndex])}
                  streaming={Boolean(acp?.processing)}
                >
                  <WorkSectionContext.Provider value={activeWorkSectionId ? ALWAYS_OPEN_WORK_SECTION : null}>
                  <TurnTimelineContent
                    disclosureState={liveWorkDisclosure}
                    key={`turn-${turnSerial}-fallback-content`}
                    startedAt={turnClockStart}
                          interrupted={Boolean(messages[turnAssistantMessageIndex]?.interrupted)}
                          stopReason={messages[turnAssistantMessageIndex]?.stopReason}
                          events={turnEvents}
                      tools={liveTools}
                      planSummary={acp?.planSummary ?? ''}
                      planEntries={livePlanEntries}
                      planActions={activePlanActions}
                      pendingPermission={pendingPermission ?? null}
                      pendingUserInput={pendingUserInput ?? null}
                      waitIsStale={acp?.waitIsStale}
                      waitStaleReason={acp?.waitStaleReason}
                      waitSeconds={acp?.waitSeconds}
                      onSelectTool={selectLiveTool}
                      onResolvePermission={resolveLivePermission}
                    onCancelTurn={cancelLiveTurn}
                    onStopRuntime={stopLiveRuntime}
                    sourceChatId={session.id}
                    active={Boolean(acp?.processing)}
                    workingMode={workingDisplayMode}
                    workedSeconds={turnWorkedSeconds}
                  />
                  </WorkSectionContext.Provider>
                </MessageFrame>
              )}
              {!chatWorking && !historyIncomplete && (repositoryChanges || (isGitWorktree && latestAssistantMessage?.checkpointSha)) && (
                <section
                  aria-label="Review changes"
                  className="uam-changes-card mx-4 overflow-hidden rounded-md text-xs"
                  style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}
                >
                  <div className="flex flex-wrap items-center justify-between gap-2 px-3 py-2 font-medium" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text)' }}>
                    <div className="flex min-w-0 items-baseline gap-2">
                      <span>Chat changes</span>
                      {repositoryChanges && (
                        <span className="font-normal" style={{ color: 'var(--text-3)' }}>
                          {repositoryChanges.changedFiles.length} changed file{repositoryChanges.changedFiles.length === 1 ? '' : 's'} · +{repositoryChanges.changedFiles.reduce((total, file) => total + file.additions, 0)} / -{repositoryChanges.changedFiles.reduce((total, file) => total + file.deletions, 0)}
                        </span>
                      )}
                      {repositoryChanges && (() => {
                        const totals = changedLineTotals(repositoryChanges.changedFiles)
                        return (
                          <span className="flex gap-2 font-mono font-normal">
                            <span style={{ color: 'var(--success)' }}>+{totals.additions}</span>
                            <span style={{ color: 'var(--error)' }}>-{totals.deletions}</span>
                          </span>
                        )
                      })()}
                    </div>
                    <Button style={isCompanionContext() ? { display: 'none' } : undefined} type="button" size="sm" variant="secondary" aria-label="Open commit panel" onClick={() => setCommitPanelOpen(true)}>
                      Open commit panel
                    </Button>
                  </div>
                  {repositoryChanges && (
                    <ChangedFilesTree
                      files={repositoryChanges.changedFiles}
                      selectedPath={selectedRepositoryFile?.path}
                      onOpenFile={setSelectedRepositoryFile}
                    />
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
              <ChatStatusStrip open={Boolean(acp?.compacting)} tone="info" scene={<CompactingScene />} title="Compacting context" />
              <SubagentWaitStatus binding={acp} />
              <ChatStatusStrip open={Boolean(stoppedStatusLabel)} tone="warning" scene={<SleepingScene />} title={stoppedStatusLabel} timeMode="since" />
              </div>
              </SubAgentDisclosureProvider>
              </InlineSubagentProvider>
            </div>
            {/* Follow the padded end so the latest output clears the floating composer. */}
            <div ref={bottomRef} />
          </div>
          {session.messagesDigest && <MessageNavigationRail key={session.id}
            userMessageIndices={navigableUserMessageIndices} startIndex={historyStartIndex} messages={messages}
            currentIndex={searchNavigation?.messageIndex ?? Math.max(0, historyStartIndex + messages.length - 1)}
            viewportRef={scrollRef} busy={searchNavigation?.phase === 'loading'}
            onNavigate={(index) => { isNearBottomRef.current = false; void useAppStore.getState().jumpToSessionMessage(session.id, index) }} />}
          {showScrollToBottom && (
            <div className="pointer-events-none absolute inset-x-0 z-10 flex justify-center" style={{ bottom: 'calc(var(--composer-dock-h, 0px) + 12px)' }}>
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

        <div ref={showComposer ? composerDockRef : undefined} className={showComposer ? `uam-composer-dock${composerCentred ? ' uam-composer-dock--centred' : ''}` : 'contents'}>
        {displayedGoal && goalsPopupOpen && (
          <ViewportMenu ref={goalsPopupRef} anchorRef={goalsTriggerRef} side="top" role="region" aria-label="Chat goals" id={goalsPopupId} manageFocus={false}
            onKeyDownCapture={(event) => {
              if (event.key !== 'Escape' || getTopmostModal() || !event.currentTarget.contains(event.target as Node)) return
              event.preventDefault(); event.stopPropagation()
              setGoalsPopupOwner(null)
              goalsTriggerRef.current?.focus()
            }}
            className="conversation-goals-popup"
            style={{ width: 560, maxHeight: 'min(360px, calc(100vh - 16px))' }}>
          <GoalBanner key={goalOwner}
            goal={displayedGoal}
            onComplete={handleCompleteGoal}
            onPause={handlePauseGoal}
			onResume={session.importedReadOnly ? undefined : () => void handleResumeGoal()}
            onRestart={session.importedReadOnly ? undefined : () => void runGoalMutation(
              () => resumeGoal(session.id, displayedGoal.id, true), 'Failed to restart goal.',
            )}
            turnRunning={Boolean(acp?.processing || cli?.processing)}
            resumePending={goalSubmitting}
            onRemove={handleRemoveGoal}
            onEdit={displayedGoal.executionOwner !== 'provider' && displayedGoal.status !== 'complete' ? handleEditGoal : undefined}
            workerModelLabel={displayedGoalWorkerLabel}
            reviewerModelLabel={displayedGoalReviewerLabel}
          />
          </ViewportMenu>
        )}

        {composerCentred && <div className="uam-composer-region uam-composer-centre-heading">What would you like to work on?</div>}
        <form
          ref={composerFormRef}
          onSubmit={submit}
          className={showComposer ? "flex-shrink-0" : "contents"}
        >
            <div className={showComposer ? "uam-chat-content uam-composer-region flex flex-col p-3" : "uam-chat-content flex shrink-0 flex-col"}>
            {confirmYolo && (
              <Notice
                tone="warning"
                title="Enable YOLO?"
                dismissLabel="Dismiss YOLO warning"
                onDismiss={() => setConfirmYolo(false)}
                actions={(
                  <>
                    <Button type="button" size="sm" variant="secondary" onClick={() => setConfirmYolo(false)}>Cancel</Button>
                    <Button type="button" size="sm" variant="danger" onClick={() => {
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
                    <Button type="button" size="sm" variant="secondary" onClick={() => setRollbackConfirmation(null)}>Cancel</Button>
                    <Button type="button" size="sm" variant="danger" loading={workspaceActionBusy} onClick={() => void confirmLatestTurnRollback()}>Roll back turn</Button>
                  </>
                )}
              >
                This returns the isolated workspace to before the selected turn. {rollbackConfirmation.diff}
              </Notice>
            )}
            {messageBranchError && (
              <Notice key={`branch:${messageBranchError}`} tone="error" title={editingMessageSource.kind === 'unsent' ? 'Message edit failed' : 'Message branch failed'} dismissLabel={editingMessageSource.kind === 'unsent' ? 'Dismiss message edit error' : 'Dismiss message branch error'} onDismiss={() => setMessageBranchError('')}>
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
            {acp && currentAcpError && currentAcpErrorKey !== dismissedAcpErrorKey && (
              <div
                role="alert"
                className="uam-notice mb-2 flex items-start gap-2 rounded-md px-2 py-1.5 text-xs"
                style={{
                  border: '1px solid color-mix(in srgb, var(--red) 42%, var(--border))',
                  background: 'color-mix(in srgb, var(--red) 9%, var(--surface))',
                  color: 'var(--red)',
                }}
              >
                <span className="min-w-0 flex-1" style={{ overflowWrap: 'anywhere' }}>{currentErrorSource}: {currentAcpError}</span>
                <span className="flex shrink-0 items-center gap-1">
                  <CopyTextButton text={buildAcpErrorCopyText({ ...acp, lastError: currentAcpError }, currentErrorTitle)} label="Copy error" title="Copy error details" />
                  <IconButton icon={<X size={13} />} size="sm" label="Dismiss composer error" onClick={() => setDismissedAcpErrorKey(currentAcpErrorKey)} />
                </span>
              </div>
            )}
            {claudePlanPrompt !== null && (
              <Notice
                tone="warning"
                title="Claude Plan mode"
                dismissLabel="Dismiss Claude Plan mode warning"
                onDismiss={() => setClaudePlanPrompt(null)}
                actions={(
                  <>
                  <Button type="button"
                    size="sm"
                    variant="primary"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('acceptEdits')}
                  >
                    Accept edits and proceed
                  </Button>
                  <Button type="button"
                    size="sm"
                    variant="secondary"
                    disabled={submitting}
                    onClick={() => void resolveClaudePlanPrompt('default')}
                  >
                    Review each edit
                  </Button>
                  <Button type="button"
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
            {!showComposer && dictationError && <Notice tone="error" title="Dictation failed" dismissLabel="Dismiss dictation error" onDismiss={dismissDictationError}>{dictationError}</Notice>}
            <InlinePromptQueue key={session.id}
              held={Boolean(acp?.queueHeld && acp.queuedPrompts?.length)} onResume={(expected) => resumeQueuedAcpPrompts(session.id, expected)} prompts={acp?.queuedPrompts ?? []} disabled={steering || session.importedReadOnly}
              onEdit={(id, revision, text, retainedAttachmentIds) => editQueuedAcpPrompt(session.id, id, revision, text, retainedAttachmentIds)}
              onReorder={(expected, orderedIds) => reorderQueuedAcpPrompts(session.id, expected, orderedIds)}
              onRemove={(index, expected) => expected ? removeQueuedAcpPrompt(session.id, index, expected) : removeQueuedAcpPrompt(session.id, index)}
              onSteer={async (index, expected) => {
                const submittedSessionId = session.id
                setSteering(true)
                try { return await (expected ? steerQueuedAcpPrompt(submittedSessionId, index, expected) : steerQueuedAcpPrompt(submittedSessionId, index)) }
                finally { if (currentSessionIdRef.current === submittedSessionId) setSteering(false) }
              }}
            />
            {showComposer && (!isCompanionContext() || displayedGoal || showSideChatControls) && <div className="uam-composer-tabs" data-workspace={(!isCompanionContext() || displayedGoal) ? 'true' : undefined}>
            {(!isCompanionContext() || displayedGoal) && <div className="uam-composer-workspace-tab" data-worktree={isGitWorktree || undefined}>
            {!isCompanionContext() && (
              <>
                <div ref={workspaceMenuRef} className="relative shrink-0">
                  <IconButton
                    size="sm"
                    disabled={!workspaceDirectory}
                    onClick={() => {
                      setWorkspaceMenuOpen((open) => !open)
                      setGoalsPopupOwner(null)
                      setModelOpen(false)
                    }}
                    icon={<CustomIcon icon={folderIcon} fallback={<ComposerIcon name="folder" size={14} />} />}
                    label={workspaceDirectory ? 'Workspace actions' : 'Workspace not selected'}
                    aria-haspopup="menu"
                    aria-expanded={workspaceMenuOpen}
                    aria-controls={workspaceMenuOpen ? workspaceMenuId : undefined}
                    tooltipSide="bottom"
                  />
                  {remoteBrowseKey === remoteContextKey && recoveryHost && (
                    <RemoteDirectoryBrowser key={remoteContextKey} host={recoveryHost} initialPath={workspaceDirectory} onCancel={() => setRemoteBrowseKey('')} onSelect={(path) => { setRemoteBrowseKey(''); void stageDirectoryPaths([path]) }} />
                  )}
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
                        <Tooltip label={workspaceDirectory} side="right">
                          <div className="flex items-center gap-2 px-2 py-1 text-[11px]" style={{ color: 'var(--green)' }}>
                            <ComposerIcon name="git-tree" size={14} />
                            <span className="truncate">{session.workspaceBranchName || 'Git worktree'}</span>
                          </div>
                        </Tooltip>
                      )}
                      {remoteComputerUseDisabled ? (
                        <div className="px-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}>
                          This directory lives on the remote computer. Use this chat or its CLI view for target-side work.
                          <button type="button" role="menuitem" className="uam-menu-select__option mt-2 flex w-full items-center gap-2 rounded-md px-2 py-2 text-left" onClick={() => { setWorkspaceMenuOpen(false); void openWorkspaceTerminal() }}><ComposerIcon name="terminal" size={14} /><span>Open terminal</span></button>
                          <button type="button" role="menuitem" disabled={!recoveryHost || recoveryHost.runnerStatus !== 'ready' || workspaceActionBusy} className="uam-menu-select__option flex w-full items-center gap-2 px-2 py-2 text-left" onClick={() => { setWorkspaceMenuOpen(false); setRemoteBrowseKey(remoteContextKey) }}><ComposerIcon name="folder" size={14} /><span>Attach remote directory</span></button>
                          <label className="mt-2 grid gap-1">
                            Move this chat to
                            <select aria-label="Move chat workspace" value={remoteMoveFolderId} disabled={remoteMoveDisabled} onChange={(event) => setRemoteMoveFolderId(event.target.value)} className="px-2 py-1" style={{ background: '#000', color: '#fff', border: '1px solid var(--border)' }}>
                              <option value="">Select workspace on {recoveryHost?.label || 'this host'}</option>
                              {workspaceFolders.filter((folder) => folder.executionHostId === session.executionHostId && folder.id !== session.folderId).map((folder) => <option key={folder.id} value={folder.id}>{folder.name} · {folder.directory}</option>)}
                            </select>
                          </label>
                          <button type="button" role="menuitem" disabled={remoteMoveDisabled || !remoteMoveFolderId} aria-busy={workspaceActionBusy || undefined} className="uam-menu-select__option mt-1 w-full px-2 py-2 text-left" onClick={() => void moveRemoteWorkspace()}>{workspaceActionBusy ? 'Moving chat…' : 'Move chat'}</button>
                          <div className="mt-1">History and goals stay with this chat. A new provider session starts in the selected workspace.</div>
                          {Boolean(acp?.running || cli?.running) && <div className="mt-1">Stop the chat before moving its workspace.</div>}
                          <label className="mt-2 flex items-center gap-2">
                            <input type="checkbox" checked={session.remoteRecoveryEnabled ?? false} disabled={!recoveryHost?.startupEnabled || recoveryHost.startupStatus !== 'enabled'} onChange={(event) => {
                              void sendToCEF({ action: 'setChatRemoteRecovery', payload: { chatId: session.id, enabled: event.target.checked } }).then((response) => {
                                if (!response.ok) setWorkspaceFeedback({ tone: 'error', message: response.error || 'Chat recovery could not be saved.' })
                              })
                            }} />
                            Recover this chat after host restart
                          </label>
                          {(!recoveryHost?.startupEnabled || recoveryHost.startupStatus !== 'enabled') && <div className="mt-1">Enable runner startup in SSH host settings first.</div>}
                          {session.remoteRecoveryState === 'blocked' && <div className="mt-1" role="status">Recovery blocked. Review the last turn before retrying.</div>}
                        </div>
                      ) : (
                        <>
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
                        </>
                      )}
                    </ViewportMenu>
                  )}
                </div>
                <span className="inline-flex min-w-0 items-center gap-1 truncate text-xs" title={workspaceDirectory}>
                  <span className="truncate">{workspaceDirectory.replace(/\\/g, '/').split('/').filter(Boolean).pop() || 'No workspace'}</span>
                  {composerBranch && (branchSwitching
                    ? <ChatBranchPicker chatId={session.id} branch={composerBranch} disabled={workspaceActionsDisabled} disabledReason="Wait for the agent to finish before switching branches." onSwitched={handleBranchSwitched} />
                    : <span className="inline-flex min-w-0 items-center gap-1 truncate" title={composerBranch}><GitBranch size={12} aria-hidden /><span className="truncate font-mono">{composerBranch}</span></span>)}
                  {isGitWorktree && <span className="uam-composer-workspace-tab__badge">Worktree</span>}
                </span>
              </>
            )}
            {displayedGoal && <button ref={goalsTriggerRef} type="button" aria-label="Goals" aria-expanded={goalsPopupOpen}
              aria-controls={goalsPopupOpen ? goalsPopupId : undefined} aria-haspopup="true"
              className="conversation-goal-chip" title={displayedGoal.objective}
              onClick={() => { setGoalsPopupOwner(goalsPopupOpen ? null : goalOwner); setWorkspaceMenuOpen(false); setModelOpen(false) }}
              onKeyDown={(event) => {
                if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); setGoalsPopupOwner(goalOwner) }
                else if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); setGoalsPopupOwner(null) }
              }}>
              <Target size={13} aria-hidden /><span className="conversation-goal-chip__status">{displayedGoal.status}</span><span className="conversation-goal-chip__title">{displayedGoal.objective}</span>
            </button>}
            </div>}
            <div className="uam-composer-tabs__end">
              {showSideChatControls && (session.temporaryParentChatId ? (
                <div className="uam-composer-side-controls" role="group" aria-label="Side chat">
                  <span className="uam-composer-side-controls__pill" title="Main chat context is reference only">
                    <MessageSquareDashed size={12} aria-hidden />
                    Side chat
                  </span>
                  <IconButton size="sm" icon={<CornerUpLeft size={14} aria-hidden />} label="Return to main chat" tooltip="Back to the main chat; this side chat stays open (/side-return)" onClick={() => void navigateSide('return')} />
                  <IconButton size="sm" icon={<X size={14} aria-hidden />} label="Dismiss side chat" tooltip="Close and discard this side chat (/side-dismiss)" disabled={sideBusy} onClick={() => void navigateSide('dismiss')} />
                </div>
              ) : (
                <div className="uam-composer-side-controls">
                  <Button type="button" size="sm" variant="ghost" leadingIcon={<MessageSquareDashed size={13} aria-hidden />} title="Resume the side chat (/side)" onClick={() => void navigateSide()}>Resume side chat</Button>
                </div>
              ))}
            </div>
            </div>}
            {pendingUserInput && !session.importedReadOnly && (
              <UserInputInlineCard
                key={`${session.id}:${pendingUserInput.requestId}:${pendingUserInput.itemId}`}
                input={pendingUserInput}
                onResolve={(requestId, answers) => resolveAcpUserInput(session.id, requestId, answers)}
                waitIsStale={acp?.waitIsStale}
                waitStaleReason={acp?.waitStaleReason}
                waitSeconds={acp?.waitSeconds}
                onCancelTurn={cancelLiveTurn}
                onStopRuntime={stopLiveRuntime}
              />
            )}
            {editingMessageIndex !== null && <div className="conversation-edit-strip" role="status">
              <Pencil size={13} aria-hidden />
              <span className="min-w-0 flex-1 truncate">{editingMessageSource.kind === 'unsent' ? 'Editing unsent message. Saving updates this input.' : 'Editing message. Saving creates a new branch.'}</span>
              <Button type="button" size="sm" variant="ghost" disabled={branchingMessageIndex !== null} onClick={cancelEditingMessage}>Cancel</Button>
            </div>}
            {(showComposer || editingMessageIndex !== null) && <div
              className="uam-composer-surface"
              onDragOver={(event) => event.preventDefault()}
              onDrop={onComposerDrop}
            >
            {editingMessageIndex === null && (markdownStoreAttachments.length > 0 || composerAttachments.length > 0) && (
              <div className="flex flex-wrap gap-2 px-3 pt-3">
                {markdownStoreAttachments.map((entry) => (
                  <ContextChip
                    key={entry.filePath}
                    kind="skill"
                    name={entry.title || entry.filePath.split(/[\\/]/).pop() || 'skill'}
                    detail={entry.filePath}
                    onRemove={() => detachMarkdownStoreEntry(session.id, entry.filePath)}
                    removeLabel={`Remove ${entry.title || 'skill'} attachment`}
                  />
                ))}
                {composerAttachments.map((attachment) => (
                  <ContextChip
                    key={attachment.id}
                    kind={attachmentChipKind(attachment)}
                    name={attachment.name}
                    detail={attachment.error || attachmentLabel(attachment)}
                    path={attachment.status === 'ready' ? attachment.path : undefined}
                    meta={attachment.status === 'staging' ? 'copying' : undefined}
                    error={attachment.status === 'failed'}
                    onRemove={() => {
                      removedAttachmentIdsRef.current.add(attachment.id)
                      setComposerAttachments((current) => current.filter((item) => item.id !== attachment.id))
                    }}
                    removeLabel={`Remove ${attachment.name} attachment`}
                  />
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
            {activeSlashGroups.map((group, groupIndex) => {
              const path = group.id.slice('md-group:'.length)
              const anchor = { current: slashGroupButtonRefs.current[path] }
              if (!group.groupEntries || !anchor.current) return null
              return <ViewportMenu key={group.id} id={`${slashListboxId}-group-${groupIndex}`} anchorRef={anchor} side="right" role="menu" manageFocus={false} aria-label={`${path} skills`} className="animate-fade-in" style={{ width: 280, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}>
                <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>{path}</div>
                {group.groupEntries.map((command, index) => {
                  const childPath = command.id.startsWith('md-group:') ? command.id.slice('md-group:'.length) : path
                  const active = group === activeSlashGroup && index === Math.min(slashGroupIndex, group.groupEntries!.length - 1)
                  return <button key={command.id} id={`${slashListboxId}-group-${groupIndex}-option-${index}`} type="button" role="menuitem" aria-haspopup={command.groupEntries ? 'menu' : undefined} aria-expanded={command.groupEntries ? slashGroup === childPath : undefined} onMouseEnter={() => { setSlashGroupIndex(command.groupEntries ? 0 : index); setSlashGroup(childPath) }} onMouseDown={(event) => { event.preventDefault(); runSlashCommand(command) }} className={`uam-menu-select__option w-full flex items-start gap-2 px-2 py-2 text-left${active ? ' is-selected' : ''}`} style={{ borderRadius: 6, color: active ? 'var(--text)' : 'var(--text-2)' }}>{command.groupEntries ? <BookOpen size={15} className="mt-0.5 shrink-0" aria-hidden /> : <FileText size={15} className="mt-0.5 shrink-0" aria-hidden />}<span className="min-w-0 flex-1"><span className="block font-mono text-sm" style={{ color: active ? 'var(--accent)' : 'var(--text)' }}>{command.label}{command.groupEntries && <span ref={(element) => { slashGroupButtonRefs.current[childPath] = element }}><ChevronRight className="ml-1 inline" size={14} aria-hidden /></span>}</span><span className="block truncate text-xs" style={{ color: 'var(--text-3)' }}>{command.hint}</span></span></button>
                })}
              </ViewportMenu>
            })}
            <div className="flex items-center gap-2 py-1 text-xs">
              <Button size="sm" variant="ghost" disabled={!voice.available || dictationActive} aria-pressed={voice.active} onClick={voice.toggle}>{voice.active ? 'Stop voice mode' : 'Start voice mode'}</Button>
              <Button size="sm" variant="ghost" disabled={!voice.available || dictationActive || (!voice.speaking && !messages.some(message => message.role === 'assistant' && !message.isStreaming && message.content.trim()))} onClick={() => {
                if (voice.speaking) voice.stop()
                else voice.read([...messages].reverse().find(message => message.role === 'assistant' && !message.isStreaming && message.content.trim())?.content || '')
              }}>{voice.speaking ? 'Stop speaking' : 'Read response'}</Button>
              {voice.status && <span role="status">{voice.status}</span>}
              {voice.error && <span role="alert" style={{ color: 'var(--red)' }}>{voice.error}</span>}
              {isCompanionContext() && <span>Use your keyboard microphone for dictation.</span>}
            </div>
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
            <div className="uam-composer-input-row">
              {showComposer && featurePreference === 'uam' && <ComposerAgentSelector
                agents={uamAgents}
                agentId={selectedUamAgentId}
                nextTurn={Boolean(providerAcp?.processing)}
                onSelectUamAgent={(agentId) => void setSessionUamAgent(session.id, agentId)}
              />}

            <textarea
              ref={composerTextareaRef}
              value={editingMessageIndex !== null ? editingMessageText : draft}
              aria-label={editingMessageIndex !== null ? "Edit message" : undefined}
              onChange={(event) => {
                if (editingMessageIndex !== null) { setEditingMessageText(event.target.value); return }
                setDraft(event.target.value)
                setComposerSelection({ start: event.target.selectionStart, end: event.target.selectionEnd })
                setSlashGroup('')
                setPermissionMenuOpen(false)
              }}
              onSelect={(event) => {
                if (draft.includes('/')) {
                  setComposerSelection({ start: event.currentTarget.selectionStart, end: event.currentTarget.selectionEnd })
                }
              }}
              onKeyDown={onComposerKeyDown}
              onPaste={editingMessageIndex === null ? onComposerPaste : undefined}
              rows={1}
              placeholder={`Message ${currentProviderName} · / for commands`}
              disabled={dictationActive || session.importedReadOnly || branchingMessageIndex !== null}
              aria-describedby={dictationActive || dictationError ? `dictation-status-${session.id}` : undefined}
              aria-haspopup="listbox"
              aria-expanded={slashOpen}
              aria-controls={controlledSlashMenuIds}
              aria-activedescendant={activeSlashOptionId}
              aria-autocomplete="list"
              className="uam-composer-textarea w-full resize-none text-sm"
            />
            </div>
            {editingMessageIndex !== null && (
              <div className="flex flex-wrap items-center justify-end gap-2 px-3 pb-2">
                {editingMessageAttachments.map((attachment) => (
                  <div key={`${attachment.type === 'markdown-store' ? 'skill' : 'native'}:${attachment.id}`} className="inline-flex items-center gap-1">
                    <AttachmentList attachments={[attachment]} chatId={session.id} nativeMessageIndex={messages[editingMessageIndex - historyStartIndex]?.nativeMessageIndex} />
                    <IconButton icon={<X size={14} />} label={`Remove ${attachment.name} from edited message`} disabled={branchingMessageIndex !== null} onClick={() => removeEditingAttachment(attachment.id, attachment.type === 'markdown-store')} />
                  </div>
                ))}
                <Button type="button" size="sm" variant="primary" loading={branchingMessageIndex !== null} disabled={(!editingMessageText.trim() && editingMessageAttachments.length === 0) || !canChangeProvider} onClick={() => void submit()}>{editingMessageSource.kind === 'unsent' ? 'Save changes' : 'Save to new branch'}</Button>
              </div>
            )}
            {showComposer && <>
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
              draftPending={editingMessageIndex === null && draft.trim().length > 0}
              acp={providerAcp}
              provider={currentProvider}
              providers={providers}
              providerId={currentProviderId}
              canChangeProvider={canChangeProvider}
              canSend={canSend}
              runtimeStatusLabel={statusLabel(providerRuntime)}
              runtimeStatusColor={statusColor(providerRuntime)}
              modelId={session.smallModelMode ? activeGoal?.workerModelId || currentModelId : currentModelId}
              reviewerModelId={session.smallModelMode ? activeGoal?.reviewerModelId || currentReviewerModelId : currentReviewerModelId}
              includeDefaultModel={showUnresolvedDefaultModel}
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
              computerUseMode={computerUseMode}
              computerUseAwaitingTarget={computerUseMode && !session.computerUseTargetId}
              memoryLevel={currentMemoryLevel}
              defaultMemoryLevel={defaultMemoryLevel}
              smallModelMode={session.smallModelMode ?? false}
              modelOpen={modelOpen}
              modelMenuRef={modelMenuRef}
			  onToggleModel={() => {
				if (!modelOpen && isOpenCodeProvider(currentProvider, currentProviderId) && !providerAcp?.modelsLoading && !providerAcp?.availableModels.length) {
				  void discoverProviderModels('', currentProviderId, workspaceDirectory, session.executionHostId || 'local')
				}
                setModelOpen((value) => !value)
                setWorkspaceMenuOpen(false)
              }}
              workspaceDirectory={workspaceDirectory}
              executionHostId={session.executionHostId || 'local'}
              onSelectProviderModel={(providerId, modelId) => {
                setModelOpen(false)
                if (providerId !== currentProviderId) {
                  setProviderHandoffModelId(modelId)
                  setProviderHandoffTargetId(providerId)
                }
              }}
              onRefreshModels={refreshDisplayedModels}
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
              onToggleComputerUseMode={() => void setComputerUseActive(!computerUseMode)}
              onSelectMemoryLevel={(level) => void setSessionMemoryLevel(session.id, level)}
              onToggleSmallModelMode={() => void setSessionSmallModelMode(session.id, !(session.smallModelMode ?? false))}
              goalArmed={goalArmNextMessage}
              goalActive={Boolean(activeGoal)}
              goalPaused={Boolean(goalPaused)}
              defaultGoalTokenBudget={defaultGoalTokenBudget}
              onToggleGoal={handleToggleGoal}
              onSetDefaultGoalTokenBudget={(value) => setDefaultGoalTokenBudget(session.id, value)}
              onCancelTurn={cancelLiveTurn}
              onAttachFile={() => fileInputRef.current?.click()}
              onOpenMarkdownStore={() => void openMarkdownStore()}
              dictationState={dictationState}
              dictationError={dictationError}
              dictationElapsedSeconds={dictationElapsedSeconds}
              dictationAvailable={dictationAvailable && !voice.active && !voice.speaking}
              onToggleDictation={() => {
                if (dictationActiveRef.current) void stopDictation()
                else void startDictation()
              }}
            />
            </>}
            </div>}
          </div>
        </form>
        </div>
      </div>
    </div>
    </FileInspectionProvider>
  )
})
