import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
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

function AppTitleBar() {
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const openAllMemoryLibrary = useAppStore((s) => s.openAllMemoryLibrary)
  const sidebarCollapsed = useAppStore((s) => s.sidebarCollapsed)
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const setSidebarCollapsed = useAppStore((s) => s.setSidebarCollapsed)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)

  return (
    <div className="uam-titlebar">
      <Logo size={24} showText={true} />
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
      <div className="uam-titlebar__spacer" />
      <button
        type="button"
        title={commitPanelOpen ? 'Close Git/SVN commit panel' : 'Open Git/SVN commit panel'}
        aria-label={commitPanelOpen ? 'Close Git/SVN commit panel' : 'Open Git/SVN commit panel'}
        className="uam-icon-button"
        onClick={() => setCommitPanelOpen(!commitPanelOpen)}
      >
        <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          <path d="M4 2.5h6l2 2V13a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V3.5a1 1 0 0 1 1-1z" />
          <path d="M5 8h6M5 10.5h4" />
          <path d="M10 2.5V5h2" />
        </svg>
      </button>
      <ThemeToggle />
      <button
        type="button"
        title="Memory library"
        aria-label="Memory library"
        className="uam-icon-button"
        onClick={() => { void openAllMemoryLibrary() }}
      >
        <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
          <path d="M6.1 3.1A2 2 0 0 0 2.8 5a2.2 2.2 0 0 0 .2 4.3 2.1 2.1 0 0 0 3.1 2.3V3.1z" />
          <path d="M9.9 3.1A2 2 0 0 1 13.2 5a2.2 2.2 0 0 1-.2 4.3 2.1 2.1 0 0 1-3.1 2.3V3.1z" />
          <path d="M6.1 6.1H4.5M9.9 6.1h1.6M6.1 9.1H4.6M9.9 9.1h1.5" />
        </svg>
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
    </div>
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
  const commitPanelWidth = useAppStore((s) => s.commitPanelWidth)
  const setSidebarCollapsed = useAppStore((s) => s.setSidebarCollapsed)
  const setCommitPanelWidth = useAppStore((s) => s.setCommitPanelWidth)

  return (
    <div
      className="h-screen w-screen overflow-hidden flex flex-col uam-app"
      style={{ color: 'var(--text)' }}
    >
      <AppTitleBar />
      <PanelGroup direction="horizontal" className="flex-1 flex overflow-hidden">
        {/* Sidebar panel */}
        <Panel
          key={sidebarCollapsed ? 'sidebar-rail' : 'sidebar-full'}
          defaultSize={sidebarCollapsed ? 4 : 26}
          minSize={sidebarCollapsed ? 4 : 17}
          maxSize={sidebarCollapsed ? 4 : 34}
          style={{ background: 'var(--sidebar-bg)' }}
          className="flex flex-col overflow-hidden"
        >
          {sidebarCollapsed ? (
            <div className="flex h-full flex-col items-center gap-3 py-3" style={{ background: 'var(--sidebar-bg)' }}>
              <Logo size={22} showText={false} />
              <button
                type="button"
                title="Expand chat selector"
                aria-label="Expand chat selector"
                className="uam-icon-button"
                onClick={() => setSidebarCollapsed(false)}
              >
                <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                  <rect x="2.5" y="2.5" width="11" height="11" rx="1.5" />
                  <path d="M6 3v10" />
                  <path d="M10 6 7.5 8 10 10" />
                </svg>
              </button>
            </div>
          ) : (
            <Sidebar />
          )}
        </Panel>

        {/* Resize handle */}
        <PanelResizeHandle className="w-px cursor-col-resize flex-shrink-0 transition-colors duration-150"
          style={{ background: 'var(--border)' }}
        />

        {/* Main content panel */}
        <Panel className="flex flex-col overflow-hidden" style={{ background: 'var(--bg)' }}>
          <MainPanel />
        </Panel>

        {commitPanelOpen && (
          <>
            <PanelResizeHandle className="w-px cursor-col-resize flex-shrink-0 transition-colors duration-150"
              style={{ background: 'var(--border)' }}
            />
            <Panel
              defaultSize={commitPanelWidth}
              minSize={22}
              maxSize={42}
              onResize={(size) => setCommitPanelWidth(size)}
              className="flex flex-col overflow-hidden"
              style={{ background: 'var(--surface)' }}
            >
              <VcsCommitPanel />
            </Panel>
          </>
        )}
      </PanelGroup>

      {/* Modals */}
      {isNewChatModalOpen && <NewChatModal />}
      {isSettingsOpen && <SettingsModal />}
      {memoryLibraryScope && <MemoryLibraryModal />}
      {isMemoryScanModalOpen && <MemoryScanModal />}
      {isMarkdownStoreOpen && <MarkdownStoreModal />}
    </div>
  )
}
