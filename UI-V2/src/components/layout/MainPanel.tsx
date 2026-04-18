import { memo, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { CLIView } from '../views/CLIView'
import { ChatView } from '../views/ChatView'
import { isCefContext } from '../../ipc/cefBridge'

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

export function MainPanel() {
  const [view, setView] = useState<'chat' | 'cli'>('chat')
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const session = useAppStore(useShallow((s) => s.sessions.find((x) => x.id === activeSessionId) ?? null))
  const acpBinding = useAppStore((s) => activeSessionId ? s.acpBindingBySessionId[activeSessionId] : undefined)
  const cliBinding = useAppStore((s) => activeSessionId ? s.cliBindingBySessionId[activeSessionId] : undefined)
  const viewSwitchLocked = Boolean(
    acpBinding?.processing ||
      acpBinding?.lifecycleState === 'waitingPermission' ||
      cliBinding?.processing ||
      cliBinding?.lifecycleState === 'busy' ||
      cliBinding?.lifecycleState === 'shuttingDown'
  )

  if (!session) {
    return (
      <div
        className="flex-1 flex items-center justify-center h-full"
        style={{ color: 'var(--text-3)' }}
      >
        <div className="text-center">
          <div className="text-3xl mb-3" style={{ opacity: 0.3 }}>◈</div>
          <div className="text-sm" style={{ color: 'var(--text-3)' }}>
            No session selected
          </div>
          <div className="text-xs mt-1" style={{ color: 'var(--text-3)', opacity: 0.6 }}>
            Select or create a session from the sidebar
          </div>
        </div>
      </div>
    )
  }

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {/* Header bar */}
      <div
        className="flex items-center gap-0 flex-shrink-0 px-1"
        style={{
          height: 40,
          borderBottom: '1px solid var(--border)',
          background: 'var(--surface)',
        }}
      >
        {/* Session name */}
        <div
          className="flex-1 px-3 text-sm font-medium truncate"
          style={{ color: 'var(--text)' }}
          title={session.name}
        >
          {session.name}
        </div>

        <div
          className="flex items-center p-0.5 mr-2"
          style={{
            border: '1px solid var(--border)',
            borderRadius: 6,
            background: 'var(--bg)',
          }}
        >
          <button
            type="button"
            title={viewSwitchLocked && view !== 'chat' ? 'Wait for current output to finish' : 'Chat view'}
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
          <button
            type="button"
            title={viewSwitchLocked && view !== 'cli' ? 'Wait for current output to finish' : 'Terminal fallback'}
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
        </div>

        {isCefContext() && <PushStatusDot />}
      </div>

      {/* View content */}
      <div className="flex-1 overflow-hidden">
        {view === 'chat' ? <ChatView session={session} /> : <CLIView session={session} />}
      </div>
    </div>
  )
}
