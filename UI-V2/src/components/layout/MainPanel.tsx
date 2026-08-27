import { lazy, memo, Suspense, useCallback, useEffect, useRef, useState } from 'react'
import { Columns2, MessageSquare, Rows2, SquareTerminal, X } from 'lucide-react'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { ChatView } from '../views/ChatView'
import { isCefContext } from '../../ipc/cefBridge'
import { IconButton, StatusDot, Tooltip } from '../ui'
import type { Session } from '../../types/session'

const CLIView = lazy(() => import('../views/CLIView').then(({ CLIView }) => ({ default: CLIView })))
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

const ChatPane = memo(function ChatPane({ session, active, leafId, paneIndex, multiPane, onActivate, onClose }: {
  session: Session
  active: boolean
  leafId: string
  paneIndex: number
  multiPane: boolean
  onActivate: (leafId: string, sessionId?: string) => void
  onClose: (leafId: string, sessionId: string) => void
}) {
  const [view, setView] = useState<'chat' | 'cli'>(session.importedReadOnly ? 'chat' : session.viewMode)
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

  useEffect(() => {
    if (session.importedReadOnly) writeChatViewMode(session.id, 'chat')
    setView(session.importedReadOnly ? 'chat' : session.viewMode)
  }, [session.id, session.importedReadOnly, session.viewMode])

  return (
    <div
      className="uam-chat-pane relative flex flex-col h-full overflow-hidden"
      data-testid={`chat-pane-${session.id}`}
      data-pane={paneIndex + 1}
      data-focused={active}
      data-multi-pane={multiPane}
      onMouseDown={() => { if (!active) onActivate(leafId, session.id) }}
      onFocusCapture={() => { if (!active) onActivate(leafId, session.id) }}
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
                writeChatViewMode(session.id, 'chat')
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
                  writeChatViewMode(session.id, 'cli')
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

      {/* View content */}
      <div className="flex-1 overflow-hidden">
        {view === 'chat'
          ? <ChatView session={session} onOpenTerminalFallback={() => {
              if (session.importedReadOnly || cliSwitchLocked) return
              writeChatViewMode(session.id, 'cli')
              setView('cli')
            }} />
          : <Suspense fallback={<div className="flex h-full items-center justify-center text-sm" style={{ color: 'var(--text-2)' }}>Loading terminal…</div>}><CLIView session={session} /></Suspense>}
      </div>
    </div>
  )
})

const EmptyPane = memo(function EmptyPane({ active, leafId, paneIndex, multiPane, onActivate }: { active: boolean; leafId: string; paneIndex: number; multiPane: boolean; onActivate: (leafId: string, sessionId?: string) => void }) {
  const paneColor = chatPaneColors[paneIndex]
  return (
    <button
      type="button"
      className="uam-chat-pane relative flex h-full w-full items-center justify-center text-center"
      data-focused={active}
      data-multi-pane={multiPane}
      onClick={() => onActivate(leafId)}
      onFocus={() => { if (!active) onActivate(leafId) }}
      style={{ color: 'var(--text-3)', '--pane-color': paneColor } as React.CSSProperties}
    >
      <span>
        <MessageSquare size={28} style={{ opacity: 0.3, margin: '0 auto 10px' }} />
        <span className="block text-sm">Drag a chat here or select one</span>
      </span>
    </button>
  )
})

export function MainPanel() {
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const lastAppliedStateRevision = useAppStore((s) => s.lastAppliedStateRevision)
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const loadSessionMessages = useAppStore((s) => s.loadSessionMessages)
  const [layout, setLayout] = useState<ChatGridLayout>(readChatGridLayout)
  const [dropTargetPane, setDropTargetPane] = useState<string | null>(null)
  const leaves = chatGridLeaves(layout.root)
  const visibleIds = leaves.map((leaf) => leaf.sessionId)
  const visibleAcpSignatures = useAppStore(useShallow((s) => visibleIds.map((id) => {
    const binding = id ? s.acpBindingBySessionId[id] : undefined
    return `${binding?.processing ? 1 : 0}:${binding?.turnSerial ?? 0}`
  })))
  const previousVisibleAcp = useRef(new Map<string, { processing: boolean; turnSerial: number }>())

  useEffect(() => writeChatGridLayout(layout), [layout])
  useEffect(() => subscribeChatGridLayout((next) => setLayout((current) => current === next ? current : next)), [])

  useEffect(() => {
    if (isCefContext() && lastAppliedStateRevision < 0) return
    setLayout((current) => clearMissingChats(current, sessions.map((session) => session.id)))
  }, [lastAppliedStateRevision, sessions])

  useEffect(() => {
    if (!activeSessionId || !sessions.some((session) => session.id === activeSessionId)) return
    setLayout((current) => {
      const currentLeaves = chatGridLeaves(current.root)
      if (currentLeaves.find((leaf) => leaf.id === current.activeLeafId)?.sessionId === activeSessionId) return current
      const existingPane = currentLeaves.find((leaf) => leaf.sessionId === activeSessionId)
      if (existingPane) return { ...current, activeLeafId: existingPane.id }
      return setChatInLeaf(current, activeSessionId, current.activeLeafId)
    })
  }, [activeSessionId, sessions])

  useEffect(() => {
    for (const id of visibleIds) {
      if (id) loadSessionMessages(id)
    }
  }, [loadSessionMessages, visibleIds.join('|')])

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
      if (id !== activeSessionId && prior?.processing) {
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
    setLayout((current) => clearChatLeaf(current, leafId))
    if (activeSessionId === sessionId) setActiveSession(null)
  }, [activeSessionId, setActiveSession])

  const closePane = useCallback((leafId: string) => {
    setLayout((current) => {
      const next = closeChatLeaf(current, leafId)
      setActiveSession(chatGridLeaves(next.root).find((leaf) => leaf.id === next.activeLeafId)?.sessionId || null)
      return next
    })
  }, [setActiveSession])

  const pane = (leafId: string, index: number, sessionId: string) => {
    const session = sessions.find((candidate) => candidate.id === sessionId)
    return session
      ? <ChatPane key={session.id} session={session} active={layout.activeLeafId === leafId} leafId={leafId} paneIndex={index} multiPane={leaves.length > 1} onActivate={selectPane} onClose={clearChat} />
      : <EmptyPane active={layout.activeLeafId === leafId} leafId={leafId} paneIndex={index} multiPane={leaves.length > 1} onActivate={selectPane} />
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
      {pane(leafId, index, sessionId)}
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
          style={{ color: 'var(--text-2)', background: 'transparent', opacity: paneLimitReached ? 0.45 : 1 }}
        >
          {icon}
        </button>
      </Tooltip>
    )
  }

  return (
    <div className="flex h-full flex-col overflow-hidden">
      <div
        className="flex h-9 flex-shrink-0 items-center justify-between px-2"
        style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}
      >
        <div className="flex items-center gap-1" aria-label={`Chat pane layout, ${leaves.length} of ${MAX_CHAT_PANES} panes`}>
          {splitButton('horizontal', <Columns2 size={15} aria-hidden />)}
          {splitButton('vertical', <Rows2 size={15} aria-hidden />)}
          <Tooltip label={leaves.length > 1 ? 'Close active pane' : 'Clear active pane'} side="bottom">
            <button
              type="button"
              className="uam-layout-button flex h-7 w-8 items-center justify-center rounded"
              aria-label={leaves.length > 1 ? 'Close active pane' : 'Clear active pane'}
              disabled={!activeLeaf?.sessionId && leaves.length === 1}
              onClick={() => activeLeaf && closePane(activeLeaf.id)}
              style={{ color: 'var(--text-2)', background: 'transparent', opacity: !activeLeaf?.sessionId && leaves.length === 1 ? 0.45 : 1 }}
            >
              <X size={15} aria-hidden />
            </button>
          </Tooltip>
        </div>
        {isCefContext() && <PushStatusDot />}
      </div>

      <div className="min-h-0 flex-1" data-testid={`chat-grid-${leaves.length}`}>
        {renderNode(layout.root)}
      </div>
    </div>
  )
}
