import { Logo } from '../shared/Logo'
import { ThemeToggle } from '../shared/ThemeToggle'
import { useAppStore } from '../../store/useAppStore'

export function SidebarHeader() {
  const { setNewChatModalOpen, setSettingsOpen } = useAppStore()

  return (
    <div
      className="flex items-center gap-2 px-3 flex-shrink-0"
      style={{ height: 48 }}
    >
      {/* Logo */}
      <div className="flex-1">
        <Logo size={18} showText={true} />
      </div>

      {/* Theme toggle */}
      <ThemeToggle />

      {/* Settings gear */}
      <button
        onClick={() => setSettingsOpen(true)}
        title="Settings"
        className="flex items-center justify-center rounded-md transition-colors duration-150"
        style={{
          width: 28,
          height: 28,
          background: 'transparent',
          color: 'var(--text-2)',
          cursor: 'pointer',
          border: 'none',
        }}
        onMouseEnter={(e) => {
          e.currentTarget.style.background = 'var(--sidebar-item-hover)'
          e.currentTarget.style.color = 'var(--text)'
        }}
        onMouseLeave={(e) => {
          e.currentTarget.style.background = 'transparent'
          e.currentTarget.style.color = 'var(--text-2)'
        }}
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <circle cx="12" cy="12" r="3" />
          <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
        </svg>
      </button>

      {/* New chat button */}
      <button
        onClick={() => setNewChatModalOpen(true)}
        title="New Chat (Ctrl+N)"
        className="flex items-center justify-center rounded-md transition-all duration-150"
        style={{
          width: 28,
          height: 28,
          background: 'var(--accent)',
          color: '#fff',
          cursor: 'pointer',
          border: 'none',
          flexShrink: 0,
        }}
        onMouseEnter={(e) => {
          e.currentTarget.style.opacity = '0.85'
        }}
        onMouseLeave={(e) => {
          e.currentTarget.style.opacity = '1'
        }}
      >
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round">
          <line x1="12" y1="5" x2="12" y2="19" />
          <line x1="5" y1="12" x2="19" y2="12" />
        </svg>
      </button>
    </div>
  )
}
