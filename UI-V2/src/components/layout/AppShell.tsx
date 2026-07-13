import { useCallback, useEffect, useState } from 'react'
import type { MouseEvent as ReactMouseEvent } from 'react'
import { PanelLeftClose, PanelLeftOpen, Brain, Settings2, GitBranch, ArrowUpCircle, Bell } from 'lucide-react'

/** GitHub mark (lucide dropped brand icons). */
function GithubLogo({ size = 17 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" fill="currentColor" aria-hidden focusable="false">
      <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.6 7.6 0 0 1 2-.27c.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z" />
    </svg>
  )
}

/** Simplified Apache Subversion mark. */
function SvnLogo({ size = 17 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden focusable="false">
      <circle cx="12" cy="12" r="2.2" fill="currentColor" stroke="none" />
      <path d="M12 4a8 8 0 0 1 7 4.1" />
      <path d="M20 12a8 8 0 0 1-4.1 7" />
      <path d="M12 20a8 8 0 0 1-7-4.1" />
      <path d="M4 12a8 8 0 0 1 4.1-7" />
    </svg>
  )
}
import { Sidebar } from './Sidebar'
import { MainPanel } from './MainPanel'
import { VcsCommitPanel } from './VcsCommitPanel'
import { UpdatesPanel } from './UpdatesPanel'
import { NewChatModal } from '../sidebar/NewChatModal'
import { SettingsModal } from '../settings/SettingsModal'
import { MemoryLibraryModal } from '../settings/MemoryLibraryModal'
import { MemoryScanModal } from '../settings/MemoryScanModal'
import { MarkdownStoreModal } from '../settings/MarkdownStoreModal'
import { useAppStore } from '../../store/useAppStore'
import { Logo } from '../shared/Logo'
import { ThemeToggle } from '../shared/ThemeToggle'
import { IconButton, StatusDot } from '../ui'
import { useUpdateMonitor, type UpdateMonitor } from '../../hooks/useUpdateMonitor'

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

function RightActivityRail({ monitor, updatesOpen, onToggleUpdates }: { monitor: UpdateMonitor; updatesOpen: boolean; onToggleUpdates: () => void }) {
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const getChatWorktreeStatus = useAppStore((s) => s.getChatWorktreeStatus)
  const missingFolders = useAppStore((s) => s.folders.filter((folder) => folder.missing))
  const shellActionNotification = useAppStore((s) => s.shellActionNotification)
  const [vcsKind, setVcsKind] = useState<'git' | 'svn' | null>(null)
  const [alertsOpen, setAlertsOpen] = useState(false)

  // Detect the active chat's VCS so the toggle shows the matching logo.
  useEffect(() => {
    if (!activeSessionId) { setVcsKind(null); return }
    let cancelled = false
    void getChatWorktreeStatus(activeSessionId).then((status) => {
      if (cancelled) return
      setVcsKind(status?.isSvnWorkspace ? 'svn' : status?.isGitRepository ? 'git' : null)
    })
    return () => { cancelled = true }
  }, [activeSessionId, getChatWorktreeStatus])

  const vcsLabelKind = vcsKind === 'svn' ? 'SVN' : vcsKind === 'git' ? 'Git' : 'Git/SVN'
  const vcsIcon = vcsKind === 'svn' ? <SvnLogo size={17} /> : vcsKind === 'git' ? <GithubLogo size={16} /> : <GitBranch size={17} />

  const updateCount = monitor.updates.length
  const runtimeUpdateCount = monitor.updates.filter((update) => update.providerId).length
  const alertCount = missingFolders.length + (shellActionNotification ? 1 : 0)

  return (
    <aside className="uam-side-rail uam-side-rail--right" aria-label="Tool windows">
      <IconButton
        icon={vcsIcon}
        label={commitPanelOpen ? `Close ${vcsLabelKind} commit panel` : `Open ${vcsLabelKind} commit panel`}
        tooltipSide="left"
        active={commitPanelOpen}
        onClick={() => setCommitPanelOpen(!commitPanelOpen)}
      />
      <div className="uam-side-rail__spacer" />
      <span className="relative inline-flex">
        <IconButton
          icon={<Bell size={17} />}
          label={alertCount > 0 ? `${alertCount} alert${alertCount === 1 ? '' : 's'}` : 'No new alerts'}
          tooltipSide="left"
          active={alertsOpen}
          onClick={() => setAlertsOpen((open) => !open)}
        />
        {alertCount > 0 && (
          <span className="absolute -right-0.5 -top-0.5 pointer-events-none" aria-hidden>
            <StatusDot tone="warning" size={7} />
          </span>
        )}
        {alertsOpen && (
          <div
            role="status"
            aria-label="Notifications"
            className="absolute right-9 bottom-0 z-50 grid gap-2 rounded-md p-3"
            style={{ width: 320, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
          >
            {missingFolders.length === 0 && !shellActionNotification ? (
              <span className="text-xs" style={{ color: 'var(--text-2)' }}>No notifications.</span>
            ) : <>
              {shellActionNotification && (
                <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  <strong style={{ color: 'var(--text)' }}>Finder / Explorer action</strong>
                  <span>{shellActionNotification}</span>
                </div>
              )}
              {missingFolders.map((folder) => (
              <div key={folder.id} className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                <strong style={{ color: 'var(--yellow)' }}>Workspace folder missing: {folder.name}</strong>
                <span className="break-all" style={{ color: 'var(--text-3)' }}>{folder.directory}</span>
              </div>
              ))}
            </>}
          </div>
        )}
      </span>
      <span className="relative inline-flex">
        <IconButton
          icon={<ArrowUpCircle size={17} />}
          label={updateCount > 0 ? `${updateCount} update${updateCount === 1 ? '' : 's'} available` : 'Check for updates'}
          tooltipSide="left"
          active={updatesOpen || updateCount > 0}
          onClick={onToggleUpdates}
        />
        {runtimeUpdateCount > 0 && (
          <span className="absolute -right-1.5 -top-1.5 pointer-events-none min-w-4 rounded-full px-1 text-center text-[9px] font-bold leading-4" style={{ background: 'var(--accent)', color: 'var(--bg)' }} aria-hidden>
            {runtimeUpdateCount}
          </span>
        )}
      </span>
    </aside>
  )
}

export function AppShell() {
  const refreshCustomThemes = useAppStore((state) => state.refreshCustomThemes)
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
  const updateMonitor = useUpdateMonitor()
  const [updatesOpen, setUpdatesOpen] = useState(false)

  useEffect(() => {
    void refreshCustomThemes()
  }, [refreshCustomThemes])

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

      {updatesOpen && <UpdatesPanel monitor={updateMonitor} onClose={() => setUpdatesOpen(false)} />}

      <RightActivityRail
        monitor={updateMonitor}
        updatesOpen={updatesOpen}
        onToggleUpdates={() => setUpdatesOpen((open) => !open)}
      />

      {/* Modals */}
      {isNewChatModalOpen && <NewChatModal />}
      {isSettingsOpen && <SettingsModal />}
      {memoryLibraryScope && <MemoryLibraryModal />}
      {isMemoryScanModalOpen && <MemoryScanModal />}
      {isMarkdownStoreOpen && <MarkdownStoreModal />}
    </div>
  )
}
