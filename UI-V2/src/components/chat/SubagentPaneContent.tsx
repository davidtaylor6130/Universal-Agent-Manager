import { useEffect, useRef, useState } from 'react'
import { usePassiveHistoryWindow } from './usePassiveHistoryWindow'
import { useAppStore } from '../../store/useAppStore'
import { hasNativeSubagentLineage, subagentParentFor, type SubagentRoute } from '../../utils/subagentNavigation'
import type { Session } from '../../types/session'
import { Notice } from '../ui'
import { ManagedSubagentConversation } from './ManagedSubagentConversation'
import { SubagentHeader, SubagentTranscript, SubagentTranscriptLoading, type OlderHistoryOutcome } from './SubagentTranscript'

export function SubagentPaneContent({ parent, route, active = true, navigationKey = '', onBack }: { parent: Session; route: SubagentRoute; active?: boolean; navigationKey?: string; onBack?: () => void }) {
  const visitKey = navigationKey ? JSON.stringify([navigationKey, readIdentity(parent)]) : ''
  return route.kind === 'managed'
    ? <ManagedSubagentConversation sourceChatId={parent.id} parentName={parent.name} onBack={onBack} runId={route.runId} transcriptChatId={route.transcriptChatId} title={route.title} status={route.status} autoFocus={active} navigationKey={visitKey} />
    : <NativeConversation key={route.chatId} parent={parent} onBack={onBack} chatId={route.chatId} autoFocus={active} navigationKey={visitKey} />
}

const readIdentity = (session: Session | undefined) => session ? JSON.stringify([
  session.id, session.createdAt.getTime(), session.providerId, session.executionHostId,
  session.workspaceDirectory, session.workspaceWorktreeDirectory, session.folderId,
  session.importedReadOnly, session.subagentParentChatId, session.subagentParentCreatedAt,
]) : ''

// Passive journal Sessions retain the exact attested history reference as their ID.
// The existing native import endpoint validates the reference; lineage grants no execution authority.
function nativeJournalReference(child: Session | undefined) {
  const prefix = child?.providerId === 'gemini-cli' ? 'gemini' : child?.providerId === 'claude-cli' ? 'claude' : child?.providerId === 'copilot-cli' ? 'copilot' : ''
  return child?.importedReadOnly && prefix && (child.id.startsWith(`${prefix}-agent-`) || child.id.startsWith(`${prefix}-a32-`)) ? child.id : undefined
}

/** Passive native history has no composer, permission buttons, resume or provider controls. */
type NativeConversationProps = { parent?: Session; chatId: string; autoFocus?: boolean; navigationKey?: string; onBack?: () => void; onClose?: () => void }
export function NativeConversation(props: NativeConversationProps) {
  const { parent, chatId } = props
  const child = useAppStore(state => {
    const candidate = state.sessions.find(session => session.id === chatId)
    return candidate && (parent ? subagentParentFor(candidate, state.sessions)?.id === parent.id : hasNativeSubagentLineage(candidate) && !subagentParentFor(candidate, state.sessions)) ? candidate : undefined
  })
  return <NativeConversationContent key={JSON.stringify([props.navigationKey, chatId, readIdentity(parent), readIdentity(child), child?.importedReadOnly ? [child.messagesDigest, child.messageCount] : null])} {...props} child={child} />
}

function NativeConversationContent({ parent, chatId, child, autoFocus = true, navigationKey = '', onBack, onClose }: NativeConversationProps & { child?: Session }) {
  const childIdentity = readIdentity(child)
  const parentIdentity = readIdentity(parent)
  const viewportKey = navigationKey && child ? JSON.stringify([navigationKey, 'native', parentIdentity, childIdentity, child.messagesDigest, child.messageCount]) : ''
  const messages = useAppStore(state => state.messages[chatId])
  const error = useAppStore(state => state.chatHistoryErrorBySessionId[chatId])
  const before = useAppStore(state => state.historyStartIndexBySessionId[chatId] ?? 0)
  const scopedHistory = usePassiveHistoryWindow({
    // A loaded-only history retains its existing viewport memory. Remote around-page reads require
    // the child's own bounded, nonempty catalogue; an imported flag alone cannot authorize them.
    enabled: Boolean(child?.importedReadOnly && child.messagesDigest?.trim() && child.messagesDigest.length <= 256 &&
      Number.isSafeInteger(child.messageCount) && child.messageCount! > 0),
    owner: viewportKey || JSON.stringify([navigationKey, parentIdentity, childIdentity, child?.messagesDigest, child?.messageCount]),
    childId: chatId, digest: child?.messagesDigest || '', total: child?.messageCount ?? 0,
    messages: messages ?? [], first: before,
    current: () => {
      const state = useAppStore.getState()
      const live = state.sessions.find(session => session.id === chatId)
      const liveParent = live && subagentParentFor(live, state.sessions)
      return Boolean(child && live && live.importedReadOnly && Number.isFinite(live.createdAt.getTime()) &&
        (!parent || Number.isFinite(parent.createdAt.getTime())) && readIdentity(live) === childIdentity &&
        live.messagesDigest === child.messagesDigest && live.messageCount === child.messageCount &&
        Number.isSafeInteger(live.messageCount) && live.messageCount! > 0 &&
        (parent ? readIdentity(liveParent) === parentIdentity : !liveParent && hasNativeSubagentLineage(live)))
    },
  })
  const [loading, setLoading] = useState(false)
  const [readError, setReadError] = useState('')
  const [attempt, setAttempt] = useState(0)
  const heading = useRef<HTMLHeadingElement>(null)
  useEffect(() => { if (autoFocus) heading.current?.focus({ preventScroll: true }) }, [])
  const reference = parent && nativeJournalReference(child)
  useEffect(() => {
    if (!child) return
    let disposed = false
    const current = () => {
      if (disposed) return false
      const state = useAppStore.getState()
      const liveChild = state.sessions.find(session => session.id === chatId)
      const liveParent = liveChild && subagentParentFor(liveChild, state.sessions)
      return readIdentity(liveChild) === childIdentity && (parent
        ? readIdentity(liveParent) === parentIdentity
        : !liveParent && Boolean(liveChild && hasNativeSubagentLineage(liveChild)))
    }
    setLoading(true)
    setReadError('')
    void (async () => {
      try {
        if (attempt > 0 && reference && parent) {
          const imported = await useAppStore.getState().openSubAgentSession(parent.id, reference, child.name, false)
          if (!current()) return
          if (imported !== chatId) throw new Error('Native history import unavailable')
        }
        if (current()) await useAppStore.getState().loadSessionMessages(chatId, attempt > 0)
      } catch {
        if (current()) setReadError('The native conversation could not be read. Refresh to try again.')
      } finally {
        if (current()) setLoading(false)
      }
    })()
    return () => { disposed = true }
  }, [chatId, childIdentity, parentIdentity, reference, attempt])
  // One existing child-scoped older page. The store records failures per chat, which separates a failed read from a
  // stale/superseded one. This view is keyed by child/parent identity, and the window ignores results after unmount.
  const loadOlder = async (): Promise<OlderHistoryOutcome> => {
    if (await useAppStore.getState().loadOlderSessionMessages(chatId)) return 'loaded'
    const state = useAppStore.getState()
    if ((state.historyStartIndexBySessionId[chatId] ?? 0) <= 0) return 'end'
    return state.chatHistoryErrorBySessionId[chatId] ? 'failed' : 'unchanged'
  }
  // Full-history rail marks only from this passive child's own catalogue entry; executable/other children mark loaded rows.
  const userMessageIndices = child?.importedReadOnly && child.messagesDigest ? child.userMessageIndices : undefined
  // With rows loaded, a failed read never hides them; the notice says what is still shown.
  const shown = Boolean(messages?.length)
  const failure = (error || readError) && <Notice tone="error" title={shown ? 'Native conversation partly unavailable' : 'Native conversation unavailable'} dismissLabel="Dismiss native conversation error">
    {readError || `Could not load this native conversation. ${error}`}{shown ? ' Loaded messages are still shown.' : ''}</Notice>
  return <section className="uam-managed-conversation" aria-label="Native subagent conversation">
    <SubagentHeader title={child?.name || 'Native subagent'} parentName={parent?.name} detail="Read-only" headingRef={heading} onBack={onBack} onClose={onClose}
      onRefresh={child ? () => { scopedHistory.invalidate(); setAttempt(value => value + 1) } : undefined} refreshing={loading} refreshDisabled={loading} />
    <p className="sr-only">Read-only native conversation. Follow-up messages and Stop are unavailable here.</p>
    {child && messages && messages.length > 0
      ? <SubagentTranscript messages={messages} nativeStartIndex={before} fallbackProviderId={child.providerId || ''} sessionId={chatId}
          viewportKey={viewportKey} contentVersion={messages} userMessageIndices={userMessageIndices} recoveryEpoch={attempt} label="Native conversation messages"
          loadOlder={before > 0 && !loading ? loadOlder : undefined} scopedView={scopedHistory.view} notices={failure} />
      : <div className="uam-subagent-transcript__empty">
          {failure}
          {loading ? <SubagentTranscriptLoading label="Loading native conversation…" />
            : <p role="status" className="conversation-empty">{!child || !messages
              ? 'This native conversation is unavailable. The provider has not supplied a readable transcript.'
              : 'No readable native messages are available.'}</p>}
        </div>}
  </section>
}
