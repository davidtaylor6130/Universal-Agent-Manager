import { useAppStore } from '../../store/useAppStore'
import { CLIView } from '../views/CLIView'

export function MainPanel() {
  const { activeSessionId, sessions } = useAppStore()
  const session = sessions.find((s) => s.id === activeSessionId)

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

        <div className="mr-3 text-xs" style={{ color: 'var(--text-3)' }}>
          CLI
        </div>
      </div>

      {/* View content */}
      <div className="flex-1 overflow-hidden">
        <CLIView session={session} />
      </div>
    </div>
  )
}
