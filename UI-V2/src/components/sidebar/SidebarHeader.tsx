import { Logo } from '../shared/Logo'
import { ThemeToggle } from '../shared/ThemeToggle'
import { useAppStore } from '../../store/useAppStore'

function formatMemoryTitle(entryCount: number, lastCreatedAt: string): string {
  if (entryCount <= 0) {
    return 'No memories yet'
  }

  const countLabel = `${entryCount} ${entryCount === 1 ? 'memory' : 'memories'} saved`
  const parsed = lastCreatedAt ? new Date(lastCreatedAt) : null
  if (!parsed || Number.isNaN(parsed.getTime())) {
    return countLabel
  }

  return `${countLabel}, last updated ${parsed.toLocaleString([], {
    month: 'short',
    day: 'numeric',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  })}`
}

export function SidebarHeader() {
  const { setNewChatModalOpen, setSettingsOpen, openAllMemoryLibrary, memoryActivity } = useAppStore()
  const hasMemories = memoryActivity.entryCount > 0
  const hasActivity = memoryActivity.runningCount > 0 || memoryActivity.lastCreatedCount > 0
  const memoryTitle = formatMemoryTitle(memoryActivity.entryCount, memoryActivity.lastCreatedAt)

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

      {/* All memory */}
      <button
        onClick={() => { void openAllMemoryLibrary() }}
        title={memoryTitle}
        aria-label={memoryTitle}
        className="relative flex items-center justify-center rounded-md transition-colors duration-150"
        style={{
          width: 28,
          height: 28,
          background: 'transparent',
          color: hasMemories ? 'var(--accent)' : 'var(--text-3)',
          cursor: 'pointer',
          border: 'none',
          flexShrink: 0,
        }}
        onMouseEnter={(e) => {
          e.currentTarget.style.background = 'var(--sidebar-item-hover)'
          e.currentTarget.style.color = hasMemories ? 'var(--accent)' : 'var(--text)'
        }}
        onMouseLeave={(e) => {
          e.currentTarget.style.background = 'transparent'
          e.currentTarget.style.color = hasMemories ? 'var(--accent)' : 'var(--text-3)'
        }}
      >
        <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          <path d="M6.1 3.1A2 2 0 0 0 2.8 5a2.2 2.2 0 0 0 .2 4.3 2.1 2.1 0 0 0 3.1 2.3V3.1z" />
          <path d="M9.9 3.1A2 2 0 0 1 13.2 5a2.2 2.2 0 0 1-.2 4.3 2.1 2.1 0 0 1-3.1 2.3V3.1z" />
          <path d="M6.1 6.1H4.5M9.9 6.1h1.6M6.1 9.1H4.6M9.9 9.1h1.5" />
        </svg>
        {hasActivity && (
          <span
            className="memory-activity-dot"
            data-testid="memory-activity-dot"
            aria-hidden="true"
          />
        )}
      </button>

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
