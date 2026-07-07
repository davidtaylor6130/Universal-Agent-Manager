import { useCallback } from 'react'
import type { MouseEvent as ReactMouseEvent } from 'react'
import { PanelLeftClose, PanelLeftOpen, Brain, Settings2, GitBranch } from 'lucide-react'
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
import { IconButton, StatusDot } from '../ui'

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
      <IconButton
        icon={sidebarCollapsed ? <PanelLeftOpen size={17} /> : <PanelLeftClose size={17} />}
        label={sidebarCollapsed ? 'Expand chat selector' : 'Collapse chat selector'}
        tooltipSide="right"
        onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
      />
      <div className="uam-side-rail__spacer" />
      <ThemeToggle />
      <span className="relative inline-flex">
        <IconButton
          icon={<Brain size={17} />}
          label="Memory library"
          tooltip={memoryTitle}
          tooltipSide="right"
          active={hasMemories}
          onClick={() => { void openAllMemoryLibrary() }}
        />
        {hasActivity && (
          <span className="absolute -right-0.5 -top-0.5 pointer-events-none" data-testid="memory-activity-dot" aria-hidden="true">
            <StatusDot tone="accent" pulse size={7} />
          </span>
        )}
      </span>
      <IconButton
        icon={<Settings2 size={17} />}
        label="Settings"
        tooltipSide="right"
        onClick={() => setSettingsOpen(true)}
      />
    </aside>
  )
}

function RightActivityRail() {
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)

  return (
    <aside className="uam-side-rail uam-side-rail--right" aria-label="Tool windows">
      <IconButton
        icon={<GitBranch size={17} />}
        label={commitPanelOpen ? 'Close Git/SVN commit panel' : 'Open Git/SVN commit panel'}
        tooltipSide="left"
        active={commitPanelOpen}
        onClick={() => setCommitPanelOpen(!commitPanelOpen)}
      />
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
    let resizeAnimationFrame: number | null = null
    let pendingWidth = startWidth

    const commitResize = () => {
      resizeAnimationFrame = null
      if (side === 'sidebar') {
        setSidebarWidthPx(pendingWidth)
      } else {
        setCommitPanelWidthPx(pendingWidth)
      }
    }

    const onMouseMove = (moveEvent: MouseEvent) => {
      const delta = moveEvent.clientX - startX
      pendingWidth = side === 'sidebar' ? startWidth + delta : startWidth - delta
      if (resizeAnimationFrame === null) {
        resizeAnimationFrame = window.requestAnimationFrame(commitResize)
      }
    }

    const onMouseUp = () => {
      if (resizeAnimationFrame !== null) {
        window.cancelAnimationFrame(resizeAnimationFrame)
        resizeAnimationFrame = null
      }
      commitResize()
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
