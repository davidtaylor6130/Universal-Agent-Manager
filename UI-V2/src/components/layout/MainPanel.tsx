import { memo, useEffect, useState } from 'react'
import { Columns2, Grid2X2, MessageSquare, ChevronDown, Square } from 'lucide-react'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { CLIView } from '../views/CLIView'
import { ChatView } from '../views/ChatView'
import { isCefContext } from '../../ipc/cefBridge'
import { ProviderLogo } from '../shared/ProviderLogo'
import { DEFAULT_PROVIDER_ID, providerShortName } from '../../utils/providerMetadata'
import { Tooltip } from '../ui'
import type { Session } from '../../types/session'
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

  const color =
    pushChannelStatus === 'connected' ? '#22c55e'
    : pushChannelStatus === 'no-push-yet' ? '#eab308'
    : '#ef4444'

  const timeLabel = lastPushAtMs
    ? new Date(lastPushAtMs).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
    : ''

  const tooltip =
    pushChannelStatus === 'connected'
      ? `Push connected • last update ${lastPushAtMs ? new Date(lastPushAtMs).toLocaleTimeString() : '—'} • UI ${uiBuildId}`
      : pushChannelStatus === 'no-push-yet'
        ? `Waiting for backend push channel • UI ${uiBuildId}`
        : `Push error: ${pushChannelError}`

  return (
    <div
      className="flex items-center gap-1 flex-shrink-0 mr-3"
      style={{ color, fontSize: 11, opacity: 0.9 }}
      title={tooltip}
    >
      <span style={{ fontSize: 7, lineHeight: 1 }}>●</span>
      {timeLabel && <span style={{ opacity: 0.8 }}>{timeLabel}</span>}
    </div>
  )
})

function ChatPane({ session, active, paneIndex, onActivate }: {
  session: Session
  active: boolean
  paneIndex: number
  onActivate: () => void
}) {
  const [view, setView] = useState<'chat' | 'cli'>('chat')
  const providers = useAppStore(useShallow((s) => s.providers))
  const acpBinding = useAppStore((s) => s.acpBindingBySessionId[session.id])
  const cliBinding = useAppStore((s) => s.cliBindingBySessionId[session.id])
  const viewSwitchLocked = Boolean(
    acpBinding?.processing ||
      acpBinding?.lifecycleState === 'waitingPermission' ||
      cliBinding?.processing ||
      cliBinding?.lifecycleState === 'busy' ||
      cliBinding?.lifecycleState === 'shuttingDown'
  )
  const providerId = session?.providerId || acpBinding?.providerId || DEFAULT_PROVIDER_ID
  const provider = providers.find((candidate) => candidate.id === providerId)
  const providerName = providerShortName(provider, providerId)
  const paneColor = chatPaneColors[paneIndex]

  return (
    <div
      className="flex flex-col h-full overflow-hidden"
      data-testid={`chat-pane-${session.id}`}
      data-pane={paneIndex + 1}
      data-focused={active}
      onMouseDown={() => { if (!active) onActivate() }}
      style={{
        border: `1px solid ${paneColor}`,
        boxShadow: `inset 0 0 ${active ? 4 : 3}px color-mix(in srgb, ${paneColor} ${active ? 18 : 8}%, transparent)`,
        filter: active ? 'none' : 'brightness(0.82) saturate(0.72)',
        transition: 'filter 140ms ease, box-shadow 140ms ease, border-color 140ms ease',
        '--accent': paneColor,
        '--accent-dim': `color-mix(in srgb, ${paneColor} 12%, transparent)`,
        '--accent-glow': `color-mix(in srgb, ${paneColor} 20%, transparent)`,
      } as React.CSSProperties}
    >
      {/* Header bar */}
      <div
        className="flex items-center gap-3 flex-shrink-0 px-4"
        style={{
          height: 48,
          borderBottom: '1px solid var(--border)',
          background: 'var(--surface)',
        }}
      >
        {/* Session name */}
        <div
          className="flex items-center gap-3 flex-1 min-w-0 text-sm font-semibold truncate"
          style={{ color: 'var(--text)' }}
          title={session.name}
        >
          <span className="truncate">{session.name}</span>
          <span
            className="inline-flex items-center gap-2 text-xs font-medium"
            style={{
              height: 30,
              border: '1px solid color-mix(in srgb, var(--teal) 28%, var(--border))',
              borderRadius: 7,
              background: 'var(--teal-dim)',
              color: 'var(--teal)',
              padding: '0 10px',
              flexShrink: 0,
            }}
          >
            <ProviderLogo providerId={providerId} />
            <span>{providerName} Provider</span>
            <ChevronDown size={13} aria-hidden />
          </span>
        </div>

        <div
          className="flex items-center p-0.5 mr-2"
          style={{
            border: '1px solid var(--border)',
            borderRadius: 6,
            background: 'var(--bg)',
          }}
        >
          <Tooltip label={viewSwitchLocked && view !== 'chat' ? 'Wait for current output to finish' : 'Chat view'} side="bottom">
            <button
              type="button"
              disabled={viewSwitchLocked && view !== 'chat'}
              onClick={() => {
                if (!viewSwitchLocked) setView('chat')
              }}
              className="h-7 px-3 text-xs"
              style={{
                borderRadius: 5,
                color: view === 'chat' ? 'var(--text)' : 'var(--text-2)',
                background: view === 'chat' ? 'var(--surface-up)' : 'transparent',
                opacity: viewSwitchLocked && view !== 'chat' ? 0.5 : 1,
                cursor: viewSwitchLocked && view !== 'chat' ? 'not-allowed' : 'default',
              }}
            >
              Chat
            </button>
          </Tooltip>
          <Tooltip label={viewSwitchLocked && view !== 'cli' ? 'Wait for current output to finish' : 'Terminal fallback'} side="bottom">
            <button
              type="button"
              disabled={viewSwitchLocked && view !== 'cli'}
              onClick={() => {
                if (!viewSwitchLocked) setView('cli')
              }}
              className="h-7 px-3 text-xs"
              style={{
                borderRadius: 5,
                color: view === 'cli' ? 'var(--text)' : 'var(--text-2)',
                background: view === 'cli' ? 'var(--surface-up)' : 'transparent',
                opacity: viewSwitchLocked && view !== 'cli' ? 0.5 : 1,
                cursor: viewSwitchLocked && view !== 'cli' ? 'not-allowed' : 'default',
              }}
            >
              CLI
            </button>
          </Tooltip>
        </div>

      </div>

      {/* View content */}
      <div className="flex-1 overflow-hidden">
        {view === 'chat' ? <ChatView session={session} accentColor={paneColor} /> : <CLIView session={session} />}
      </div>
    </div>
  )
}

function EmptyPane({ active, paneIndex, onActivate }: { active: boolean; paneIndex: number; onActivate: () => void }) {
  const paneColor = chatPaneColors[paneIndex]
  return (
    <button
      type="button"
      className="flex h-full w-full items-center justify-center text-center"
      onClick={onActivate}
      style={{ color: 'var(--text-3)', border: `1px solid ${paneColor}`, boxShadow: `inset 0 0 ${active ? 4 : 3}px color-mix(in srgb, ${paneColor} ${active ? 18 : 8}%, transparent)`, filter: active ? 'none' : 'brightness(0.82) saturate(0.72)', transition: 'filter 140ms ease, box-shadow 140ms ease, border-color 140ms ease' }}
    >
      <span>
        <MessageSquare size={28} style={{ opacity: 0.3, margin: '0 auto 10px' }} />
        <span className="block text-sm">Select Chat</span>
      </span>
    </button>
  )
}

function LayoutButton({ count, current, onClick, children }: {
  count: ChatPaneCount
  current: ChatPaneCount
  onClick: (count: ChatPaneCount) => void
  children: React.ReactNode
}) {
  return (
    <button
      type="button"
      className="flex h-7 w-8 items-center justify-center rounded"
      aria-label={`Show ${count === 1 ? 'one chat' : count === 2 ? 'two chats' : 'four chats'}`}
      aria-pressed={current === count}
      onClick={() => onClick(count)}
      style={{ color: current === count ? 'var(--accent)' : 'var(--text-2)', background: current === count ? 'var(--accent-dim)' : 'transparent' }}
    >
      {children}
    </button>
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

  const selectPane = (index: number) => {
    setLayout((current) => ({ ...current, activePane: index }))
    const sessionId = visibleIds[index]
    if (sessionId && sessionId !== activeSessionId) setActiveSession(sessionId)
  }

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
      ? <ChatPane key={session.id} session={session} active={layout.activePane === index} paneIndex={index} onActivate={() => selectPane(index)} />
      : <EmptyPane active={layout.activePane === index} paneIndex={index} onActivate={() => selectPane(index)} />
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
