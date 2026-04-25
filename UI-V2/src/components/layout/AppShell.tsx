import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { Sidebar } from './Sidebar'
import { MainPanel } from './MainPanel'
import { NewChatModal } from '../sidebar/NewChatModal'
import { SettingsModal } from '../settings/SettingsModal'
import { MemoryLibraryModal } from '../settings/MemoryLibraryModal'
import { MemoryScanModal } from '../settings/MemoryScanModal'
import { useAppStore } from '../../store/useAppStore'

export function AppShell() {
  const isNewChatModalOpen = useAppStore((s) => s.isNewChatModalOpen)
  const isSettingsOpen = useAppStore((s) => s.isSettingsOpen)
  const memoryLibraryScope = useAppStore((s) => s.memoryLibraryScope)
  const isMemoryScanModalOpen = useAppStore((s) => s.isMemoryScanModalOpen)

  return (
    <div
      className="h-screen w-screen overflow-hidden flex flex-col"
      style={{ background: 'var(--bg)', color: 'var(--text)' }}
    >
      <PanelGroup direction="horizontal" className="flex-1 flex overflow-hidden">
        {/* Sidebar panel */}
        <Panel
          defaultSize={19}
          minSize={13}
          maxSize={32}
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
    </div>
  )
}
