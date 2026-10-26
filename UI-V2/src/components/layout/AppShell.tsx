import { SettingsLoading } from '../settings/SettingsPrimitives'
import { StartupNativeImportChooser } from '../modals/StartupNativeImportChooser'
import { ProviderMcpWarning } from '../shared/ProviderMcpWarning'
import { nextAttentionSession } from '../../utils/nextAttention'
import { useShallow } from 'zustand/react/shallow'
import { subscribeChatGridLayout } from '../../utils/chatGridStorage'
import { subscribeSubagentNavigation } from '../../utils/subagentNavigation'
import { lazy, memo, Suspense, useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from 'react'
import type { KeyboardEvent as ReactKeyboardEvent, MouseEvent as ReactMouseEvent } from 'react'
import { PanelLeftClose, PanelLeftOpen, Settings2, GitBranch, ArrowUpCircle, Bell, CheckCheck, CheckCircle2, AlertTriangle, Info, X, XCircle, ChartNoAxesCombined } from 'lucide-react'
import './panels.css'

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
import { InterfaceErrorBoundary } from '../shared/InterfaceErrorBoundary'
import { Sidebar } from './Sidebar'
import { MainPanel } from './MainPanel'
import { WindowChrome, useWindowChrome } from './WindowChrome'
import { UpdatesPanel } from './UpdatesPanel'
import type { SettingsHandle } from '../settings/SettingsModal'
import { useAppStore } from '../../store/useAppStore'
import { Logo } from '../shared/Logo'
import { ThemeToggle } from '../shared/ThemeToggle'
import { Button, IconButton, Notice, Presence, StatusDot, Tooltip, usePresence } from '../ui'
import { VIEWPORT_MENU_Z_INDEX } from '../ui/ViewportMenu'
import type { ButtonVariant } from '../ui'
import { useUpdateMonitor, type UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { FILE_MANAGER_NAME, matchShortcut, shortcutLabel } from '../../utils/shortcuts'
import { focusActiveInput } from '../../utils/activeInputFocus'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'
import { useNotificationClock, useNotificationTimes } from '../../hooks/useNotificationTimes'
import { notificationTimeGroup, relativeNotificationTime } from '../../utils/notificationTime'
import { isSshHost } from '../../utils/remoteWorkspace'
import { SSH_ENABLED } from '../../config/buildFeatures'

// These panels subscribe to their own state; shell navigation must not rerender their histories.
const ChatPanel = memo(MainPanel)
const ChatSidebar = memo(Sidebar)

const SettingsModal = lazy(() => import('../settings/SettingsModal').then(({ SettingsModal }) => ({ default: SettingsModal })))
const UsagePage = lazy(() => import('../usage/UsagePage').then(({ UsagePage }) => ({ default: UsagePage })))
const VcsCommitPanel = lazy(() => import('./VcsCommitPanel').then(({ VcsCommitPanel }) => ({ default: VcsCommitPanel })))
const NewChatModal = lazy(() => import('../sidebar/NewChatModal').then(({ NewChatModal }) => ({ default: NewChatModal })))
const MemoryLibraryModal = lazy(() => import('../settings/MemoryLibraryModal').then(({ MemoryLibraryModal }) => ({ default: MemoryLibraryModal })))
const MemoryScanModal = lazy(() => import('../settings/MemoryScanModal').then(({ MemoryScanModal }) => ({ default: MemoryScanModal })))
const MarkdownStoreModal = lazy(() => import('../settings/MarkdownStoreModal').then(({ MarkdownStoreModal }) => ({ default: MarkdownStoreModal })))

type TransientAlert = { id: string; tone: 'error' | 'info'; message: string; actionLabel: string; action: 'notifications' | 'updates' }

const ALERT_EXPIRY_MS = 8000

/** The one shared transient alert for app-wide messages; every message is also kept in the notifications panel. */
function TransientAlertSurface({ alert, notice, onAction, onDismiss, onHeldChange, fallbackId, onFallbackChange }: { alert: TransientAlert | null; notice: Pick<TransientAlert, 'id' | 'message'> | null; onAction: (action: TransientAlert['action']) => void; onDismiss: () => void; onHeldChange: (held: boolean) => void; fallbackId: string; onFallbackChange: (fallback: boolean) => void }) {
  const { mounted, closing } = usePresence(alert !== null)
  const surfaceRef = useRef<HTMLDivElement>(null)
  const [hovered, setHovered] = useState(false)
  const [focused, setFocused] = useState(false)
  const [placement, setPlacement] = useState<{ right: number; width: number; height: number; fits: boolean } | null>(null)
  const [announcement, setAnnouncement] = useState<{ id: string; message: string } | null>(null)
  // Keep the existing rail-relative horizontal anchor, with the popup at the GUI's bottom edge.
  // David explicitly permits covering Send; composer growth and first-send FLIP do not lift it.
  useLayoutEffect(() => {
    if (!mounted || !alert || !surfaceRef.current) {
      setPlacement(null)
      onFallbackChange(false)
      return
    }
    const surface = surfaceRef.current
    const app = surface.closest('.uam-app')
    let frame: number | null = null
    let observed: Element[] = []
    const visibleRect = (element: Element) => {
      const rect = element.getBoundingClientRect()
      return rect.width > 0 && rect.height > 0 && rect.bottom > 0 && rect.top < window.innerHeight ? rect : null
    }
    const measure = () => {
      const rail = app?.querySelector('.uam-side-rail--right')
      const bell = app?.querySelector<HTMLElement>('[data-notification-bell]')
      const railRect = rail ? visibleRect(rail) : null
      const bellRect = bell ? visibleRect(bell) : null
      const rightEdge = Math.min(window.innerWidth - 16, (railRect?.left ?? window.innerWidth) - 8)
      const width = Math.max(0, Math.min(360, rightEdge - 16))
      const left = rightEdge - width
      const overlaps = (rect: DOMRect) => rect.left < rightEdge && rect.right > left
      const bottomEdge = window.innerHeight
      const appRect = app?.getBoundingClientRect()
      const stripHeight = app ? Number.parseFloat(getComputedStyle(app).paddingTop) || 0 : 0
      let topEdge = Math.max(16, (appRect?.top ?? 0) + stripHeight + 8)
      for (const header of app?.querySelectorAll('.uam-chat-pane__header') ?? []) {
        const rect = visibleRect(header)
        if (rect && overlaps(rect) && rect.top < bottomEdge) topEdge = Math.max(topEdge, rect.bottom + 8)
      }
      const height = Math.max(0, bottomEdge - topEdge)
      const buttons = Array.from(surface.querySelectorAll('button')).map((button) => button.getBoundingClientRect())
      const actionWidth = buttons.reduce((sum, rect) => sum + rect.width, 0) + 32
      const actionHeight = Math.max(26, ...buttons.map((rect) => rect.height)) + 10
      const fits = Boolean(railRect && bellRect && width >= actionWidth + 64 && height >= actionHeight)
      const next = { right: window.innerWidth - rightEdge, width, height, fits }
      setPlacement((current) => current && Object.keys(next).every((key) => current[key as keyof typeof next] === next[key as keyof typeof next]) ? current : next)
      onFallbackChange(!fits)
      if (!fits) setHovered(false)
      if (!fits && surface.contains(document.activeElement)) {
        bell?.focus()
        setFocused(false)
      }
    }
    const schedule = () => {
      if (frame === null) frame = window.requestAnimationFrame(() => { frame = null; bind(); measure() })
    }
    const resize = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(schedule)
    const bind = () => {
      const next = [surface, ...(app ? [app, ...app.querySelectorAll('.uam-side-rail--right, .uam-chat-pane__header')] : [])]
      if (next.length === observed.length && next.every((element, index) => element === observed[index])) return
      resize?.disconnect()
      observed = next
      observed.forEach((element) => resize?.observe(element))
    }
    bind()
    // Existing layout/selection notifications cover mount replacement; do not observe transcript mutations.
    const unsubscribeGrid = subscribeChatGridLayout(schedule)
    const unsubscribeChild = subscribeSubagentNavigation(schedule)
    const unsubscribeStore = useAppStore.subscribe((state, previous) => {
      if (state.activeSessionId !== previous.activeSessionId || state.isSettingsOpen !== previous.isSettingsOpen ||
          (state.sessions !== previous.sessions && state.sessions.map((session) => `${session.id}:${session.viewMode}`).join('|') !==
            previous.sessions.map((session) => `${session.id}:${session.viewMode}`).join('|'))) schedule()
    })
    window.addEventListener('resize', schedule)
    measure()
    return () => {
      resize?.disconnect()
      unsubscribeGrid()
      unsubscribeChild()
      unsubscribeStore()
      window.removeEventListener('resize', schedule)
      if (frame !== null) window.cancelAnimationFrame(frame)
    }
  }, [mounted, alert?.id, onFallbackChange])
  // Publish into the already-mounted live region once per distinct notice, independently of placement.
  // The retained-notification instruction stays true whether the visible alert fits or falls back to the bell.
  useEffect(() => {
    if (!notice) return
    setAnnouncement((previous) => previous?.id === notice.id && previous.message === notice.message
      ? previous : { id: notice.id, message: notice.message })
  }, [notice?.id, notice?.message])
  // Hover or focus inside the alert holds the expiry timer so its actions never vanish under the pointer or keyboard.
  const held = alert !== null && (hovered || focused)
  useEffect(() => { onHeldChange(held) }, [held, onHeldChange])
  useEffect(() => {
    if (alert) return
    setHovered(false)
    setFocused(false)
  }, [alert])
  // Keep the last alert on screen while it animates out.
  const lastAlert = useRef(alert)
  if (alert) lastAlert.current = alert
  const current = lastAlert.current
  const liveRegion = <span id={fallbackId} data-testid="transient-alert-announcer" role="status" aria-live="polite" aria-atomic="true" className="sr-only">{announcement && <span key={announcement.id}>{announcement.message} Saved in Notifications. Use the Notifications button to read it.</span>}</span>
  if (!mounted || !current) return liveRegion
  // A focused control is about to unmount; hand focus to the bell rather than dropping it to the document.
  const finish = (run: () => void) => {
    const hadFocus = surfaceRef.current?.contains(document.activeElement)
    run()
    if (hadFocus) document.querySelector<HTMLElement>('[data-notification-bell]')?.focus()
  }
  return <>
    {liveRegion}
    <div ref={surfaceRef} data-testid="transient-alert" data-state={closing ? 'closed' : 'open'}
      aria-hidden={!placement?.fits || undefined}
      onMouseEnter={() => setHovered(true)} onMouseLeave={() => setHovered(false)}
      onFocus={() => setFocused(true)} onBlur={(event) => { if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setFocused(false) }}
      className="uam-transient-alert fixed flex items-center gap-2 rounded-md py-1 pl-3 pr-1 text-xs"
      style={{ zIndex: VIEWPORT_MENU_Z_INDEX + 1, right: placement?.right ?? 16, bottom: 0, width: placement?.width ?? 360, maxHeight: placement?.height, visibility: placement?.fits ? 'visible' : 'hidden', background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}>
      <span tabIndex={0} role="region" aria-label="Notification message" className="min-w-0 flex-1 focus-visible:ring-1 focus-visible:ring-[var(--accent)]" style={{ overflowWrap: 'anywhere', overflowY: 'auto', maxHeight: Math.max(0, (placement?.height ?? 0) - 10), color: current.tone === 'error' ? 'var(--error)' : 'var(--text)' }}>{current.message}</span>
      <Button className="shrink-0" variant="ghost" size="sm" onClick={() => finish(() => { onAction(current.action); onDismiss() })}>{current.actionLabel}</Button>
      <IconButton className="shrink-0" icon={<X size={13} />} size="sm" label="Dismiss alert" onClick={() => finish(onDismiss)} />
    </div>
  </>
}

const SIDEBAR_WIDTH_MIN = 260
const SIDEBAR_WIDTH_MAX = 520
const COMMIT_PANEL_WIDTH_MIN = 320
const COMMIT_PANEL_WIDTH_MAX = 680
const KEYBOARD_RESIZE_STEP = 16
const MIN_CHAT_WIDTH = 360
const ACTIVITY_RAILS_WIDTH = 88
const RESIZE_HANDLE_WIDTH = 5
const FIXED_RIGHT_PANEL_WIDTH = 360

function shellActionNotificationId(detail: string): string {
  return `shell-action-${detail}`
}

function statusLineNotificationId(detail: string): string {
  return `status-line-${detail}`
}

function providerUpdateNotificationId(result: UpdateMonitor['providerUpdateResults'][number]): string {
  return `provider-update-${result.executionHostId ? JSON.stringify([result.executionHostId, result.providerId]) : result.providerId}-${result.installedVersion}-${result.message}`
}

function updateSummaryText(updates: UpdateMonitor['updates']): string {
  return updates.map((update) => `${update.name} ${update.latestVersion}`).join(' · ')
}

function updateNotificationId(updates: UpdateMonitor['updates']): string {
  return `updates-${updateSummaryText(updates)}`
}

function missingFolderNotificationId(folder: { id: string; name: string; directory: string }): string {
  return `missing-folder-${folder.id}-${folder.name}-${folder.directory}`
}

type RemoteConnectionNotification = {
  id: string
  connectionKey: string
  hostLabel: string
  title: string
  detail: string
  warning?: boolean
}

const REMOTE_CONNECTION_ERROR = /remote|runner|ssh|bridge|disconnect|reconnect|transport|helper|app-server|process exited during an active turn|timed out/i

function matchingRemoteConnectionError(...errors: Array<string | undefined>): string {
  return errors.find((error) => error && REMOTE_CONNECTION_ERROR.test(error)) ?? ''
}

function remoteConnectionIssueSignature(state: ReturnType<typeof useAppStore.getState>): string {
  const hosts = new Map(state.executionHosts.map((host) => [host.id, host]))
  const signature: string[] = []
  for (const host of state.executionHosts) {
    if (isSshHost(host) && (host.runnerStatus === 'offline' || host.runnerStatus === 'error')) {
      signature.push(['host', host.id, host.label, host.runnerStatus].join('\0'))
    }
  }
  for (const session of state.sessions) {
    const hostId = session.executionHostId ?? 'local'
    if (hostId === 'local') continue
    const error = matchingRemoteConnectionError(
      state.acpBindingBySessionId[session.id]?.lastError,
      state.cliBindingBySessionId[session.id]?.lastError,
    )
    if (!error) continue
    signature.push(['session', session.id, session.name, hostId, hosts.get(hostId)?.label || hostId, error].join('\0'))
  }
  return signature.join('\n')
}

function remoteConnectionIssues(
  executionHosts: ReturnType<typeof useAppStore.getState>['executionHosts'],
  sessions: ReturnType<typeof useAppStore.getState>['sessions'],
  acpBindings: ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'],
  cliBindings: ReturnType<typeof useAppStore.getState>['cliBindingBySessionId'],
): RemoteConnectionNotification[] {
  const hosts = new Map(executionHosts.map((host) => [host.id, host]))
  const issues = executionHosts.flatMap((host) => isSshHost(host) && (host.runnerStatus === 'offline' || host.runnerStatus === 'error') ? [{
    id: `remote-host-${host.id}-${host.runnerStatus}`,
    connectionKey: `host-${host.id}`,
    hostLabel: host.label,
    title: `${host.label} connection ${host.runnerStatus}`,
    detail: host.runnerStatus === 'offline' ? 'UAM cannot currently reach this SSH helper.' : 'The SSH helper needs attention in Remote Hosts.',
    warning: true,
  }] : [])
  for (const session of sessions) {
    const hostId = session.executionHostId ?? 'local'
    if (hostId === 'local') continue
    const error = matchingRemoteConnectionError(
      acpBindings[session.id]?.lastError,
      cliBindings[session.id]?.lastError,
    )
    if (!error) continue
    const hostLabel = hosts.get(hostId)?.label || hostId
    issues.push({
      id: `remote-session-${session.id}-${error}`,
      connectionKey: `session-${session.id}`,
      hostLabel,
      title: `${hostLabel} connection issue`,
      detail: `${session.name}: ${error}`,
      warning: true,
    })
  }
  return issues
}

function LeftActivityRail({ settingsOpen, usageOpen, onToggleUsage, onToggleSettings }: {
  settingsOpen: boolean
  usageOpen: boolean
  onToggleUsage: () => void
  onToggleSettings: () => void
}) {
  const sidebarCollapsed = useAppStore((s) => s.sidebarCollapsed)
  const setSidebarCollapsed = useAppStore((s) => s.setSidebarCollapsed)

  return (
    <aside className="uam-side-rail uam-side-rail--left" aria-label="Main navigation">
      <Logo size={24} showText={false} />
      <IconButton
        icon={sidebarCollapsed ? <PanelLeftOpen size={17} /> : <PanelLeftClose size={17} />}
        label={sidebarCollapsed ? 'Expand chat selector' : 'Collapse chat selector'}
        disabled={settingsOpen || usageOpen}
        style={settingsOpen || usageOpen ? {opacity:0.35, color:'var(--text-3)'} : undefined}
        tooltipSide="right"
        shortcut={shortcutLabel('sidebar')}
        onClick={() => setSidebarCollapsed(!sidebarCollapsed)}
      />
      <div className="uam-side-rail__spacer" />
      <ThemeToggle />
      <IconButton icon={<ChartNoAxesCombined size={17} />} label="Usage" tooltipSide="right" active={usageOpen} onClick={onToggleUsage} />
      <IconButton
        icon={<Settings2 size={17} />}
        label="Settings"
        tooltipSide="right"
        shortcut={shortcutLabel('settings')}
        active={settingsOpen}
        onClick={onToggleSettings}
      />
    </aside>
  )
}

function NotificationsPanel({
  closing = false,
  dismissedNotificationIds,
  notificationTimes,
  remoteNotifications,
  failures,
  updates,
  providerUpdateResults,
  onClose,
  onDismiss,
  onReviewUpdates,
}: {
  closing?: boolean
  dismissedNotificationIds: ReadonlySet<string>
  notificationTimes: ReadonlyMap<string, number>
  remoteNotifications: RemoteConnectionNotification[]
  failures: AppFailure[]
  updates: UpdateMonitor['updates']
  providerUpdateResults: UpdateMonitor['providerUpdateResults']
  onClose: () => void
  onDismiss: (id: string) => void
  onReviewUpdates: () => void
}) {
  const missingFolders = useAppStore(useShallow((s) => s.folders.filter((folder) => folder.missing)))
  const shellActionNotification = useAppStore((s) => s.shellActionNotification)
  const statusLine = useAppStore((s) => s.statusLine)
  const sessions = useAppStore((s) => s.sessions)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const renameFolder = useAppStore((s) => s.renameFolder)
  const deleteFolder = useAppStore((s) => s.deleteFolder)
  const [busyAction, setBusyAction] = useState('')
  const [confirmingFolderId, setConfirmingFolderId] = useState('')
  const [folderActionError, setFolderActionError] = useState('')
  const headingRef = useRef<HTMLDivElement>(null)

  type NotificationAction = { label: string; variant?: ButtonVariant; run: () => void | Promise<void> }
  type Notification = { id: string; title: string; detail: string; warning?: boolean; failure?: AppFailure; actions?: NotificationAction[] }

  const notifications: Notification[] = [
    ...(statusLine ? [{
      id: statusLineNotificationId(statusLine),
      title: 'Application status',
      detail: statusLine,
      warning: /failed|could not|cannot|error|warning|corrupt|missing|unavailable|timed out|rejected/i.test(statusLine),
    }] : []),
    ...(shellActionNotification ? [{
      id: shellActionNotificationId(shellActionNotification),
      title: `${FILE_MANAGER_NAME} action`,
      detail: shellActionNotification,
    }] : []),
    ...failures.map((failure): Notification => ({ id: failure.id, title: 'Change not saved', detail: failure.detail, failure })),
    ...remoteNotifications,
    ...(updates.length > 0 ? [{
      id: updateNotificationId(updates),
      title: `${updates.length} update${updates.length === 1 ? '' : 's'} available`,
      detail: updateSummaryText(updates),
      actions: [{ label: 'Review updates', variant: 'primary' as const, run: onReviewUpdates }],
    }] : []),
    ...providerUpdateResults.filter((result) => result.status === 'succeeded').map((result): Notification => ({
      id: providerUpdateNotificationId(result),
      title: `${result.name} update completed`,
      detail: result.message || `Installed ${result.installedVersion}.`,
    })),
    ...missingFolders.map((folder): Notification => ({
      id: missingFolderNotificationId(folder),
      title: `Workspace folder missing: ${folder.name}`,
      detail: folder.directory,
      warning: true,
      actions: [
        {
          label: 'Relink',
          variant: 'primary',
          run: async () => {
            const directory = await browseFolderDirectory(folder.directory)
            if (!directory) return
            setFolderActionError('')
            if (!await renameFolder(folder.id, folder.name, directory)) {
              setFolderActionError('The workspace could not be relinked. Finish active work and try again.')
            }
          },
        },
        {
          label: 'Remove',
          variant: 'danger',
          run: () => setConfirmingFolderId(folder.id),
        },
      ],
    })),
  ].filter((notification) => !dismissedNotificationIds.has(notification.id))
  const now = useNotificationClock(!closing && notifications.length > 0)
  const timestampedNotifications = notifications.map((notification) => ({
    ...notification,
    time: notificationTimes.get(notification.id) ?? now,
  })).sort((a, b) => b.time - a.time)
  const confirmingFolder = missingFolders.find((folder) => folder.id === confirmingFolderId)
  const confirmingChatCount = confirmingFolder ? sessions.filter((session) => session.folderId === confirmingFolder.id).length : 0
  const confirmingChats = confirmingChatCount === 1 ? '1 chat' : `${confirmingChatCount} chats`

  const runAction = async (notificationId: string, action: NotificationAction) => {
    const actionId = `${notificationId}-${action.label}`
    setBusyAction(actionId)
    try {
      await action.run()
    } finally {
      setBusyAction('')
    }
  }

  const groups = (['Today', 'Earlier'] as const).map((label) => ({
    label,
    items: timestampedNotifications.filter((notification) => notificationTimeGroup(notification.time, now) === label),
  })).filter((group) => group.items.length > 0)

  return (
    <aside
      aria-label="Notifications"
      data-testid="notifications-panel"
      data-state={closing ? 'closed' : 'open'}
      className="uam-shell-panel uam-shell-panel--right flex h-full w-[360px] shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)', '--panel-w': '360px' } as React.CSSProperties}
    >
      <header className="flex items-center gap-1 px-4 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div ref={headingRef} tabIndex={-1} data-notifications-heading className="flex min-w-0 flex-1 items-center gap-2 text-sm font-semibold outline-none" style={{ color: 'var(--text)' }}>
          <Bell size={16} aria-hidden /> Notifications
          {notifications.length > 0 && (
            <span className="rounded-full px-1.5 text-[11px] font-medium leading-[18px] tabular-nums" style={{ background: 'var(--surface-high)', color: 'var(--text-2)' }}>{notifications.length}</span>
          )}
        </div>
        {notifications.length > 0 && (
          <IconButton
            icon={<CheckCheck size={16} />}
            label="Mark all notifications as read"
            onClick={() => {
              notifications.forEach((notification) => onDismiss(notification.id))
              headingRef.current?.focus()
            }}
          />
        )}
        <IconButton icon={<X size={16} />} label="Close notifications" onClick={onClose} />
      </header>
      <div className="flex-1 overflow-y-auto p-2">
        {notifications.length === 0 ? (
          <div className="uam-np-empty h-full">
            <span className="uam-np-empty__icon"><CheckCircle2 size={18} aria-hidden /></span>
            <strong className="uam-np-empty__title">You’re all caught up</strong>
            <span className="uam-np-empty__text">Warnings and completed system actions will appear here.</span>
          </div>
        ) : (
          <div className="grid gap-1">
            {folderActionError && (
              <Notice
                tone="warning"
                title="Workspace action failed"
                dismissLabel="Dismiss workspace action error"
                onDismiss={() => setFolderActionError('')}
              >
                {folderActionError}
              </Notice>
            )}
            {confirmingFolder && (
              <Notice
                tone="warning"
                title="Remove workspace folder?"
                dismissLabel="Dismiss workspace removal warning"
                onDismiss={() => setConfirmingFolderId('')}
                actions={(
                  <>
                    <Button size="sm" onClick={() => setConfirmingFolderId('')}>Cancel</Button>
                    <Button size="sm" variant="danger" loading={busyAction === 'remove-folder'} onClick={() => {
                      setBusyAction('remove-folder')
                      setFolderActionError('')
                      void deleteFolder(confirmingFolder.id).then((deleted) => {
                        if (deleted) setConfirmingFolderId('')
                        else setFolderActionError('The workspace could not be removed. Finish active work and try again.')
                      }).catch(() => {
                        setFolderActionError('The workspace could not be removed. Finish active work and try again.')
                      }).finally(() => setBusyAction(''))
                    }}>Confirm removal</Button>
                  </>
                )}
              >
                Remove “{confirmingFolder.name}” from Universal Agent Manager and delete {confirmingChats}? The directory on disk will not be deleted.
              </Notice>
            )}
            {groups.flatMap((group) => [
                <div key={`group-${group.label}`} className="uam-np-group">{group.label}</div>,
                ...group.items.map((notification) => {
                  const tone = notification.failure ? 'error' : notification.warning ? 'warning' : 'info'
                  const ToneIcon = tone === 'error' ? XCircle : tone === 'warning' ? AlertTriangle : Info
                  const time = new Date(notification.time)
                  const exactTime = time.toLocaleString()
                  const relativeTime = relativeNotificationTime(notification.time, now)
                  return (
                    <div key={notification.id} className="uam-np-row">
                      <span className="uam-np-row__icon" data-tone={tone} aria-hidden><ToneIcon size={14} /></span>
                      <div className="min-w-0">
                        <div className="uam-np-row__title">
                          <span>{notification.title}</span>
                          <span className="uam-np-dot" aria-hidden />
                        </div>
                        <div className="uam-np-row__meta">
                          <Tooltip label={exactTime}>
                            <time tabIndex={0} className="rounded-sm" dateTime={time.toISOString()} aria-label={relativeTime}>{relativeTime}</time>
                          </Tooltip>
                        </div>
                        {notification.failure && <div className="uam-np-row__detail" style={{ color: 'var(--text-2)' }}>{notification.failure.message}</div>}
                        <div className="uam-np-row__detail" style={{ overflowWrap: 'anywhere' }}>{notification.detail}</div>
                        {notification.actions && (
                          <div className="mt-2 flex flex-wrap gap-2">
                            {notification.actions.map((action) => {
                              const actionId = `${notification.id}-${action.label}`
                              return (
                                <Button
                                  key={action.label}
                                  size="sm"
                                  variant={action.variant}
                                  loading={busyAction === actionId}
                                  disabled={Boolean(busyAction)}
                                  onClick={() => { void runAction(notification.id, action) }}
                                >
                                  {action.label}
                                </Button>
                              )
                            })}
                          </div>
                        )}
                      </div>
                      <IconButton
                        size="sm"
                        className="uam-np-row__dismiss"
                        icon={<X size={14} />}
                        label={`Dismiss ${notification.title}`}
                        onClick={() => {
                          onDismiss(notification.id)
                          headingRef.current?.focus()
                        }}
                      />
                    </div>
                  )
                }),
            ])}
          </div>
        )}
      </div>
    </aside>
  )
}

function RightActivityRail({ alertCount, alertsOpen, monitor, updatesOpen, onToggleCommit, onToggleAlerts, onToggleUpdates, notificationDescriptionId }: { notificationDescriptionId?: string; alertCount: number; alertsOpen: boolean; monitor: UpdateMonitor; updatesOpen: boolean; onToggleCommit: () => void; onToggleAlerts: () => void; onToggleUpdates: () => void }) {
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const getChatWorktreeStatus = useAppStore((s) => s.getChatWorktreeStatus)
  const [vcsKind, setVcsKind] = useState<'git' | 'svn' | null>(null)

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
  const runtimeUpdateCount = monitor.updates.filter((update) => update.providerId || (SSH_ENABLED && update.remoteHostId)).length
  return (
    <aside className="uam-side-rail uam-side-rail--right" aria-label="Tool windows">
      <IconButton
        icon={vcsIcon}
        label={commitPanelOpen ? `Close ${vcsLabelKind} commit panel` : `Open ${vcsLabelKind} commit panel`}
        tooltipSide="left"
        active={commitPanelOpen}
        onClick={onToggleCommit}
      />
      <div className="uam-side-rail__spacer" />
      <span className="relative inline-flex">
        <IconButton
          icon={<Bell size={17} />}
          label={alertCount > 0 ? `${alertCount} alert${alertCount === 1 ? '' : 's'}` : 'No new alerts'}
          data-notification-bell=""
          aria-describedby={notificationDescriptionId}
          tooltipSide="left"
          active={alertsOpen}
          onClick={onToggleAlerts}
        />
        {alertCount > 0 && (
          <span className="absolute -right-0.5 -top-0.5 pointer-events-none" aria-hidden>
            <StatusDot tone="warning" size={7} />
          </span>
        )}
      </span>
      <span className="relative inline-flex">
        <IconButton
          icon={<ArrowUpCircle size={17} />}
          label={updateCount > 0 ? `${updateCount} update${updateCount === 1 ? '' : 's'} available` : 'Check for updates'}
          tooltipSide="left"
          active={updatesOpen}
          onClick={onToggleUpdates}
        />
        {runtimeUpdateCount > 0 && (
          <span className="absolute -right-1.5 -top-1.5 pointer-events-none min-w-4 rounded-full px-1 text-center text-[10px] font-bold leading-4" style={{ background: 'var(--accent)', color: 'var(--bg)' }} aria-hidden>
            {runtimeUpdateCount}
          </span>
        )}
      </span>
    </aside>
  )
}

export function AppShell() {
  const windowChrome = useWindowChrome()
  const settingsRef = useRef<SettingsHandle>(null)
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const refreshCustomThemes = useAppStore((state) => state.refreshCustomThemes)
  const isNewChatModalOpen = useAppStore((s) => s.isNewChatModalOpen)
  const isSettingsOpen = useAppStore((s) => s.isSettingsOpen)
  const [usageOpen, setUsageOpen] = useState(false)
  const memoryLibraryScope = useAppStore((s) => s.memoryLibraryScope)
  const isMemoryScanModalOpen = useAppStore((s) => s.isMemoryScanModalOpen)
  const isMarkdownStoreOpen = useAppStore((s) => s.isMarkdownStoreOpen)
  const sidebarCollapsed = useAppStore((s) => s.sidebarCollapsed)
  const commitPanelOpen = useAppStore((s) => s.commitPanelOpen)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const sidebarWidthPx = useAppStore((s) => s.sidebarWidthPx)
  const commitPanelWidthPx = useAppStore((s) => s.commitPanelWidthPx)
  const setSidebarWidthPx = useAppStore((s) => s.setSidebarWidthPx)
  const setCommitPanelWidthPx = useAppStore((s) => s.setCommitPanelWidthPx)
  const folders = useAppStore((s) => s.folders)
  const connectionIssueSignature = useAppStore(remoteConnectionIssueSignature)
  const shellActionNotification = useAppStore((s) => s.shellActionNotification)
  const statusLine = useAppStore((s) => s.statusLine)
  const dismissShellActionNotification = useAppStore((s) => s.dismissShellActionNotification)
  const updateMonitor = useUpdateMonitor()
  const [alertsOpen, setAlertsOpen] = useState(false)
  const [appFailures, setAppFailures] = useState<AppFailure[]>([])
  const [transientAlert, setTransientAlert] = useState<TransientAlert | null>(null)
  const [alertHeld, setAlertHeld] = useState(false)
  const [alertFallback, setAlertFallback] = useState(false)
  const alertFallbackId = useId()

  useEffect(() => {
    const onFailure = (event: Event) => {
      const failure = (event as CustomEvent<AppFailure>).detail
      setAppFailures((history) => [...history, failure])
      setTransientAlert({ id: failure.id, tone: 'error', message: failure.message, actionLabel: 'View in notifications', action: 'notifications' })
    }
    window.addEventListener(APP_FAILURE_EVENT, onFailure)
    return () => window.removeEventListener(APP_FAILURE_EVENT, onFailure)
  }, [])

  useEffect(() => {
    if (!transientAlert || alertHeld) return
    const timer = window.setTimeout(() => setTransientAlert(null), ALERT_EXPIRY_MS)
    return () => window.clearTimeout(timer)
  }, [transientAlert, alertHeld])
  const [dismissedNotificationIds, setDismissedNotificationIds] = useState<Set<string>>(() => new Set())
  const [updatesOpen, setUpdatesOpen] = useState(false)
  const updateNoticeId = updateMonitor.updates.length > 0 ? updateNotificationId(updateMonitor.updates) : ''
  const connectionIssues = useMemo(() => {
    const state = useAppStore.getState()
    return remoteConnectionIssues(
      state.executionHosts,
      state.sessions,
      state.acpBindingBySessionId,
      state.cliBindingBySessionId,
    )
  }, [connectionIssueSignature])
  const previousConnectionIssues = useRef(connectionIssues)
  const [connectionRecoveries, setConnectionRecoveries] = useState<RemoteConnectionNotification[]>([])
  const [viewportWidth, setViewportWidth] = useState(() => window.innerWidth)
  const activeNotificationIds = [
    ...appFailures.map((failure) => failure.id),
    ...(updateNoticeId ? [updateNoticeId] : []),
    ...(statusLine ? [statusLineNotificationId(statusLine)] : []),
    ...(shellActionNotification ? [shellActionNotificationId(shellActionNotification)] : []),
    ...folders.filter((folder) => folder.missing).map(missingFolderNotificationId),
    ...connectionIssues.map((notification) => notification.id),
    ...connectionRecoveries.map((notification) => notification.id),
    ...updateMonitor.providerUpdateResults.filter((result) => result.status === 'succeeded').map(providerUpdateNotificationId),
  ]
  const recordedFailureTimes = new Map(appFailures.map((failure) => [failure.id, failure.time]))
  const notificationTimes = useNotificationTimes(activeNotificationIds.map((id) => ({ id, time: recordedFailureTimes.get(id) })))
  const alertCount = activeNotificationIds.filter((id) => !dismissedNotificationIds.has(id)).length

  useEffect(() => {
    void refreshCustomThemes()
  }, [refreshCustomThemes])

  useEffect(() => {
    const updateViewportWidth = () => setViewportWidth(window.innerWidth)
    window.addEventListener('resize', updateViewportWidth)
    return () => window.removeEventListener('resize', updateViewportWidth)
  }, [])

  useEffect(() => {
    const currentKeys = new Set(connectionIssues.map((issue) => issue.connectionKey))
    const recovered = previousConnectionIssues.current.filter((issue) => !currentKeys.has(issue.connectionKey))
    if (recovered.length > 0) {
      const now = Date.now()
      setConnectionRecoveries((current) => [
        ...current.filter((notification) => !recovered.some((issue) => issue.connectionKey === notification.connectionKey)),
        ...recovered.map((issue, index) => ({
          id: `remote-reconnected-${issue.connectionKey}-${now + index}`,
          connectionKey: issue.connectionKey,
          hostLabel: issue.hostLabel,
          title: `${issue.hostLabel} reconnected`,
          detail: 'The remote connection is available again.',
        })),
      ])
    }
    previousConnectionIssues.current = connectionIssues
  }, [connectionIssues])

  useEffect(() => {
    if (updateMonitor.updates.length === 0) return
    const count = updateMonitor.updates.length
    setTransientAlert({
      id: updateNoticeId,
      tone: 'info',
      message: `${count} update${count === 1 ? '' : 's'} available · ${updateSummaryText(updateMonitor.updates)}`,
      actionLabel: 'Review updates',
      action: 'updates',
    })
    // Only a new set of updates should raise the transient alert again.
  }, [updateNoticeId])

  useEffect(() => {
    const activeIds = new Set([
      ...appFailures.map((failure) => failure.id),
      ...(updateNoticeId ? [updateNoticeId] : []),
      ...(statusLine ? [statusLineNotificationId(statusLine)] : []),
      ...(shellActionNotification ? [shellActionNotificationId(shellActionNotification)] : []),
      ...folders.filter((folder) => folder.missing).map(missingFolderNotificationId),
      ...connectionIssues.map((notification) => notification.id),
      ...connectionRecoveries.map((notification) => notification.id),
      ...updateMonitor.providerUpdateResults.filter((result) => result.status === 'succeeded').map(providerUpdateNotificationId),
    ])
    setDismissedNotificationIds((dismissed) => {
      const currentIds = [...dismissed]
      const remainingIds = currentIds.filter((id) => activeIds.has(id))
      return remainingIds.length === currentIds.length ? dismissed : new Set(remainingIds)
    })
  }, [appFailures, connectionIssues, connectionRecoveries, folders, shellActionNotification, statusLine, updateMonitor.providerUpdateResults, updateNoticeId])

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

  const resizeFromKeyboard = useCallback((side: 'sidebar' | 'commit', event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return
    event.preventDefault()
    const minimum = side === 'sidebar' ? SIDEBAR_WIDTH_MIN : COMMIT_PANEL_WIDTH_MIN
    const maximum = side === 'sidebar' ? SIDEBAR_WIDTH_MAX : COMMIT_PANEL_WIDTH_MAX
    const current = side === 'sidebar' ? sidebarWidthPx : commitPanelWidthPx
    const direction = side === 'sidebar' ? 1 : -1
    const next = event.key === 'Home' ? minimum
      : event.key === 'End' ? maximum
        : current + (event.key === 'ArrowRight' ? direction : -direction) * KEYBOARD_RESIZE_STEP
    if (side === 'sidebar') setSidebarWidthPx(next)
    else setCommitPanelWidthPx(next)
  }, [commitPanelWidthPx, setCommitPanelWidthPx, setSidebarWidthPx, sidebarWidthPx])

  const rightPanelWidth = commitPanelOpen
    ? commitPanelWidthPx
    : alertsOpen || updatesOpen
      ? FIXED_RIGHT_PANEL_WIDTH
      : 0
  const resizeHandlesWidth = (sidebarCollapsed ? 0 : RESIZE_HANDLE_WIDTH) + (commitPanelOpen ? RESIZE_HANDLE_WIDTH : 0)
  const sidebarWouldStarveChat = !sidebarCollapsed && rightPanelWidth > 0 &&
    viewportWidth - ACTIVITY_RAILS_WIDTH - resizeHandlesWidth - sidebarWidthPx - rightPanelWidth < MIN_CHAT_WIDTH

  const sidebarPresence = usePresence(!sidebarCollapsed && !sidebarWouldStarveChat)
  const commitPresence = usePresence(commitPanelOpen)
  const alertsPresence = usePresence(alertsOpen)
  const updatesPresence = usePresence(updatesOpen)
  // Any open right-side panel sits beside the rail; the alert would cover it. The bell keeps the message and unread count.
  const alertSuppressed = transientAlert !== null && (alertsOpen || updatesOpen || commitPanelOpen)
  useEffect(() => { if (alertSuppressed) setTransientAlert(null) }, [alertSuppressed])

  const toggleSettings = () => {
    if (usageOpen) { setUsageOpen(false); if (!isSettingsOpen) setSettingsOpen(true); return }
    if (isSettingsOpen && settingsRef.current) settingsRef.current.requestClose()
    else setSettingsOpen(!isSettingsOpen)
  }

  const openNotifications = () => {
    setAlertsOpen(true)
    setCommitPanelOpen(false)
    setUpdatesOpen(false)
  }
  const openUpdates = () => {
    setUpdatesOpen(true)
    setCommitPanelOpen(false)
    setAlertsOpen(false)
  }

  const shortcutStateRef = useRef({ isSettingsOpen, usageOpen, sidebarCollapsed, sidebarWouldStarveChat, toggleSettings })
  shortcutStateRef.current = { isSettingsOpen, usageOpen, sidebarCollapsed, sidebarWouldStarveChat, toggleSettings }

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      const shortcut = matchShortcut(event)
      if (!shortcut || event.defaultPrevented) return
      // Modals own the keyboard while open.
      if (document.querySelector('[aria-modal="true"]')) return
      const state = shortcutStateRef.current
      if (shortcut === 'focusActiveInput') {
        if (!state.isSettingsOpen && !state.usageOpen && focusActiveInput()) event.preventDefault()
        return
      }
      event.preventDefault()
      const store = useAppStore.getState()
      if (shortcut === 'newChat') {
        setUsageOpen(false)
        store.setNewChatModalOpen(true)
      } else if (shortcut === 'settings') {
        state.toggleSettings()
      } else if (shortcut === 'sidebar') {
        if (!state.isSettingsOpen && !state.usageOpen) store.setSidebarCollapsed(!state.sidebarCollapsed)
      } else if (shortcut === 'nextAttention') {
        const rows = Array.from(document.querySelectorAll<HTMLElement>('[data-session-id]'))
          .filter((row) => !row.closest('[hidden], [inert], [aria-hidden="true"]'))
          .map((row) => row.dataset.sessionId ?? '')
        const next = nextAttentionSession(store.sessions, store.acpBindingBySessionId, store.activeSessionId, rows)
        if (next) store.setActiveSession(next)
      } else if (shortcut === 'search') {
        if (state.isSettingsOpen) {
          settingsRef.current?.requestClose()
          if (useAppStore.getState().isSettingsOpen) return
        }
        setUsageOpen(false)
        if (state.sidebarCollapsed) store.setSidebarCollapsed(false)
        if (state.sidebarWouldStarveChat) {
          store.setCommitPanelOpen(false)
          setAlertsOpen(false)
          setUpdatesOpen(false)
        }
        window.requestAnimationFrame(() => {
          const input = document.querySelector<HTMLInputElement>('[data-chat-search-input]')
          input?.focus()
          input?.select()
        })
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [setSettingsOpen])

  return (
    <div
      className="h-screen w-screen overflow-hidden flex uam-app"
      data-right-panel-open={commitPanelOpen || alertsOpen || updatesOpen ? 'true' : 'false'}
      data-window-platform={windowChrome.state?.platform}
      data-window-fullscreen={windowChrome.state?.fullscreen ? 'true' : undefined}
      style={{ color: 'var(--text)' }}
    >
      {windowChrome.state && <WindowChrome state={windowChrome.state} command={windowChrome.command} />}
      <LeftActivityRail
        settingsOpen={isSettingsOpen && !usageOpen}
        usageOpen={usageOpen}
        onToggleUsage={() => setUsageOpen(value => !value)}
        onToggleSettings={toggleSettings}
      />

      {/* Keep chat components mounted while Settings occupies the middle region. */}
      <div data-testid="chat-region" hidden={isSettingsOpen || usageOpen} className="min-w-0 flex-1 h-full" style={{display: isSettingsOpen || usageOpen ? 'none' : 'flex'}}>

      {sidebarPresence.mounted && (
        <>
          <aside
            className="uam-shell-panel uam-shell-panel--left flex h-full flex-col overflow-hidden"
            data-testid="chat-selector-panel"
            data-state={sidebarPresence.closing ? 'closed' : 'open'}
            style={{ width: sidebarWidthPx, flex: `0 0 ${sidebarWidthPx}px`, background: 'var(--sidebar-bg)', '--panel-w': `${sidebarWidthPx}px` } as React.CSSProperties}
          >
            <ChatSidebar />
          </aside>
          {!sidebarPresence.closing && <div
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize chat selector"
            aria-valuemin={SIDEBAR_WIDTH_MIN}
            aria-valuemax={SIDEBAR_WIDTH_MAX}
            aria-valuenow={sidebarWidthPx}
            tabIndex={0}
            className="uam-resize-handle"
            onMouseDown={(event) => startResize('sidebar', event)}
            onKeyDown={(event) => resizeFromKeyboard('sidebar', event)}
          />}
        </>
      )}

      <main className="min-w-0 flex-1 overflow-hidden flex flex-col" style={{ background: 'var(--bg)' }}>
        <ProviderMcpWarning />
        <div className="min-h-0 flex-1"><ChatPanel /></div>
      </main>
      </div>
      {isSettingsOpen && <main hidden={usageOpen} style={usageOpen ? { display: 'none' } : undefined} className="min-w-0 min-h-0 flex-1 overflow-hidden" aria-label="Settings workspace"><InterfaceErrorBoundary panel onDismiss={() => useAppStore.getState().setSettingsOpen(false)}><Suspense fallback={<div className="uam-settings-scroll"><div className="uam-settings-frame" style={{ maxWidth: 720 }}><SettingsLoading rows={4} /><SettingsLoading rows={2} /></div></div>}><SettingsModal ref={settingsRef}/></Suspense></InterfaceErrorBoundary></main>}
      {usageOpen && <InterfaceErrorBoundary panel onDismiss={() => setUsageOpen(false)}><Suspense fallback={<div role="status" className="min-w-0 flex-1 p-4 text-xs">Loading usage…</div>}><UsagePage onClose={() => { setUsageOpen(false); window.requestAnimationFrame(() => document.querySelector<HTMLButtonElement>('[aria-label="Usage"]')?.focus()) }} /></Suspense></InterfaceErrorBoundary>}

      {commitPresence.mounted && (
        <>
          {!commitPresence.closing && <div
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize Git/SVN commit panel"
            aria-valuemin={COMMIT_PANEL_WIDTH_MIN}
            aria-valuemax={COMMIT_PANEL_WIDTH_MAX}
            aria-valuenow={commitPanelWidthPx}
            tabIndex={0}
            className="uam-resize-handle"
            onMouseDown={(event) => startResize('commit', event)}
            onKeyDown={(event) => resizeFromKeyboard('commit', event)}
          />}
          <aside
            className="uam-shell-panel uam-shell-panel--right flex h-full flex-col overflow-hidden"
            data-testid="commit-panel"
            data-state={commitPresence.closing ? 'closed' : 'open'}
            style={{ width: commitPanelWidthPx, flex: `0 0 ${commitPanelWidthPx}px`, background: 'var(--surface)', '--panel-w': `${commitPanelWidthPx}px` } as React.CSSProperties}
          >
            <InterfaceErrorBoundary panel onDismiss={() => setCommitPanelOpen(false)}><Suspense fallback={null}><VcsCommitPanel /></Suspense></InterfaceErrorBoundary>
          </aside>
        </>
      )}

      {updatesPresence.mounted && <UpdatesPanel monitor={updateMonitor} closing={updatesPresence.closing} onClose={() => setUpdatesOpen(false)} />}

      {alertsPresence.mounted && (
        <NotificationsPanel
          closing={alertsPresence.closing}
          dismissedNotificationIds={dismissedNotificationIds}
          notificationTimes={notificationTimes}
          remoteNotifications={[...connectionIssues, ...connectionRecoveries]}
          failures={appFailures}
          updates={updateMonitor.updates}
          providerUpdateResults={updateMonitor.providerUpdateResults}
          onClose={() => setAlertsOpen(false)}
          onReviewUpdates={openUpdates}
          onDismiss={(id) => {
            if (shellActionNotification && id === shellActionNotificationId(shellActionNotification)) {
              void dismissShellActionNotification()
              return
            }
            setTransientAlert((alert) => alert?.id === id ? null : alert)
            setDismissedNotificationIds((dismissed) => new Set(dismissed).add(id))
          }}
        />
      )}

      <RightActivityRail
        notificationDescriptionId={alertFallback ? alertFallbackId : undefined}
        alertCount={alertCount}
        alertsOpen={alertsOpen}
        monitor={updateMonitor}
        updatesOpen={updatesOpen}
        onToggleCommit={() => {
          setCommitPanelOpen(!commitPanelOpen)
          setAlertsOpen(false)
          setUpdatesOpen(false)
        }}
        onToggleAlerts={() => {
          setAlertsOpen((open) => !open)
          setCommitPanelOpen(false)
          setUpdatesOpen(false)
        }}
        onToggleUpdates={() => {
          setUpdatesOpen((open) => !open)
          setCommitPanelOpen(false)
          setAlertsOpen(false)
        }}
      />

      <StartupNativeImportChooser />
      <TransientAlertSurface alert={alertSuppressed ? null : transientAlert} notice={transientAlert} fallbackId={alertFallbackId} onFallbackChange={setAlertFallback} onHeldChange={setAlertHeld} onAction={(action) => action === 'updates' ? openUpdates() : openNotifications()} onDismiss={() => setTransientAlert(null)} />

      {/* Modals */}
      <Presence open={isNewChatModalOpen}><InterfaceErrorBoundary panel onDismiss={() => useAppStore.getState().setNewChatModalOpen(false)}><Suspense fallback={null}><NewChatModal /></Suspense></InterfaceErrorBoundary></Presence>
      <Presence open={Boolean(memoryLibraryScope) && !isSettingsOpen}><InterfaceErrorBoundary panel onDismiss={() => useAppStore.getState().closeMemoryLibrary()}><Suspense fallback={null}><MemoryLibraryModal /></Suspense></InterfaceErrorBoundary></Presence>
      <Presence open={isMemoryScanModalOpen}><InterfaceErrorBoundary panel onDismiss={() => useAppStore.getState().closeMemoryScanModal()}><Suspense fallback={null}><MemoryScanModal /></Suspense></InterfaceErrorBoundary></Presence>
      <Presence open={isMarkdownStoreOpen}><InterfaceErrorBoundary panel onDismiss={() => useAppStore.getState().closeMarkdownStore()}><Suspense fallback={null}><MarkdownStoreModal /></Suspense></InterfaceErrorBoundary></Presence>
    </div>
  )
}
