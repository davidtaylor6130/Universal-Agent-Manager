import { useCallback } from 'react'
import type { MouseEvent as ReactMouseEvent } from 'react'
import { Sidebar } from './Sidebar'
import { MainPanel } from './MainPanel'
import { VcsCommitPanel } from './VcsCommitPanel'
import { NewChatModal } from '../sidebar/NewChatModal'
import { SettingsModal } from '../settings/SettingsModal'
import { MemoryLibraryModal } from '../settings/MemoryLibraryModal'
import { MemoryScanModal } from '../settings/MemoryScanModal'
import { MarkdownStoreModal } from '../settings/MarkdownStoreModal'
import { useAppStore } from '../../store/useAppStore'
import { Logo } from '../shared/Logo'
import { ThemeToggle } from '../shared/ThemeToggle'

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

function LeftActivityRail() {
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const openAllMemoryLibrary = useAppStore((s) => s.openAllMemoryLibrary)
  const sidebarCollapsed = useAppStore((s) => s.sidebarCollapsed)
  const setSidebarCollapsed = useAppStore((s) => s.setSidebarCollapsed)
  const memoryActivity = useAppStore((s) => s.memoryActivity)
  const hasMemories = memoryActivity.entryCount > 0
  const hasActivity = memoryActivity.runningCount > 0 || memoryActivity.lastCreatedCount > 0
  const memoryTitle = formatMemoryTitle(memoryActivity.entryCount, memoryActivity.lastCreatedAt)

  return (
    <aside className="uam-side-rail uam-side-rail--left" aria-label="Main navigation">
      <Logo size={24} showText={false} />
      <button
        type="button"
        title={sidebarCollapsed ? 'Expand chat selector' : 'Collapse chat selector'}
        aria-label={sidebarCollapsed ? 'Expand chat selector' : 'Collapse chat selector'}
        className="uam-icon-button"
        onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
      >
        <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          <rect x="2.5" y="2.5" width="11" height="11" rx="1.5" />
          <path d="M6 3v10" />
          <path d={sidebarCollapsed ? 'M10 6 7.5 8 10 10' : 'M8 6 10.5 8 8 10'} />
        </svg>
      </button>
      <div className="uam-side-rail__spacer" />
      <ThemeToggle />
      <button
        type="button"
        title={memoryTitle}
        aria-label="Memory library"
        className={`uam-icon-button ${hasMemories ? 'uam-icon-button--accent' : ''}`}
        onClick={() => { void openAllMemoryLibrary() }}
      >
        <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
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
      <button
        type="button"
        title="Settings"
        aria-label="Settings"
        className="uam-icon-button"
        onClick={() => setSettingsOpen(true)}
      >
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <circle cx="12" cy="12" r="3" />
          <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
        </svg>
      </button>
    </aside>
  )
}

function RightActivityRail() {
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)

  return (
    <aside className="uam-side-rail uam-side-rail--right" aria-label="Tool windows">
      <button
        type="button"
        title={commitPanelOpen ? 'Close Git/SVN commit panel' : 'Open Git/SVN commit panel'}
        aria-label={commitPanelOpen ? 'Close Git/SVN commit panel' : 'Open Git/SVN commit panel'}
        className={`uam-icon-button ${commitPanelOpen ? 'uam-icon-button--active' : ''}`}
        onClick={() => setCommitPanelOpen(!commitPanelOpen)}
      >
        <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          <path d="M4 2.5h6l2 2V13a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V3.5a1 1 0 0 1 1-1z" />
          <path d="M5 8h6M5 10.5h4" />
          <path d="M10 2.5V5h2" />
        </svg>
      </button>
    </aside>
  )
}

export function AppShell() {
  const isNewChatModalOpen = useAppStore((s) => s.isNewChatModalOpen)
  const isSettingsOpen = useAppStore((s) => s.isSettingsOpen)
  const memoryLibraryScope = useAppStore((s) => s.memoryLibraryScope)
  const isMemoryScanModalOpen = useAppStore((s) => s.isMemoryScanModalOpen)
  const isMarkdownStoreOpen = useAppStore((s) => s.isMarkdownStoreOpen)
  const sidebarCollapsed = useAppStore((s) => s.sidebarCollapsed)
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const sidebarWidthPx = useAppStore((s) => s.sidebarWidthPx)
  const commitPanelWidthPx = useAppStore((s) => s.commitPanelWidthPx)
  const setSidebarWidthPx = useAppStore((s) => s.setSidebarWidthPx)
  const setCommitPanelWidthPx = useAppStore((s) => s.setCommitPanelWidthPx)

  const startResize = useCallback((
    side: 'sidebar' | 'commit',
    event: ReactMouseEvent<HTMLDivElement>
  ) => {
    event.preventDefault()
    const startX = event.clientX
    const startWidth = side === 'sidebar' ? sidebarWidthPx : commitPanelWidthPx

    const onMouseMove = (moveEvent: MouseEvent) => {
      const delta = moveEvent.clientX - startX
      if (side === 'sidebar') {
        setSidebarWidthPx(startWidth + delta)
      } else {
        setCommitPanelWidthPx(startWidth - delta)
      }
    }

    const onMouseUp = () => {
      document.removeEventListener('mousemove', onMouseMove)
      document.removeEventListener('mouseup', onMouseUp)
      document.body.style.cursor = ''
      document.body.style.userSelect = ''
    }

    document.body.style.cursor = 'col-resize'
    document.body.style.userSelect = 'none'
    document.addEventListener('mousemove', onMouseMove)
    document.addEventListener('mouseup', onMouseUp)
  }, [commitPanelWidthPx, setCommitPanelWidthPx, setSidebarWidthPx, sidebarWidthPx])

  return (
    <div
      className="h-screen w-screen overflow-hidden flex uam-app"
      style={{ color: 'var(--text)' }}
    >
      <LeftActivityRail />

      {!sidebarCollapsed && (
        <>
          <aside
            className="flex h-full flex-col overflow-hidden"
            data-testid="chat-selector-panel"
            style={{ width: sidebarWidthPx, flex: `0 0 ${sidebarWidthPx}px`, background: 'var(--sidebar-bg)' }}
          >
            <Sidebar />
          </aside>
          <div
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize chat selector"
            className="uam-resize-handle"
            onMouseDown={(event) => startResize('sidebar', event)}
          />
        </>
      )}

      <main className="min-w-0 flex-1 overflow-hidden" style={{ background: 'var(--bg)' }}>
        <MainPanel />
      </main>

      {commitPanelOpen && (
        <>
          <div
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize Git/SVN commit panel"
            className="uam-resize-handle"
            onMouseDown={(event) => startResize('commit', event)}
          />
          <aside
            className="flex h-full flex-col overflow-hidden"
            data-testid="commit-panel"
            style={{ width: commitPanelWidthPx, flex: `0 0 ${commitPanelWidthPx}px`, background: 'var(--surface)' }}
          >
            <VcsCommitPanel />
          </aside>
        </>
      )}

      <RightActivityRail />

      {/* Modals */}
      {isNewChatModalOpen && <NewChatModal />}
      {isSettingsOpen && <SettingsModal />}
      {memoryLibraryScope && <MemoryLibraryModal />}
      {isMemoryScanModalOpen && <MemoryScanModal />}
      {isMarkdownStoreOpen && <MarkdownStoreModal />}
    </div>
  )
}
