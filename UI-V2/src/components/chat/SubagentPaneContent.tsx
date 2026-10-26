import { useEffect, useRef, useState } from 'react'
import { usePassiveHistoryWindow } from './usePassiveHistoryWindow'
import { useAppStore } from '../../store/useAppStore'
import type { AcceptedHistoryPage } from '../../store/storeTypes'
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

const nativeReadFallback = 'The native conversation could not be read. Refresh to try again.'
// Exact application-authored refusal only; provider/transport diagnostics are secondary.
const shorterNativeHistory = "Native child history is shorter than the saved conversation. Saved history was retained for reading. Refresh can succeed when this child's complete native history is available; the same shorter source cannot replace it."
type NativeHistoryFailure = { message: string; details?: string; savedRetained?: boolean }
function nativeHistoryFailure(raw = ''): NativeHistoryFailure {
  if (raw === shorterNativeHistory) return { message: shorterNativeHistory, savedRetained: true }
  const detail = raw.replace(/\u001b\[[0-?]*[ -/]*[@-~]/g, '').replace(/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/g, ' ').replace(/\s+/g, ' ').trim()
  return { message: nativeReadFallback, details: detail && detail !== nativeReadFallback ? detail.slice(0, 400) + (detail.length > 400 ? '…' : '') : undefined }
}

/** Passive native history has no composer, permission buttons, resume or provider controls. */
type NativeConversationProps = { parent?: Session; chatId: string; autoFocus?: boolean; navigationKey?: string; onBack?: () => void; onClose?: () => void }
export function NativeConversation(props: NativeConversationProps) {
  const { parent, chatId } = props
  const child = useAppStore(state => {
    const candidate = state.sessions.find(session => session.id === chatId)
    return candidate && (parent ? subagentParentFor(candidate, state.sessions)?.id === parent.id : hasNativeSubagentLineage(candidate) && !subagentParentFor(candidate, state.sessions)) ? candidate : undefined
  })
  return <NativeConversationContent key={JSON.stringify([props.navigationKey, chatId, readIdentity(parent), readIdentity(child)])} {...props} child={child} />
}

function NativeConversationContent({ parent, chatId, child, autoFocus = true, navigationKey = '', onBack, onClose }: NativeConversationProps & { child?: Session }) {
  const childIdentity = readIdentity(child)
  const parentIdentity = readIdentity(parent)
  const viewportKey = navigationKey && child ? JSON.stringify([navigationKey, 'native', parentIdentity, childIdentity]) : ''
  const [attempt, setAttempt] = useState(0)
  const consumedAttempt = useRef(0)
  const loadSerial = useRef(0)
  const activeLoad = useRef<{ key: string; pending: boolean; acceptedOwner?: string }>()
  const [catchup, setCatchup] = useState(0)
  const readOwner = JSON.stringify([navigationKey, parentIdentity, childIdentity, child?.messagesDigest, child?.messageCount, attempt])
  const [accepted, setAccepted] = useState<{ owner: string; page: AcceptedHistoryPage }>()
  const publishedPage = useAppStore(state => state.chatHistoryPageBySessionId?.[chatId])
  const [baseline, setBaseline] = useState<{ key: string; page: AcceptedHistoryPage | undefined }>()
  const hydrationKey = JSON.stringify([chatId, childIdentity, parentIdentity, parent && nativeJournalReference(child), child?.messagesDigest, attempt])
  // An accepted page attests the dataset, not exclusive ownership of the shared viewport.
  // Another reader may replace that viewport without changing this immutable page's provenance.
  const pageCurrent = (page: AcceptedHistoryPage) => {
    const state = useAppStore.getState()
    const live = state.sessions.find(session => session.id === chatId)
    return Boolean(state.chatHistoryPageBySessionId?.[chatId] === page && live &&
      readIdentity(page.session) === childIdentity && readIdentity(live) === childIdentity &&
      page.summaryDigest === live.messagesDigest && page.totalCount === live.messageCount)
  }
  const publishedCurrent = publishedPage && pageCurrent(publishedPage) && (attempt === 0 ||
    baseline?.key === hydrationKey && publishedPage !== baseline.page)
  const acceptedForRead = publishedCurrent ? { owner: readOwner, page: publishedPage } : accepted
  const messages = useAppStore(state => state.messages[chatId])
  const error = useAppStore(state => state.chatHistoryErrorBySessionId[chatId])
  const before = useAppStore(state => state.historyStartIndexBySessionId[chatId] ?? 0)
  const scopedHistory = usePassiveHistoryWindow({
    // A loaded-only history retains its existing viewport memory. Remote around-page reads require
    // the child's own bounded, nonempty catalogue; an imported flag alone cannot authorize them.
    enabled: Boolean(child?.importedReadOnly && child.messagesDigest?.trim() && child.messagesDigest.length <= 256 &&
      Number.isSafeInteger(child.messageCount) && child.messageCount! > 0),
    displayOwner: viewportKey || JSON.stringify([navigationKey, parentIdentity, childIdentity]),
    owner: readOwner, accepted: acceptedForRead,
    pageCurrent,
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
  const [readError, setReadError] = useState<NativeHistoryFailure>()
  const heading = useRef<HTMLHeadingElement>(null)
  useEffect(() => { if (autoFocus) heading.current?.focus({ preventScroll: true }) }, [])
  const reference = parent && nativeJournalReference(child)
  useEffect(() => () => { ++loadSerial.current; activeLoad.current = undefined }, [])
  useEffect(() => {
    if (!child) return
    // A successful tail read can itself correct the summary count. Let that read finish before
    // deciding whether a count-only notification needs a catch-up; never infer acceptance from it.
    const key = hydrationKey
    if (publishedCurrent && activeLoad.current?.key === key) return
    if (activeLoad.current?.key === key && (activeLoad.current.pending || activeLoad.current.acceptedOwner === readOwner)) return
    const retainedRefusal = activeLoad.current?.key === key && readError?.savedRetained ? readError : undefined
    const serial = ++loadSerial.current
    activeLoad.current = { key, pending: true }
    let adopted = false
    let adoptedOwner: string | undefined
    const explicit = attempt !== consumedAttempt.current
    consumedAttempt.current = attempt
    const current = (total = child.messageCount) => {
      if (serial !== loadSerial.current) return false
      const state = useAppStore.getState()
      const liveChild = state.sessions.find(session => session.id === chatId)
      const liveParent = liveChild && subagentParentFor(liveChild, state.sessions)
      return readIdentity(liveChild) === childIdentity && liveChild?.messagesDigest === child.messagesDigest &&
        liveChild?.messageCount === total && (parent
        ? readIdentity(liveParent) === parentIdentity
        : !liveParent && Boolean(liveChild && hasNativeSubagentLineage(liveChild)))
    }
    setLoading(true)
    setReadError(retainedRefusal)
    void (async () => {
      let refreshFailure: NativeHistoryFailure | undefined = retainedRefusal
      try {
        if (explicit && reference && parent) {
          let importError = ''
          const imported = await useAppStore.getState().openSubAgentSession(parent.id, reference, child.name, false, message => { importError = message })
          if (!current()) return
          if (imported !== chatId) {
            refreshFailure = nativeHistoryFailure(importError)
            setReadError(refreshFailure)
            if (useAppStore.getState().messages[chatId]?.length || scopedHistory.view?.messages.length) return
          }
        }
        if (current()) {
          setBaseline({ key, page: useAppStore.getState().chatHistoryPageBySessionId?.[chatId] })
          // A refused native import grants no new authority. Empty panes can still read the
          // retained saved tail through the same passive route as initial opening.
          const page = await useAppStore.getState().loadSessionMessages(chatId, refreshFailure ? false : explicit)
          if (page && current(page.totalCount) && page.chatId === chatId && page.birth === child.createdAt.getTime() &&
              page.summaryDigest === child.messagesDigest) {
            adopted = true
            adoptedOwner = JSON.stringify([navigationKey, parentIdentity, childIdentity, child.messagesDigest, page.totalCount, attempt])
            setAccepted({ owner: adoptedOwner, page })
          }
        }
      } catch {
        if (current()) setReadError(refreshFailure ?? nativeHistoryFailure())
      } finally {
        if (serial === loadSerial.current) {
          activeLoad.current = { key, pending: false, acceptedOwner: adoptedOwner }
          setLoading(false)
          const live = useAppStore.getState().sessions.find(session => session.id === chatId)
          if (!adopted && live && current(live.messageCount) && live.messageCount !== child.messageCount) setCatchup(value => value + 1)
        }
      }
    })()
  }, [chatId, childIdentity, parentIdentity, reference, readOwner, attempt, catchup])
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
  const visibleMessages = scopedHistory.view?.messages ?? messages
  const shown = Boolean(visibleMessages?.length)
  const unverified = shown && !error && !loading && activeLoad.current?.pending === false && scopedHistory.view &&
    !(acceptedForRead?.owner === readOwner && pageCurrent(acceptedForRead.page))
  const unavailable: NativeHistoryFailure | undefined = readError || (unverified ? { message: 'Loaded messages are retained. Refresh to verify the current history before reading more.', savedRetained: true } : error ? nativeHistoryFailure(error) : undefined)
  const details = unavailable?.details
  const failure = unavailable && <Notice key={attempt} tone="error" title={shown ? 'Native conversation partly unavailable' : 'Native conversation unavailable'} dismissLabel="Dismiss native conversation error">
    <p>{unavailable.message}{shown && !unavailable.savedRetained ? ' Loaded messages are still shown.' : ''}</p>
    {details && <details className="mt-2" key={details}><summary>Technical details</summary><pre className="mt-1 whitespace-pre-wrap font-mono" style={{ color: 'var(--text-2)', overflowWrap: 'anywhere' }}>{details}</pre></details>}
  </Notice>
  return <section className="uam-managed-conversation" aria-label="Native subagent conversation">
    <SubagentHeader title={child?.name || 'Native subagent'} parentName={parent?.name} detail="Read-only" headingRef={heading} onBack={onBack} onClose={onClose}
      onRefresh={child ? () => { scopedHistory.invalidate(); setAttempt(value => value + 1) } : undefined} refreshing={loading} refreshDisabled={loading} />
    <p className="sr-only">Read-only native conversation. Follow-up messages and Stop are unavailable here.</p>
    {child && visibleMessages && visibleMessages.length > 0
      ? <SubagentTranscript messages={visibleMessages} nativeStartIndex={before} fallbackProviderId={child.providerId || ''} sessionId={chatId}
          viewportKey={viewportKey} contentVersion={messages} userMessageIndices={userMessageIndices} recoveryEpoch={attempt} label="Native conversation messages"
          loadOlder={before > 0 && !loading ? loadOlder : undefined} scopedView={scopedHistory.view} notices={failure} />
      : <div className="uam-subagent-transcript__empty">
          {failure}
          {loading ? <SubagentTranscriptLoading label="Loading native conversation…" />
            : <p role="status" className="conversation-empty">{readError?.savedRetained
              ? 'Saved history was retained, but could not be read this time. Refresh to try reading it again.'
              : !child || !messages
              ? 'This native conversation is unavailable. The provider has not supplied a readable transcript.'
              : 'No readable native messages are available.'}</p>}
        </div>}
  </section>
}
