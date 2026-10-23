import { NativeConversation, SubagentPaneContent } from '../chat/SubagentPaneContent'
import { SubagentHeader } from '../chat/SubagentTranscript'
import { SubagentRouteStage, subagentStageKey } from './SubagentRouteStage'
import { activateSubagentLeaf, canExecuteNativeSubagent, hasNativeSubagentLineage, clearSubagentView, subagentRouteFor, type SubagentRoute, subagentParentFor, subscribeSubagentNavigation, subagentNavigationRevision } from '../../utils/subagentNavigation'
import { ExecutionRoutingWaitNotice } from '../modals/ExecutionRoutingDialog'
import { lazy, memo, Suspense, useCallback, useEffect, useLayoutEffect, useRef, useState, useSyncExternalStore } from 'react'
import { Columns2, MessageSquare, Plus, PowerOff, Rows2, SquareTerminal, X } from 'lucide-react'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { ChatView } from '../views/ChatView'
import { isCefContext, isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { Button, IconButton, Notice, StatusDot, Tooltip } from '../ui'
import { shortcutLabel } from '../../utils/shortcuts'
import type { Session } from '../../types/session'
import type { Message } from '../../types/message'
import { RuntimeTimeout } from '../shared/RuntimeTimeout'
import { useRuntimeActivity } from '../../hooks/useRuntimeActivity'
import { useCountdownClock } from '../../hooks/useCountdownClock'
import './MainPanel.css'

const CLIView = lazy(() => import('../views/CLIView').then(({ CLIView }) => ({ default: CLIView })))
const HIDDEN_RUNTIME_STOP = { visible: false, disabled: true, deadline: undefined, timeout: undefined, countdownStarts: undefined }
import {
  chatGridLeaves,
  chatPaneColors,
  clearChatLeaf,
  clearMissingChats,
  closeChatLeaf,
  MAX_CHAT_PANES,
  readChatGridLayout,
  resizeChatSplit,
  setChatInLeaf,
  splitChatLeaf,
  subscribeChatGridLayout,
  writeChatViewMode,
  writeChatGridLayout,
  type ChatGridLayout,
  type ChatPaneNode,
  type ChatSplitDirection,
} from '../../utils/chatGridStorage'

const PushStatusDot = memo(function PushStatusDot() {
  const pushChannelStatus = useAppStore((s) => s.pushChannelStatus)
  const pushChannelError = useAppStore((s) => s.pushChannelError)
  const lastPushAtMs = useAppStore((s) => s.lastPushAtMs)
  const uiBuildId = useAppStore((s) => s.uiBuildId)

  const tooltip =
    pushChannelStatus === 'connected'
      ? `Push connected • last update ${lastPushAtMs ? new Date(lastPushAtMs).toLocaleTimeString() : '—'} • UI ${uiBuildId}`
      : pushChannelStatus === 'no-push-yet'
        ? `Waiting for backend push channel • UI ${uiBuildId}`
        : `Push error: ${pushChannelError}`

  return (
    <Tooltip label={tooltip} side="bottom">
      <span className="mr-3 inline-flex h-7 items-center px-2" aria-label={`Backend connection: ${pushChannelStatus}`}>
        <StatusDot tone={pushChannelStatus === 'connected' ? 'success' : pushChannelStatus === 'no-push-yet' ? 'warning' : 'error'} pulse={pushChannelStatus === 'connected'} size={7} />
      </span>
    </Tooltip>
  )
})

function hasNonBlankChatContent(messages: Message[] | undefined) {
  return Boolean(messages?.some((message) =>
    Boolean(message.content.trim() || message.thoughts?.trim() || message.planSummary?.trim()) ||
    Boolean(message.blocks?.some((block) => 'text' in block && Boolean(block.text?.trim())))))
}

type ChildLineage = { orphan?: true; parentId?: string; parentCreatedAtMs?: number; parentName?: string; onBack?: () => void; onClose?: () => void }

const ChatPane = memo(function ChatPane({ session, active, leafId, paneIndex, multiPane, glide, onActivate, onClose, lineage }: {
  session: Session
  active: boolean
  leafId: string
  paneIndex: number
  multiPane: boolean
  glide: boolean
  onActivate: (leafId: string, sessionId?: string) => void
  onClose: (leafId: string, sessionId: string) => void
  /** Executable native child: the single subagent bar replaces the chat header; Chat view only, no close or view toggles. */
  lineage?: ChildLineage
}) {
  // Display-only access to this already executable, actually linked child; never a runtime grant.
  const nativeFollowupAllowed = useAppStore(state => {
    if (!lineage) return false
    const live = state.sessions.find(candidate => candidate.id === session.id)
    if (!live || live.createdAt.getTime() !== session.createdAt.getTime() || !canExecuteNativeSubagent(live)) return false
    const parent = subagentParentFor(live, state.sessions)
    const renderedParent = subagentParentFor(session, state.sessions)
    // This discriminator is supplied only by the actual unavailable-parent route, never a failed requested-parent view.
    if (lineage.orphan) return !lineage.parentId && !lineage.onBack && Boolean(lineage.onClose) &&
      hasNativeSubagentLineage(live) && hasNativeSubagentLineage(session) && canExecuteNativeSubagent(session) && !parent && !renderedParent
    return Boolean(parent && renderedParent && parent.id === lineage.parentId && renderedParent.id === parent.id &&
      parent.createdAt.getTime() === lineage.parentCreatedAtMs)
  })
  const nativeFollowupOwnerKey = JSON.stringify([leafId, session.id, session.createdAt.getTime(), lineage?.parentId, lineage?.parentCreatedAtMs, lineage?.orphan === true])
  const [nativeFollowupOwner, setNativeFollowupOwner] = useState<string | null>(null)
  const nativeFollowupOpen = nativeFollowupAllowed && nativeFollowupOwner === nativeFollowupOwnerKey
  useLayoutEffect(() => { setNativeFollowupOwner(null) }, [nativeFollowupOwnerKey, nativeFollowupAllowed])
  const view = session.importedReadOnly || lineage ? 'chat' : session.viewMode
  const setView = (requestedView: 'chat' | 'cli') => {
    const nextView = session.importedReadOnly ? 'chat' : requestedView
    useAppStore.setState((state) => ({ sessions: state.sessions.map((current) =>
      current.id === session.id && current.viewMode !== nextView ? { ...current, viewMode: nextView } : current
    ) }))
    writeChatViewMode(session.id, nextView)
  }
  const loadSessionMessages = useAppStore((s) => s.loadSessionMessages)
  const runtimeStopState = useAppStore(useShallow((s) => {
    if (view !== 'chat' || session.importedReadOnly) return HIDDEN_RUNTIME_STOP
    const acp = s.acpBindingBySessionId[session.id]
    return {
      visible: Boolean(acp?.running),
      disabled: acp?.lifecycleState !== 'ready' || Boolean(acp?.processing || acp?.pendingPermission || acp?.pendingUserInput),
      deadline: acp?.idleShutdownAtMs,
      countdownStarts: acp?.idleCountdownStartsAtMs,
      timeout: acp?.idleShutdownTimeoutSeconds,
    }
  }))
  const runtimeClockMs = useCountdownClock(runtimeStopState.visible && !runtimeStopState.disabled, runtimeStopState.countdownStarts, runtimeStopState.deadline)
  const idleSecondsLeft = runtimeStopState.deadline && runtimeStopState.countdownStarts && runtimeClockMs >= runtimeStopState.countdownStarts
    ? Math.max(0, Math.ceil((runtimeStopState.deadline - runtimeClockMs) / 1000))
    : null
  const idleFraction = idleSecondsLeft !== null && runtimeStopState.timeout
    ? Math.min(1, idleSecondsLeft / runtimeStopState.timeout)
    : null
  const recordActivity = useRuntimeActivity(session.id)
  const stopAcpSession = useAppStore((s) => s.stopAcpSession)
  const [stopRuntimeError, setStopRuntimeError] = useState('')
  useEffect(() => setStopRuntimeError(''), [session.id])
  const refreshFor = useRef({ key: nativeFollowupOwnerKey })
  if (refreshFor.current.key !== nativeFollowupOwnerKey) refreshFor.current = { key: nativeFollowupOwnerKey }
  const refreshOwner = refreshFor.current
  const [refreshState, setRefreshState] = useState({ owner: refreshOwner, pending: false, error: '' })
  // Derive flags during render so a changed owner never paints the previous owner's state.
  const refreshing = refreshState.owner === refreshOwner && refreshState.pending
  const refreshError = refreshState.owner === refreshOwner ? refreshState.error : ''
  useLayoutEffect(() => { setRefreshState({ owner: refreshOwner, pending: false, error: '' }) }, [refreshOwner])
  const cliSwitchLocked = useAppStore((s) => {
    const acpBinding = s.acpBindingBySessionId[session.id]
    const cliBinding = s.cliBindingBySessionId[session.id]
    return Boolean(
      cliBinding?.lifecycleState === 'shuttingDown' ||
      (!cliBinding?.running &&
        (acpBinding?.processing || acpBinding?.lifecycleState === 'waitingPermission'))
    )
  })
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const executionHosts = useAppStore((s) => s.executionHosts)
  const paneColor = chatPaneColors[paneIndex]
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
  const workspaceLabel = workspaceDirectory.split(/[\\/]/).filter(Boolean).pop() ?? workspaceDirectory
  const executionHost = executionHosts.find((candidate) => candidate.id === session.executionHostId)
  const executionHostLabel = session.executionHostId && session.executionHostId !== 'local'
    ? `Remote · ${executionHost?.label?.trim() || executionHost?.sshAlias?.trim() || session.executionHostId}`
    : 'Local'

  useEffect(() => {
    if (session.importedReadOnly) {
      writeChatViewMode(session.id, 'chat')
      if (session.viewMode !== 'chat') useAppStore.setState((state) => ({ sessions: state.sessions.map((current) =>
        current.id === session.id ? { ...current, viewMode: 'chat' } : current
      ) }))
    }
  }, [session.id, session.importedReadOnly, session.viewMode])

  const runtimePill = runtimeStopState.visible && (() => {
    const countdown = idleSecondsLeft !== null
      ? `${Math.floor(idleSecondsLeft / 60)}:${String(idleSecondsLeft % 60).padStart(2, '0')}`
      : ''
    const phase = runtimeStopState.disabled ? 'busy' : idleSecondsLeft !== null && idleSecondsLeft <= 60 ? 'ending' : countdown ? 'idle' : 'live'
    return (
      <Tooltip
        label={runtimeStopState.disabled
          ? 'Runtime busy · stop is available when this turn finishes'
          : countdown
            ? `Runtime idle · shuts down automatically in ${countdown}. Click to stop now.`
            : 'Runtime running · click to stop it'}
        side="bottom"
      >
        <button
          type="button"
          className="uam-runtime-pill"
          data-phase={phase}
          aria-label="Stop runtime"
          disabled={runtimeStopState.disabled}
          onClick={() => {
            setStopRuntimeError('')
            void stopAcpSession(session.id, 'timeout').then((ok) => {
              if (!ok) setStopRuntimeError('The runtime did not stop. Try again.')
            }).catch(() => setStopRuntimeError('The runtime did not stop. Try again.'))
          }}
        >
          <span className="uam-runtime-stop-icon">
            <PowerOff size={12} aria-hidden />
            {!runtimeStopState.disabled && idleFraction !== null && (
              <svg className="uam-runtime-stop-timer" viewBox="0 0 28 28" aria-hidden="true">
                <circle className="uam-runtime-stop-track" cx="14" cy="14" r="12" fill="none" strokeWidth="2.5" />
                <circle cx="14" cy="14" r="12" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round"
                  strokeDasharray={`${(2 * Math.PI * 12 * idleFraction).toFixed(2)} 75.40`} />
              </svg>
            )}
          </span>
          <span className="uam-runtime-pill__label" aria-hidden>
            <span className="uam-runtime-pill__state">{phase === 'busy' ? 'Busy' : countdown || 'Live'}</span>
            <span className="uam-runtime-pill__stop">Stop</span>
          </span>
        </button>
      </Tooltip>
    )
  })()

  return (
    <div
      className="uam-chat-pane relative flex flex-col h-full overflow-hidden"
      data-testid={`chat-pane-${session.id}`}
      onPointerDownCapture={recordActivity}
      onKeyDownCapture={recordActivity}
      onWheelCapture={recordActivity}
      onTouchMoveCapture={recordActivity}
      data-pane={paneIndex + 1}
      data-focused={active}
      data-multi-pane={multiPane}
      onMouseDown={() => { if (!active) onActivate(leafId, session.id) }}
      onFocusCapture={() => { if (!active) onActivate(leafId, session.id) }}
      style={{
        '--pane-color': paneColor,
      } as React.CSSProperties}
    >
      {lineage ? (
        <SubagentHeader title={session.name} parentName={lineage.parentName} onBack={lineage.onBack} onClose={lineage.onClose}
          refreshing={refreshing} refreshDisabled={refreshing}
          onRefresh={() => {
            // Explicit read of this child's history only; never sends input or touches the parent. Loaded messages stay
            // shown on failure; results belong only to this displayed child birth/pane/lineage visit.
            const id = session.id
            const owner = refreshOwner
            setRefreshState({ owner, pending: true, error: '' })
            const settle = (failed: boolean) => {
              if (refreshFor.current !== owner) return
              setRefreshState(current => current.owner !== owner ? current : { owner, pending: false,
                error: failed ? 'The conversation could not be refreshed. Loaded messages are still shown.' : '' })
            }
            Promise.resolve().then(() => loadSessionMessages(id, true)).then(result => settle(result === false), () => settle(true))
          }}>
          {nativeFollowupAllowed && <Button variant="ghost" size="sm" aria-expanded={nativeFollowupOpen}
            aria-label={nativeFollowupOpen ? 'Hide follow-up input' : 'Follow up'}
            onClick={event => {
              // Hide changes presentation only: no draft/resource clearing, retry, replay or runtime Stop.
              setNativeFollowupOwner(nativeFollowupOpen ? null : nativeFollowupOwnerKey)
              if (nativeFollowupOpen) event.currentTarget.focus({ preventScroll: true })
            }}>{nativeFollowupOpen ? 'Hide input' : 'Follow up'}</Button>}
          {runtimePill}
        </SubagentHeader>
      ) : (
      <div
        className="uam-chat-pane__header flex items-center gap-3 flex-shrink-0 px-4"
        style={{
          height: 44,
          borderBottom: '1px solid var(--border)',
          background: 'var(--surface)',
        }}
      >
        {/* Session name */}
        <div className="flex min-w-0 flex-1 flex-col justify-center leading-tight" title={session.name}>
          <span className="uam-chat-pane__title truncate text-sm font-semibold" style={{ color: 'var(--text)' }}>{session.name}</span>
          <span
            data-testid={`chat-workspace-${session.id}`}
            className="uam-chat-pane__workspace flex min-w-0 gap-1 text-[10px] font-normal"
            style={{ color: 'var(--text-3)' }}
            title={workspaceDirectory ? `${workspaceDirectory} · ${executionHostLabel}` : executionHostLabel}
          >
            {workspaceLabel && <span className="min-w-0 truncate">{workspaceLabel}</span>}
            <span data-testid={`chat-host-${session.id}`} className="shrink-0">{workspaceLabel ? ` · ${executionHostLabel}` : executionHostLabel}</span>
          </span>
        </div>

        <div
          className="uam-chat-pane__view-switch flex items-center p-0.5 mr-2"
          style={{
            borderRadius: 6,
            background: 'var(--surface-up)',
          }}
        >
          <Tooltip label="Chat view" side="bottom">
            <button
              type="button"
              onClick={() => {
                if (view === 'cli') void loadSessionMessages(session.id, true, true)
                setView('chat')
              }}
              aria-label="Chat view"
              aria-pressed={view === 'chat'}
              className="uam-segment-button flex h-7 w-7 items-center justify-center"
              style={{
                borderRadius: 5,
                color: view === 'chat' ? 'var(--text)' : 'var(--text-2)',
                background: view === 'chat' ? 'var(--surface-high)' : 'transparent',
              }}
            >
              <MessageSquare size={14} aria-hidden />
            </button>
          </Tooltip>
          <Tooltip label={session.importedReadOnly ? 'Imported transcripts are read-only' : cliSwitchLocked && view !== 'cli' ? 'Wait for current output to finish' : 'Terminal fallback'} side="bottom">
            <button
              type="button"
              disabled={session.importedReadOnly || (cliSwitchLocked && view !== 'cli')}
              aria-label="Terminal fallback"
              aria-pressed={view === 'cli'}
              onClick={() => {
                if (!session.importedReadOnly && !cliSwitchLocked) {
                  setView('cli')
                }
              }}
              className="uam-segment-button flex h-7 w-7 items-center justify-center"
              style={{
                borderRadius: 5,
                color: view === 'cli' ? 'var(--text)' : 'var(--text-2)',
                background: view === 'cli' ? 'var(--surface-high)' : 'transparent',
                opacity: session.importedReadOnly || cliSwitchLocked && view !== 'cli' ? 0.5 : 1,
                cursor: session.importedReadOnly || cliSwitchLocked && view !== 'cli' ? 'not-allowed' : 'default',
              }}
            >
              <SquareTerminal size={14} aria-hidden />
            </button>
          </Tooltip>
        </div>
        {view === 'cli' && <RuntimeTimeout chatId={session.id} />}
        {runtimePill}
        <IconButton
          icon={<X size={14} aria-hidden />}
          label={`Close ${session.name}`}
          tooltip="Close chat"
          tooltipSide="bottom"
          size="sm"
          onMouseDown={(event) => event.stopPropagation()}
          onClick={() => onClose(leafId, session.id)}
        />

      </div>
      )}

      <ExecutionRoutingWaitNotice session={session} />
      {refreshError && (
        <Notice tone="error" title="Refresh failed" dismissLabel="Dismiss refresh error" onDismiss={() => setRefreshState(current => current.owner === refreshOwner ? { ...current, error: '' } : current)}>
          {refreshError}
        </Notice>
      )}
      {stopRuntimeError && (
        <Notice tone="error" title="Runtime stop failed" dismissLabel="Dismiss runtime error" onDismiss={() => setStopRuntimeError('')}>
          {stopRuntimeError}
        </Notice>
      )}

      {/* View content */}
      <div key={`${session.id}:${view}`} className={`${glide && !multiPane ? 'uam-pane-glide ' : ''}flex-1 overflow-hidden`} data-pane-content={session.id} data-view={view}>
        {view === 'chat'
          ? <ChatView key={lineage ? nativeFollowupOwnerKey : undefined} session={session} navigationKey={leafId} showSubagentLineage={false} showComposer={!lineage || nativeFollowupOpen} />
          : <Suspense fallback={<div className="flex h-full items-center justify-center text-sm" style={{ color: 'var(--text-2)' }}>Loading terminal…</div>}><CLIView session={session} /></Suspense>}
      </div>
    </div>
  )
})

function SubagentPane({ parent, route, leafId, paneIndex, active, multiPane, onActivate, onBack }: {
  parent: Session; route: SubagentRoute; leafId: string; paneIndex: number; active: boolean; multiPane: boolean
  onActivate: (leafId: string, sessionId?: string) => void; onBack: () => void
}) {
  const child = useAppStore(state => route.kind === 'native' ? state.sessions.find(candidate => candidate.id === route.chatId) : undefined)
  const executable = child && canExecuteNativeSubagent(child) && subagentParentFor(child, useAppStore.getState().sessions)?.id === parent.id
  const selectedId = useAppStore(state => state.activeSessionId)
  useEffect(() => {
    if (active && executable && selectedId !== child.id) useAppStore.getState().setActiveSession(child.id, { preserveSubagentRoute: true, leafId })
  }, [active, Boolean(executable), child?.id, selectedId, leafId])
  // One bar only: the child content (or the executable child's ChatPane) owns the subagent header.
  return <div className="uam-chat-pane flex h-full min-h-0 flex-col overflow-hidden" data-testid={`subagent-pane-${parent.id}`} data-pane={paneIndex + 1} data-focused={active} data-multi-pane={multiPane}
    style={{ '--pane-color': chatPaneColors[paneIndex] } as React.CSSProperties}
    onMouseDown={() => { if (!active) onActivate(leafId) }} onFocusCapture={() => { if (!active) onActivate(leafId) }}>
    <div className="min-h-0 flex-1 overflow-hidden">{executable
      ? <ChatPane session={child} active={active} leafId={leafId} paneIndex={paneIndex} multiPane={multiPane} glide={false} onActivate={onActivate} onClose={onBack} lineage={{ parentId: parent.id, parentCreatedAtMs: parent.createdAt.getTime(), parentName: parent.name, onBack }} />
      : <SubagentPaneContent parent={parent} route={route} active={active} navigationKey={leafId} onBack={onBack} />}</div>
  </div>
}

function OrphanSubagentPane({ child, leafId, paneIndex, active, multiPane, onActivate, onClose }: { child: Session; leafId: string; paneIndex: number; active: boolean; multiPane: boolean; onActivate: () => void; onClose: () => void }) {
  return <div className="uam-chat-pane flex h-full min-h-0 flex-col overflow-hidden" data-testid={`orphan-subagent-pane-${child.id}`} data-pane={paneIndex + 1} data-focused={active} data-multi-pane={multiPane}
    style={{ '--pane-color': chatPaneColors[paneIndex] } as React.CSSProperties} onMouseDown={() => { if (!active) onActivate() }} onFocusCapture={() => { if (!active) onActivate() }}>
    <div className="min-h-0 flex-1 overflow-hidden">{canExecuteNativeSubagent(child)
      ? <ChatPane session={child} active={active} leafId={leafId} paneIndex={paneIndex} multiPane={multiPane} glide={false} onActivate={onActivate} onClose={onClose} lineage={{ orphan: true, onClose }} />
      : <NativeConversation chatId={child.id} autoFocus={active} navigationKey={leafId} onClose={onClose} />}</div>
  </div>
}

const EmptyPane = memo(function EmptyPane({ active, leafId, paneIndex, multiPane, onActivate, onClose }: {
  active: boolean
  leafId: string
  paneIndex: number
  multiPane: boolean
  onActivate: (leafId: string, sessionId?: string) => void
  onClose: (leafId: string) => void
}) {
  const paneColor = chatPaneColors[paneIndex]
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  return (
    <div
      className="uam-chat-pane relative flex h-full w-full items-center justify-center text-center"
      data-testid="empty-pane"
      data-focused={active}
      data-multi-pane={multiPane}
      onMouseDown={() => { if (!active) onActivate(leafId) }}
      onFocusCapture={() => { if (!active) onActivate(leafId) }}
      style={{ color: 'var(--text-3)', '--pane-color': paneColor } as React.CSSProperties}
    >
      {multiPane && <div className="absolute right-2 top-2"><IconButton size="sm" icon={<X size={14} />} label="Close view" onClick={() => onClose(leafId)} /></div>}
      <div className="flex max-w-xs flex-col items-center gap-4 px-4">
        <button type="button" className="flex cursor-pointer flex-col items-center gap-2 rounded-lg p-2 text-sm hover:text-[var(--text-2)] focus-visible:outline" onClick={() => onActivate(leafId)}>
          <MessageSquare size={28} aria-hidden style={{ opacity: 0.35 }} />
          Drag a chat here or select one
        </button>
        <Button variant="primary" size="sm" onClick={() => {
          onActivate(leafId)
          useAppStore.getState().setNewChatModalOpen(true)
        }}>New Chat</Button>
      </div>
    </div>
  )
})

export function MainPanel() {
  const subagentNavigationVersion = useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const previousActiveSessionId = useRef<string | null>(activeSessionId)
  const [glideSessionId, setGlideSessionId] = useState<string | null>(null)
  const lastAppliedStateRevision = useAppStore((s) => s.lastAppliedStateRevision)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const loadSessionMessages = useAppStore((s) => s.loadSessionMessages)
  const unloadSessionMessages = useAppStore((s) => s.unloadSessionMessages)
  const [layout, setLayout] = useState<ChatGridLayout>(readChatGridLayout)
  const [returnFocusLeaf, setReturnFocusLeaf] = useState('')
  const panelRef = useRef<HTMLDivElement>(null)
  const [dropTargetPane, setDropTargetPane] = useState<string | null>(null)
  const leaves = chatGridLeaves(layout.root)
  const visibleIds = [...new Set(leaves.flatMap(leaf => {
    const parent = sessions.find(session => session.id === leaf.sessionId)
    const route = parent && subagentRouteFor(leaf.id, parent)
    return route?.kind === 'native' && sessions.some(session => session.id === route.chatId) ? [leaf.sessionId, route.chatId] : [leaf.sessionId]
  }))]
  useLayoutEffect(() => {
    const selected = sessions.find(session => session.id === activeSessionId)
    const parent = selected && subagentParentFor(selected, sessions)
    if (selected && parent) {
      const activeLeaf = chatGridLeaves(layout.root).find(leaf => leaf.id === layout.activeLeafId)
      // A selected native child can itself own the displayed passive/managed route.
      // Retain that immediate-parent view instead of reopening the selected child's parent.
      if (activeLeaf?.sessionId === selected.id && subagentRouteFor(activeLeaf.id, selected)) return
      const route = activeLeaf && subagentRouteFor(activeLeaf.id, parent)
      if (route?.kind === 'native' && route.chatId === selected.id && canExecuteNativeSubagent(selected)) return
      // A restored backend selection can bypass the explicit sidebar action.
      setActiveSession(selected.id)
      setLayout(readChatGridLayout())
    }
  }, [activeSessionId, sessions, subagentNavigationVersion, setActiveSession])
  useLayoutEffect(() => {
    const previousId = previousActiveSessionId.current
    const state = useAppStore.getState()
    const activeSession = state.sessions.find((session) => session.id === activeSessionId)
    const previousSession = state.sessions.find((session) => session.id === previousId)
    setGlideSessionId(
      activeSessionId && previousId && activeSessionId !== previousId &&
      (activeSession?.importedReadOnly || activeSession?.viewMode === 'chat') &&
      (previousSession?.importedReadOnly || previousSession?.viewMode === 'chat') &&
      hasNonBlankChatContent(state.messages[activeSessionId]) &&
      hasNonBlankChatContent(state.messages[previousId]) &&
      // Parent/child moves belong to the route stage's spring, not the session glide.
      !(activeSession && previousSession && (subagentParentFor(activeSession, state.sessions)?.id === previousId || subagentParentFor(previousSession, state.sessions)?.id === activeSessionId))
        ? activeSessionId
        : null,
    )
    previousActiveSessionId.current = activeSessionId
  }, [activeSessionId])
  const visibleAcpSignatures = useAppStore(useShallow((s) => visibleIds.map((id) => {
    const binding = id ? s.acpBindingBySessionId[id] : undefined
    return `${binding?.processing ? 1 : 0}:${binding?.turnSerial ?? 0}`
  })))
  const previousVisibleAcp = useRef(new Map<string, { processing: boolean; turnSerial: number }>())
  const previousVisibleIds = useRef(new Set(visibleIds.filter(Boolean)))
  const pendingEvictionIds = useRef(new Set<string>())
  const pendingNativeReleaseIds = useRef(new Set<string>())

  useLayoutEffect(() => {
    if (!returnFocusLeaf) return
    const target = [...(panelRef.current?.querySelectorAll<HTMLElement>('[data-leaf-id]') || [])].find(element => element.dataset.leafId === returnFocusLeaf)
    target?.querySelector<HTMLTextAreaElement>('textarea')?.focus({ preventScroll: true })
    setReturnFocusLeaf('')
  }, [returnFocusLeaf])
  useEffect(() => writeChatGridLayout(layout), [layout])
  useEffect(() => subscribeChatGridLayout((next) => setLayout((current) => current === next ? current : next)), [])
  useEffect(() => useAppStore.subscribe((state) => {
    for (const id of pendingEvictionIds.current) {
      if (state.activeSessionId === id || previousVisibleIds.current.has(id)) { pendingEvictionIds.current.delete(id); continue }
      if (state.acpBindingBySessionId[id]?.processing || state.cliBindingBySessionId[id]?.processing) continue
      pendingEvictionIds.current.delete(id)
      state.unloadSessionMessages(id)
    }
    for (const id of pendingNativeReleaseIds.current) {
      if (state.activeSessionId === id || previousVisibleIds.current.has(id)) { pendingNativeReleaseIds.current.delete(id); continue }
      const acp = state.acpBindingBySessionId[id]
      const cli = state.cliBindingBySessionId[id]
      if (acp && (acp.running || acp.processing || !['stopped', 'failed', 'error'].includes(acp.lifecycleState))) continue
      if (cli && (cli.running || cli.processing || !['stopped', 'failed', 'error'].includes(cli.lifecycleState))) continue
      pendingNativeReleaseIds.current.delete(id)
      if (isCefContext() && !isCompanionContext()) void sendToCEF({ action: 'releaseChatMessages', payload: { chatId: id } })
    }
  }), [])

  useEffect(() => {
    if (isCefContext() && lastAppliedStateRevision < 0) return
    setLayout((current) => clearMissingChats(current, sessions.map((session) => session.id)))
  }, [lastAppliedStateRevision, sessions])

  useEffect(() => {
    if (!activeSessionId) return
    const sessionsById = new Map(sessions.map((session) => [session.id, session]))
    const selectedSession = sessionsById.get(activeSessionId)
    if (!selectedSession || subagentParentFor(selectedSession, sessions)) return
    const familyKey = (candidate: Session) => hasNativeSubagentLineage(candidate) ? candidate.id : candidate.branchRootChatId || candidate.parentChatId || candidate.id
    const selectedBranchRoot = familyKey(selectedSession)
    setLayout((current) => {
      const currentLeaves = chatGridLeaves(current.root)
      const activeLeaf = currentLeaves.find((leaf) => leaf.id === current.activeLeafId)
      const activeParent = sessionsById.get(activeLeaf?.sessionId || '')
      if (activeLeaf && activeParent && subagentRouteFor(activeLeaf.id, activeParent) && activeParent.id === activeSessionId) return current
      const branchLeaves = currentLeaves.filter((leaf) => {
        const session = sessionsById.get(leaf.sessionId)
        return session && familyKey(session) === selectedBranchRoot && (!subagentRouteFor(leaf.id, session) || leaf.id === current.activeLeafId)
      })
      const existingPane = branchLeaves.find((leaf) => leaf.id === current.activeLeafId && leaf.sessionId === activeSessionId) ?? branchLeaves.find((leaf) => leaf.sessionId === activeSessionId)
      const branchTarget = existingPane ?? branchLeaves.find((leaf) => leaf.id === current.activeLeafId) ?? branchLeaves[0]
      if (branchTarget) {
        if (branchLeaves.length === 1 && existingPane && current.activeLeafId === branchTarget.id) return current
        let next = existingPane
          ? { ...current, activeLeafId: branchTarget.id }
          : setChatInLeaf(current, activeSessionId, branchTarget.id)
        for (const leaf of branchLeaves) {
          if (leaf.id !== branchTarget.id) next = clearChatLeaf(next, leaf.id)
        }
        return next.activeLeafId === branchTarget.id ? next : { ...next, activeLeafId: branchTarget.id }
      }
      const targetLeaf = activeLeaf && !activeLeaf.sessionId
        ? activeLeaf
        : currentLeaves.find((leaf) => !leaf.sessionId) ?? activeLeaf ?? currentLeaves[0]
      return targetLeaf ? setChatInLeaf(current, activeSessionId, targetLeaf.id) : current
    })
  }, [activeSessionId, sessions, subagentNavigationVersion])

  useEffect(() => {
    if (isCefContext() && !isCompanionContext()) {
      void sendToCEF({ action: 'setVisibleChatIds', payload: { chatIds: visibleIds.filter((id): id is string => Boolean(id)) } })
    }
  }, [visibleIds.join('|')])

  useEffect(() => {
    for (const id of visibleIds) {
      if (id && useAppStore.getState().chatSearchNavigation?.chatId !== id) loadSessionMessages(id)
    }
  }, [loadSessionMessages, visibleIds.join('|')])

  useEffect(() => {
    const current = new Set(visibleIds.filter(Boolean))
    const previous = previousVisibleIds.current
    previousVisibleIds.current = current
    for (const id of previous) {
      if (current.has(id)) continue
      const state = useAppStore.getState()
      if (isCefContext() && !isCompanionContext() && (state.acpBindingBySessionId[id]?.running || state.cliBindingBySessionId[id]?.running || state.acpBindingBySessionId[id]?.processing || state.cliBindingBySessionId[id]?.processing)) {
        pendingNativeReleaseIds.current.add(id)
      }
      if (state.acpBindingBySessionId[id]?.processing || state.cliBindingBySessionId[id]?.processing) {
        pendingEvictionIds.current.add(id)
      } else {
        unloadSessionMessages(id)
      }
    }
    for (const id of current) { pendingEvictionIds.current.delete(id); pendingNativeReleaseIds.current.delete(id) }
  }, [unloadSessionMessages, visibleIds.join('|')])

  useEffect(() => {
    const previous = previousVisibleAcp.current
    const current = new Map<string, { processing: boolean; turnSerial: number }>()
    for (const id of visibleIds) {
      if (!id) continue
      const binding = useAppStore.getState().acpBindingBySessionId[id]
      const snapshot = {
        processing: Boolean(binding?.processing),
        turnSerial: binding?.turnSerial ?? 0,
      }
      const prior = previous.get(id)
      if (id !== activeSessionId && prior?.processing && useAppStore.getState().chatSearchNavigation?.chatId !== id) {
        if (!snapshot.processing) {
          loadSessionMessages(id, true)
        } else if (snapshot.turnSerial > prior.turnSerial) {
          loadSessionMessages(id)
        }
      }
      current.set(id, snapshot)
    }
    previousVisibleAcp.current = current
  }, [activeSessionId, loadSessionMessages, visibleIds.join('|'), visibleAcpSignatures.join('|')])

  const selectPane = useCallback((leafId: string, sessionId?: string) => {
    setLayout((current) => ({ ...current, activeLeafId: leafId }))
    setActiveSession(sessionId || null)
  }, [setActiveSession])

  const splitActivePane = (direction: ChatSplitDirection) => setLayout((current) => {
    const next = splitChatLeaf(current, current.activeLeafId, direction)
    if (next !== current) setActiveSession(null)
    return next
  })

  const clearChat = useCallback((leafId: string, sessionId: string) => {
    clearSubagentView(leafId)
    setLayout((current) => clearChatLeaf(current, leafId))
    if (useAppStore.getState().activeSessionId === sessionId) setActiveSession(null)
  }, [setActiveSession])

  const closePane = useCallback((leafId: string) => {
    clearSubagentView(leafId)
    setLayout((current) => {
      const next = closeChatLeaf(current, leafId)
      setActiveSession(chatGridLeaves(next.root).find((leaf) => leaf.id === next.activeLeafId)?.sessionId || null)
      return next
    })
  }, [setActiveSession])

  const pane = (leafId: string, index: number, sessionId: string) => {
    const session = sessions.find((candidate) => candidate.id === sessionId)
    const route = session && subagentRouteFor(leafId, session)
    // An orphan can still own a valid child route; its unavailable-parent view is the return destination.
    if (session && !route && hasNativeSubagentLineage(session) && !subagentParentFor(session, sessions)) return <OrphanSubagentPane key={`orphan:${session.id}`} child={session} leafId={leafId} paneIndex={index} active={layout.activeLeafId === leafId} multiPane={leaves.length > 1} onActivate={() => selectPane(leafId, session.id)} onClose={() => clearChat(leafId, session.id)} />
    if (session && route) return <SubagentPane key={`child:${session.id}`} parent={session} route={route} leafId={leafId} paneIndex={index} active={layout.activeLeafId === leafId} multiPane={leaves.length > 1} onActivate={() => {
      activateSubagentLeaf(leafId)
      const child = route.kind === 'native' ? sessions.find(candidate => candidate.id === route.chatId) : undefined
      setActiveSession(child && canExecuteNativeSubagent(child) ? child.id : session.id, { preserveSubagentRoute: true, leafId })
    }} onBack={() => {
      clearSubagentView(leafId)
      setReturnFocusLeaf(leafId)
      setLayout(current => ({ ...current, activeLeafId: leafId }))
      setActiveSession(session.id, { preserveSubagentRoute: true, leafId })
    }} />
    return session
      ? <ChatPane key={session.id} session={session} active={layout.activeLeafId === leafId} leafId={leafId} paneIndex={index} multiPane={leaves.length > 1} glide={glideSessionId === session.id} onActivate={selectPane} onClose={clearChat} />
      : <EmptyPane active={layout.activeLeafId === leafId} leafId={leafId} paneIndex={index} multiPane={leaves.length > 1} onActivate={selectPane} onClose={closePane} />
  }

  const stageKey = (leafId: string, sessionId: string) => {
    const session = sessions.find((candidate) => candidate.id === sessionId)
    const route = session && subagentRouteFor(leafId, session)
    return route ? subagentStageKey(sessionId, route.kind === 'native' ? route.chatId : route.runId) : sessionId
  }

  const paneDropTarget = (leafId: string, index: number, sessionId: string) => (
    <div
      className="h-full"
      data-testid={`pane-drop-target-${index + 1}`}
      data-leaf-id={leafId}
      data-drop-target={dropTargetPane === leafId}
      style={{ outline: dropTargetPane === leafId ? `2px solid ${chatPaneColors[index]}` : 'none', outlineOffset: -2 }}
      onDragOver={(event) => {
        if (!Array.from(event.dataTransfer.types).includes('text/x-uam-chat-id')) return
        event.preventDefault()
        event.dataTransfer.dropEffect = 'copy'
        setDropTargetPane(leafId)
      }}
      onDragLeave={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setDropTargetPane(null)
      }}
      onDrop={(event) => {
        if (!Array.from(event.dataTransfer.types).includes('text/x-uam-chat-id')) return
        event.preventDefault()
        const sessionId = event.dataTransfer.getData('text/x-uam-chat-id').trim()
        setDropTargetPane(null)
        if (!sessions.some((session) => session.id === sessionId)) return
        setLayout((current) => setChatInLeaf(current, sessionId, leafId))
        setActiveSession(sessionId)
      }}
    >
      <SubagentRouteStage routeKey={stageKey(leafId, sessionId)}>{pane(leafId, index, sessionId)}</SubagentRouteStage>
    </div>
  )

  const renderNode = (node: ChatPaneNode): React.ReactNode => {
    if (node.type === 'leaf') {
      const index = leaves.findIndex((leaf) => leaf.id === node.id)
      return paneDropTarget(node.id, index, node.sessionId)
    }
    const columns = node.direction === 'horizontal'
    return (
      <PanelGroup key={node.id} direction={node.direction} onLayout={(sizes) => setLayout((current) => resizeChatSplit(current, node.id, sizes))}>
        <Panel defaultSize={node.sizes[0]} minSize={12}>{renderNode(node.children[0])}</Panel>
        <PanelResizeHandle
          className="uam-pane-resize-handle"
          style={columns ? { width: 3, cursor: 'col-resize' } : { height: 3, cursor: 'row-resize' }}
          aria-label={`Resize ${columns ? 'columns' : 'rows'}`}
          tabIndex={0}
        />
        <Panel defaultSize={node.sizes[1]} minSize={12}>{renderNode(node.children[1])}</Panel>
      </PanelGroup>
    )
  }

  const paneLimitReached = leaves.length >= MAX_CHAT_PANES
  const activeLeaf = leaves.find((leaf) => leaf.id === layout.activeLeafId) ?? leaves[0]
  const splitButton = (direction: ChatSplitDirection, icon: React.ReactNode) => {
    const label = `Split active pane ${direction === 'horizontal' ? 'into columns' : 'into rows'}`
    return (
      <Tooltip label={paneLimitReached ? `Maximum ${MAX_CHAT_PANES} panes` : label} side="bottom">
        <button
          type="button"
          className="uam-layout-button flex h-7 w-8 items-center justify-center rounded"
          aria-label={label}
          disabled={paneLimitReached}
          onClick={() => splitActivePane(direction)}
          style={{ color: 'var(--text-2)', background: 'transparent' }}
        >
          {icon}
        </button>
      </Tooltip>
    )
  }

  return (
    <div ref={panelRef} className="flex h-full flex-col overflow-hidden">
      <div
        className="uam-layout-toolbar flex h-9 flex-shrink-0 items-center justify-between px-2"
        style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}
      >
        <div className="flex items-center gap-1" aria-label={`Chat pane layout, ${leaves.length} of ${MAX_CHAT_PANES} panes`}>
          {splitButton('horizontal', <Columns2 size={15} aria-hidden />)}
          {splitButton('vertical', <Rows2 size={15} aria-hidden />)}
          {leaves.length > 1 && (
            <Tooltip label="Close active pane" side="bottom">
              <button
                type="button"
                className="uam-layout-button flex h-7 w-8 items-center justify-center rounded"
                aria-label="Close active pane"
                onClick={() => activeLeaf && closePane(activeLeaf.id)}
                style={{ color: 'var(--text-2)', background: 'transparent' }}
              >
                <X size={15} aria-hidden />
              </button>
            </Tooltip>
          )}
        </div>
        {isCefContext() && <PushStatusDot />}
      </div>

      <div className="min-h-0 flex-1" data-testid={`chat-grid-${leaves.length}`}>
        {renderNode(layout.root)}
      </div>
    </div>
  )
}
