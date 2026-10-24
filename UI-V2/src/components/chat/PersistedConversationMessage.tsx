import { hasUnconfirmedNativeInput } from '../../utils/outgoingMessagePresentation'
import { memo, useMemo, useRef, useState, type ReactNode } from 'react'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'
import type { AcpToolCall } from '../../store/cpp/types'
import { isCompanionContext } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { providerShortName } from '../../utils/providerMetadata'
import { isInlineSubagentLaunch } from './inlineSubagents'
import type { ConversationDelivery } from './ConversationTurn'
import { ConversationWork, WorkSectionContext, type WorkTraceDisclosureState } from './ConversationWork'
import { AttachmentList, CompactionSeparator, InterruptedDivider, PersistedMessageContent, formatWorkedDuration, goalReviewForMessage, isImageAttachment, stoppedResponseLabel, type WorkingDisplayMode } from './MessageBlocks'
import { MessageFrame, ToolCallModal } from './ToolCallViews'
import { ALWAYS_OPEN_WORK_SECTION, buildVisibleWorkSections, dayDividerLabel, previousAssistantMessage, responseFlags, reviewAssistantLabel, turnTimestampLabel } from './conversationHistoryModel'

/** Settled turn metadata must not reformat dates on composer or stream updates. */
// timezoneOffset participates in memo equality so local time text refreshes on the next render.
export const TurnStartTime = memo(function TurnStartTime({ timestamp, identity, showIdentity }: { timestamp: number; timezoneOffset: number; identity: string; showIdentity: boolean }) {
  const createdAt = new Date(timestamp)
  // Provider and model show only when they change; otherwise the time is screen-reader text (the row title shows it on hover).
  return <time className={showIdentity ? 'conversation-turn-start' : 'conversation-turn-start sr-only'} dateTime={createdAt.toISOString()}>
    {showIdentity ? identity : turnTimestampLabel(createdAt)}
  </time>
})

type PersistedPlanActions = Parameters<typeof PersistedMessageContent>[0]['planActions']

/** The one canonical persisted message row. The main transcript and read-only child conversations both render it;
 * passive children omit every mutation callback and never trigger chat-scoped reads, previews or subagent refresh. */
export const PersistedConversationMessage = memo(function PersistedConversationMessage({
  message,
  hasDeliveredResponse,
  hasStoppedResponse,
  pendingDirectAdmission,
  delivery,
  deliveryActions,
  disclosureState,
  index,
  assistantLabel,
  sessionId,
  sessionParentChatId,
  sessionBranchFromMessageIndex,
  sessionBranchMessageEdited,
  branchSessions,
  actionsDisabled,
  planActions,
  workingMode,
  retryInPlace,
  onBeginEdit,
  onCreateBranch,
  onSelectBranch,
  onSelectTool,
  passive = false,
}: {
  message: Message
  hasDeliveredResponse: boolean
  hasStoppedResponse: boolean
  pendingDirectAdmission: boolean
  delivery?: ConversationDelivery
  deliveryActions?: ReactNode
  disclosureState: WorkTraceDisclosureState
  index: number
  assistantLabel: string
  sessionId: string
  sessionParentChatId?: string
  sessionBranchFromMessageIndex?: number
  sessionBranchMessageEdited?: boolean
  branchSessions?: readonly Session[]
  actionsDisabled?: boolean
  planActions?: PersistedPlanActions
  workingMode: WorkingDisplayMode
  retryInPlace?: boolean
  onBeginEdit?: (index: number, content: string) => void
  onCreateBranch?: (index: number, content?: string, retainedAttachmentIds?: string[], retainedMarkdownStoreFiles?: string[]) => void | Promise<void>
  onSelectBranch?: (parentId: string, index: number, sessionId: string) => void
  onSelectTool: (messageId: string, toolId: string) => void
  /** Read-only history: no branch/edit/revert actions, chat-scoped reads, previews or subagent refresh. */
  passive?: boolean
}) {
  if (message.blocks?.length === 1 && (message.blocks[0].type === 'context_compaction' || message.blocks[0].type === 'context_compaction_interrupted')) {
    const block = message.blocks[0]
    return <CompactionSeparator summary={block.text} tokensBefore={block.tokensBefore} tokensAfter={block.tokensAfter} interrupted={block.type === 'context_compaction_interrupted'} />
  }
  const isUserMessage = message.role === 'user'
  const historicalInputUnknown = delivery === undefined && hasUnconfirmedNativeInput(message)
  const effectiveDelivery = delivery ?? (historicalInputUnknown ? { state: 'unconfirmed' as const,
    label: 'UAM has no confirmed write receipt for this input. It is retained here and is never resent automatically.' } : undefined)
  const branchParentId = sessionParentChatId && sessionBranchFromMessageIndex === index
    ? sessionParentChatId
    : sessionId
  const messageBranchSessions = isUserMessage && !passive && onSelectBranch
    ? (branchSessions ?? []).filter((candidate) =>
      candidate.id === branchParentId ||
      (candidate.parentChatId === branchParentId && candidate.branchFromMessageIndex === index)
    )
    : []
  const messageBranchIndex = messageBranchSessions.findIndex((candidate) => candidate.id === sessionId)
  const isBranchPoint = messageBranchSessions.length > 1 && messageBranchIndex >= 0
  const goalReview = goalReviewForMessage(message)
  const branchLabel = !passive && sessionBranchFromMessageIndex === index
    ? sessionBranchMessageEdited ? 'Edited branch' : 'Reverted branch'
    : undefined

  return (
		<>
    {(message.attachments ?? []).some(isImageAttachment) && (
      <div className={`conversation-image-attachments mb-2${isUserMessage ? ' ml-auto w-fit max-w-[85%]' : ''}`}>
        <AttachmentList attachments={(message.attachments ?? []).filter(isImageAttachment)} chatId={passive ? undefined : sessionId} nativeMessageIndex={message.nativeMessageIndex} previews={!passive} />
      </div>
    )}
    <MessageFrame
      role={message.role}
      assistantLabel={assistantLabel}
      branchLabel={branchLabel}
      branchNavigation={isBranchPoint ? {
        current: messageBranchIndex + 1,
        total: messageBranchSessions.length,
        onPrevious: () => onSelectBranch!(branchParentId, index, messageBranchSessions[messageBranchIndex - 1].id),
        onNext: () => onSelectBranch!(branchParentId, index, messageBranchSessions[messageBranchIndex + 1].id),
      } : undefined}
      goalReview={Boolean(goalReview)}
      actionsDisabled={(actionsDisabled ?? false) || historicalInputUnknown}
      delivery={isUserMessage ? effectiveDelivery : undefined}
      deliveryActions={historicalInputUnknown ? undefined : deliveryActions}
      onEdit={isUserMessage && !historicalInputUnknown && !passive && onBeginEdit && !isCompanionContext() ? () => onBeginEdit(index, message.content) : undefined}
      onRevert={isUserMessage && !historicalInputUnknown && !passive && onCreateBranch && !isCompanionContext() ? () => void onCreateBranch(index) : undefined}
      revertLabel={retryInPlace ? 'Retry message' : undefined}
    >
        <PersistedMessageContent
          message={message}
          omitImages
          disclosureState={disclosureState}
          onSelectTool={onSelectTool}
          planActions={planActions}
          sourceChatId={sessionId}
          passive={passive}
          workingMode={workingMode}
        />
		</MessageFrame>
      {isUserMessage && message.interrupted && !pendingDirectAdmission && !effectiveDelivery && !(hasDeliveredResponse && hasStoppedResponse) &&
        <InterruptedDivider label={message.acpPromptNotSent && !hasDeliveredResponse ? 'Message was not sent' : 'Response interrupted'} />}
      {!isUserMessage && stoppedResponseLabel(message.stopReason, message.interrupted) &&
        <InterruptedDivider label={stoppedResponseLabel(message.stopReason, message.interrupted)} />}
		</>
  )
})

/** Read-only child history rendered exactly like the main transcript's settled history: same work sections, headings,
 * turn metadata, disclosure and canonical rows. It has no live timeline, mutation callbacks or chat-scoped reads; the tool
 * dialog shows embedded content only (no chat ID, so no deferred fetch, managed transcript or resume). */
export function PersistedConversationHistory({ messages, start, end, nativeStartIndex = 0, fallbackProviderId, sessionId }: {
  /** Full loaded history; sections are modelled over it so a page boundary never splits a turn's metadata. */
  messages: Message[]
  start: number
  end: number
  nativeStartIndex?: number
  fallbackProviderId: string
  /** Child identity for row keys/branch math only; passive rows never use it for reads. */
  sessionId: string
}) {
  const expandWorkTraces = useAppStore((state) => state.expandWorkTraces)
  const collapsible = useAppStore((state) => state.collapsibleWorkSections)
  const workingMode = useAppStore((state) => state.workingDisplayMode)
  const providers = useAppStore((state) => state.providers)
  const [choices, setChoices] = useState<Record<string, boolean>>({})
  const disclosures = useRef(new Map<string, WorkTraceDisclosureState>()).current
  const [selectedTool, setSelectedTool] = useState<AcpToolCall | null>(null)
  const sections = useMemo(() => buildVisibleWorkSections(messages, start), [messages, start])
  const providerName = (id: string) => providerShortName(providers.find((candidate) => candidate.id === id), id)
  const selectTool = (messageId: string, toolId: string) => {
    const tool = messages.find((message) => message.id === messageId)?.toolCalls?.find((candidate) => candidate.id === toolId)
    if (tool) setSelectedTool(tool)
  }
  return <>
    {messages.slice(start, end).map((message, offset) => {
      const index = start + offset
      const section = sections.get(index)!
      const sectionExpanded = choices[section.id] ?? expandWorkTraces
      const sectionControl = collapsible ? { expanded: sectionExpanded, toggle: () => setChoices((current) => ({ ...current, [section.id]: !sectionExpanded })) } : ALWAYS_OPEN_WORK_SECTION
      const sectionCollapsed = !sectionControl.expanded
      let disclosureState = disclosures.get(message.id)
      if (!disclosureState) disclosures.set(message.id, disclosureState = {})
      if (sectionCollapsed && ((message.role === 'user' && message.continuesTurn) || (message.role === 'assistant' && index !== section.lastAssistantIndex))) {
        // Match the main transcript's retained launch slot, without granting this history a source scope.
        return message.role === 'assistant' && message.toolCalls?.some(isInlineSubagentLaunch) ? <div key={message.id} data-message-index={index + nativeStartIndex}>
          <PersistedMessageContent passive message={message} disclosureState={disclosureState} workingMode={workingMode}
            onSelectTool={selectTool} launchesOnly />
        </div> : null
      }
      const { followingResponse, hasDeliveredResponse, hasStoppedResponse } = responseFlags(messages, index)
      const firstVisibleInSection = index === Math.max(section.firstIndex, start) ||
        (sectionCollapsed && section.firstIndex < start && index === section.lastAssistantIndex)
      const sectionStart = messages[section.firstIndex]
      const sectionResponse = messages[section.firstIndex + 1]?.role === 'assistant' ? messages[section.firstIndex + 1] : undefined
      const sectionProviderId = sectionStart.providerId?.trim() || sectionResponse?.providerId?.trim() || fallbackProviderId
      const briefTurn = !section.hasTrace && section.workedSeconds < 2
      const heading = firstVisibleInSection && section.hasAssistant && !briefTurn ? <div><ConversationWork
        sectionHeading headerOnly active={false} duration={formatWorkedDuration(section.workedSeconds)} events={[]} tools={[]} collapsible={collapsible}
        onSelectTool={(toolId) => selectTool(message.id, toolId)} /></div> : null
      const turnStart = firstVisibleInSection && sectionStart.role === 'user' && Number.isFinite(sectionStart.createdAt.getTime()) ? sectionStart : undefined
      const turnModelId = sectionStart.modelId?.trim() || sectionResponse?.modelId?.trim() || ''
      const turnIdentity = [providerName(sectionProviderId), turnModelId].filter(Boolean).join(' · ')
      const previousAssistant = turnStart ? previousAssistantMessage(messages, section.firstIndex) : undefined
      const previousIdentity = previousAssistant
        ? [providerName(previousAssistant.providerId?.trim() || fallbackProviderId), previousAssistant.modelId?.trim()].filter(Boolean).join(' · ') : ''
      const priorMessage = section.firstIndex > 0 ? messages[section.firstIndex - 1] : undefined
      const dayChanged = Boolean(turnStart) && (!priorMessage || !Number.isFinite(priorMessage.createdAt.getTime()) || priorMessage.createdAt.toDateString() !== sectionStart.createdAt.toDateString())
      const messageProviderId = message.providerId?.trim() || followingResponse?.providerId?.trim() || fallbackProviderId
      return <WorkSectionContext.Provider key={message.id} value={sectionControl}>
        <div className="space-y-1" data-message-index={index + nativeStartIndex} title={turnStart ? turnTimestampLabel(turnStart.createdAt) : undefined}>
          {dayChanged && turnStart && <div className="conversation-day-divider"><span>{dayDividerLabel(turnStart.createdAt)}</span></div>}
          {turnStart && <TurnStartTime timestamp={turnStart.createdAt.getTime()} timezoneOffset={turnStart.createdAt.getTimezoneOffset()} identity={turnIdentity}
            showIdentity={Boolean(turnIdentity && (!previousAssistant || turnIdentity !== previousIdentity))} />}
          {message.role !== 'user' && heading}
          <PersistedConversationMessage passive message={message} hasDeliveredResponse={hasDeliveredResponse} hasStoppedResponse={hasStoppedResponse}
            pendingDirectAdmission={false} disclosureState={disclosureState} index={index + nativeStartIndex}
            assistantLabel={reviewAssistantLabel(providerName(messageProviderId), message)} sessionId={sessionId}
            workingMode={workingMode} onSelectTool={selectTool} />
          {message.role === 'user' && heading}
        </div>
      </WorkSectionContext.Provider>
    })}
    {selectedTool && <ToolCallModal tool={selectedTool} onClose={() => setSelectedTool(null)} />}
  </>
}
