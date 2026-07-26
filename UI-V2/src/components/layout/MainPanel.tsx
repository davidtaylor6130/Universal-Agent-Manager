import { lazy, memo, Suspense, useCallback, useEffect, useState } from 'react'
import { Columns2, Grid2X2, MessageSquare, Square, SquareTerminal } from 'lucide-react'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { ChatView } from '../views/ChatView'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { StatusDot, Tooltip } from '../ui'
import type { Session } from '../../types/session'

const CLIView = lazy(() => import('../views/CLIView').then(({ CLIView }) => ({ default: CLIView })))
import {
  chatPaneColors,
  readChatGridLayout,
  subscribeChatGridLayout,
  writeChatGridLayout,
  type ChatGridLayout,
  type ChatPaneCount,
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

const ChatPane = memo(function ChatPane({ session, active, paneIndex, multiPane, onActivate }: {
  session: Session
  active: boolean
  paneIndex: number
  multiPane: boolean
  onActivate: (paneIndex: number, sessionId?: string) => void
}) {
  const [view, setView] = useState<'chat' | 'cli'>('chat')
  const acpBinding = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const cliBinding = useAppStore((s) => s.cliBindingBySessionId[session.id])
  const folderDirectory = useAppStore((s) =>
    session.folderId ? s.folders.find((folder) => folder.id === session.folderId)?.directory ?? '' : ''
  )
  const cliSwitchLocked = Boolean(
    acpBinding?.processing ||
      acpBinding?.lifecycleState === 'waitingPermission' ||
      cliBinding?.processing ||
      cliBinding?.lifecycleState === 'busy' ||
      cliBinding?.lifecycleState === 'shuttingDown'
  )
  const paneColor = chatPaneColors[paneIndex]
  const workspaceDirectory = session.workspaceDirectory?.trim() || folderDirectory.trim()
  const workspaceLabel = workspaceDirectory.split(/[\\/]/).filter(Boolean).pop() ?? workspaceDirectory

  return (
    <div
      className="uam-chat-pane relative flex flex-col h-full overflow-hidden"
      data-testid={`chat-pane-${session.id}`}
      data-pane={paneIndex + 1}
      data-focused={active}
      data-multi-pane={multiPane}
      onMouseDown={() => { if (!active) onActivate(paneIndex, session.id) }}
      onFocusCapture={() => { if (!active) onActivate(paneIndex, session.id) }}
      style={{
        '--pane-color': paneColor,
      } as React.CSSProperties}
    >
      {/* Header bar */}
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
          {workspaceLabel && (
            <span
              data-testid={`chat-workspace-${session.id}`}
              className="uam-chat-pane__workspace truncate text-[10px] font-normal"
              style={{ color: 'var(--text-3)' }}
              title={workspaceDirectory}
            >
              {workspaceLabel}
            </span>
          )}
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
                if (view === 'cli' && isCefContext()) {
                  void sendToCEF({
                    action: 'stopCliTerminal',
                    payload: {
                      chatId: cliBinding?.boundChatId ?? session.id,
                      terminalId: cliBinding?.terminalId ?? '',
                      quit: true,
                    },
                  })
                }
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
          <Tooltip label={cliSwitchLocked && view !== 'cli' ? 'Wait for current output to finish' : 'Terminal fallback'} side="bottom">
            <button
              type="button"
              disabled={cliSwitchLocked && view !== 'cli'}
              aria-label="Terminal fallback"
              aria-pressed={view === 'cli'}
              onClick={() => {
                if (!cliSwitchLocked) setView('cli')
              }}
              className="uam-segment-button flex h-7 w-7 items-center justify-center"
              style={{
                borderRadius: 5,
                color: view === 'cli' ? 'var(--text)' : 'var(--text-2)',
                background: view === 'cli' ? 'var(--surface-high)' : 'transparent',
                opacity: cliSwitchLocked && view !== 'cli' ? 0.5 : 1,
                cursor: cliSwitchLocked && view !== 'cli' ? 'not-allowed' : 'default',
              }}
            >
              <SquareTerminal size={14} aria-hidden />
            </button>
          </Tooltip>
        </div>

      </div>

      {/* View content */}
      <div className="flex-1 overflow-hidden">
        {view === 'chat'
          ? <ChatView session={session} />
          : <Suspense fallback={<div className="flex h-full items-center justify-center text-sm" style={{ color: 'var(--text-2)' }}>Loading terminal…</div>}><CLIView session={session} /></Suspense>}
      </div>
    </div>
  )
})

const EmptyPane = memo(function EmptyPane({ active, paneIndex, multiPane, onActivate }: { active: boolean; paneIndex: number; multiPane: boolean; onActivate: (paneIndex: number, sessionId?: string) => void }) {
  const paneColor = chatPaneColors[paneIndex]
  return (
    <button
      type="button"
      className="uam-chat-pane relative flex h-full w-full items-center justify-center text-center"
      data-focused={active}
      data-multi-pane={multiPane}
      onClick={() => onActivate(paneIndex)}
      onFocus={() => { if (!active) onActivate(paneIndex) }}
      style={{ color: 'var(--text-3)', '--pane-color': paneColor } as React.CSSProperties}
    >
      <span>
        <MessageSquare size={28} style={{ opacity: 0.3, margin: '0 auto 10px' }} />
        <span className="block text-sm">Select chat</span>
      </span>
    </button>
  )
})

function LayoutButton({ count, current, onClick, children }: {
  count: ChatPaneCount
  current: ChatPaneCount
  onClick: (count: ChatPaneCount) => void
  children: React.ReactNode
}) {
  const label = `Show ${count === 1 ? 'one chat' : count === 2 ? 'two chats' : 'four chats'}`
  return (
    <Tooltip label={label} side="bottom">
      <button
        type="button"
        className="uam-layout-button flex h-7 w-8 items-center justify-center rounded"
        aria-label={label}
        aria-pressed={current === count}
        onClick={() => onClick(count)}
        style={{ color: current === count ? 'var(--accent)' : 'var(--text-2)', background: current === count ? 'var(--accent-dim)' : 'transparent' }}
      >
        {children}
      </button>
    </Tooltip>
  )
}

export function MainPanel() {
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const loadSessionMessages = useAppStore((s) => s.loadSessionMessages)
  const [layout, setLayout] = useState<ChatGridLayout>(readChatGridLayout)

  const visibleIds = layout.sessionIds.slice(0, layout.paneCount)
  const visibleSessions = visibleIds.map((id) => sessions.find((session) => session.id === id) ?? null)

  useEffect(() => writeChatGridLayout(layout), [layout])
  useEffect(() => subscribeChatGridLayout((next) => setLayout((current) => current === next ? current : next)), [])

  useEffect(() => {
    if (!activeSessionId || !sessions.some((session) => session.id === activeSessionId)) return
    setLayout((current) => {
      if (current.sessionIds[current.activePane] === activeSessionId) return current
      const existingPane = current.sessionIds.slice(0, current.paneCount).indexOf(activeSessionId)
      if (existingPane >= 0) return { ...current, activePane: existingPane }
      const sessionIds = [...current.sessionIds]
      sessionIds[current.activePane] = activeSessionId
      return { ...current, sessionIds }
    })
  }, [activeSessionId, sessions])

  useEffect(() => {
    for (const id of visibleIds) {
      if (id) loadSessionMessages(id)
    }
  }, [loadSessionMessages, visibleIds.join('|')])

  const selectPane = useCallback((index: number, sessionId?: string) => {
    setLayout((current) => ({ ...current, activePane: index }))
    if (sessionId) setActiveSession(sessionId)
  }, [setActiveSession])

  const setPaneCount = (paneCount: ChatPaneCount) => {
    setLayout((current) => {
      const activePane = Math.min(current.activePane, paneCount - 1)
      const sessionIds = [...current.sessionIds]
      while (sessionIds.length < paneCount) sessionIds.push('')
      if (activeSessionId) sessionIds[activePane] = activeSessionId
      return { ...current, paneCount, activePane, sessionIds }
    })
  }

  const pane = (index: number) => {
    const session = visibleSessions[index]
    return session
      ? <ChatPane key={session.id} session={session} active={layout.activePane === index} paneIndex={index} multiPane={layout.paneCount > 1} onActivate={selectPane} />
      : <EmptyPane active={layout.activePane === index} paneIndex={index} multiPane={layout.paneCount > 1} onActivate={selectPane} />
  }

  const verticalHandle = <PanelResizeHandle style={{ width: 1 }} />
  const horizontalHandle = <PanelResizeHandle style={{ height: 1 }} />

  return (
    <div className="flex h-full flex-col overflow-hidden">
      <div
        className="flex h-9 flex-shrink-0 items-center justify-between px-2"
        style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}
      >
        <div className="flex items-center gap-1" aria-label="Chat grid layout">
          <LayoutButton count={1} current={layout.paneCount} onClick={setPaneCount}><Square size={15} /></LayoutButton>
          <LayoutButton count={2} current={layout.paneCount} onClick={setPaneCount}><Columns2 size={15} /></LayoutButton>
          <LayoutButton count={4} current={layout.paneCount} onClick={setPaneCount}><Grid2X2 size={15} /></LayoutButton>
        </div>
        {isCefContext() && <PushStatusDot />}
      </div>

      <div className="min-h-0 flex-1" data-testid={`chat-grid-${layout.paneCount}`}>
        {layout.paneCount === 1 && pane(0)}
        {layout.paneCount === 2 && (
          <PanelGroup direction="horizontal" onLayout={(columnSizes) => setLayout((current) => ({ ...current, columnSizes }))}>
            <Panel defaultSize={layout.columnSizes[0]} minSize={20}>{pane(0)}</Panel>
            {verticalHandle}
            <Panel defaultSize={layout.columnSizes[1]} minSize={20}>{pane(1)}</Panel>
          </PanelGroup>
        )}
        {layout.paneCount === 4 && (
          <PanelGroup direction="horizontal" onLayout={(columnSizes) => setLayout((current) => ({ ...current, columnSizes }))}>
            <Panel defaultSize={layout.columnSizes[0]} minSize={20}>
              <PanelGroup direction="vertical" onLayout={(rowSizes) => setLayout((current) => ({ ...current, rowSizes }))}>
                <Panel defaultSize={layout.rowSizes[0]} minSize={20}>{pane(0)}</Panel>
                {horizontalHandle}
                <Panel defaultSize={layout.rowSizes[1]} minSize={20}>{pane(2)}</Panel>
              </PanelGroup>
            </Panel>
            {verticalHandle}
            <Panel defaultSize={layout.columnSizes[1]} minSize={20}>
              <PanelGroup direction="vertical" onLayout={(rowSizes) => setLayout((current) => ({ ...current, rowSizes }))}>
                <Panel defaultSize={layout.rowSizes[0]} minSize={20}>{pane(1)}</Panel>
                {horizontalHandle}
                <Panel defaultSize={layout.rowSizes[1]} minSize={20}>{pane(3)}</Panel>
              </PanelGroup>
            </Panel>
          </PanelGroup>
        )}
      </div>
    </div>
  )
}
