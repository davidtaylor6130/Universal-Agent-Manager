import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { Sidebar } from './Sidebar'
import { MainPanel } from './MainPanel'
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

  return (
    <div className="uam-titlebar">
      <Logo size={24} showText={true} />
      <div className="uam-titlebar__spacer" />
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

  return (
    <div
      className="h-screen w-screen overflow-hidden flex flex-col uam-app"
      style={{ color: 'var(--text)' }}
    >
      <AppTitleBar />
      <PanelGroup direction="horizontal" className="flex-1 flex overflow-hidden">
        {/* Sidebar panel */}
        <Panel
          defaultSize={26}
          minSize={17}
          maxSize={34}
          style={{ background: 'var(--sidebar-bg)' }}
          className="flex flex-col overflow-hidden"
        >
          <Sidebar />
        </Panel>

        {/* Resize handle */}
        <PanelResizeHandle className="w-px cursor-col-resize flex-shrink-0 transition-colors duration-150"
          style={{ background: 'var(--border)' }}
        />

        {/* Main content panel */}
        <Panel className="flex flex-col overflow-hidden" style={{ background: 'var(--bg)' }}>
          <MainPanel />
        </Panel>
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
