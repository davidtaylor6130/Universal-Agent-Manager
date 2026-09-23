import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { ArrowLeft, Activity, MessageSquare, RefreshCw, Settings, Pin, PinOff, CircleAlert, Check, ChevronRight, Plus } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import type { CppAppState, CppCompanionUnchangedState } from '../../store/cpp/types'
import { ChatView } from '../views/ChatView'
import { FolderTree } from '../sidebar/FolderTree'
import { sidebarStatusIcon } from '../sidebar/SessionItem'
import { displayedChatStatus } from '../sidebar/chatSearch'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton } from '../ui'
import './companion.css'

const TOKEN_KEY = 'uam-companion-token'
const NewChatModal = lazy(() => import('../sidebar/NewChatModal').then(({ NewChatModal: Modal }) => ({ default: Modal })))

/** The companion uses the desktop store and transcript, with phone-local navigation. */
export function CompanionShell() {
  const [token, setToken] = useState(() => {
    const saved = window.localStorage.getItem(TOKEN_KEY) ?? sessionStorage.getItem(TOKEN_KEY) ?? ''
    if (saved) window.localStorage.setItem(TOKEN_KEY, saved)
    sessionStorage.removeItem(TOKEN_KEY)
    return saved
  })
  const [tokenInput, setTokenInput] = useState('')
  const [ready, setReady] = useState(false)
  const [error, setError] = useState('')
  const [tab, setTab] = useState<'activity' | 'chats' | 'settings'>('activity')
  const [conversationOpen, setConversationOpen] = useState(false)
  const [search, setSearch] = useState('')
  const [projectFilter, setProjectFilter] = useState('all')
  const [lastRefreshedAt, setLastRefreshedAt] = useState<Date | null>(null)
  const [refreshing, setRefreshing] = useState(false)
  const [pinning, setPinning] = useState(false)
  const [pinError, setPinError] = useState('')
  useEffect(() => {
    const manifest = document.createElement('link')
    manifest.rel = 'manifest'
    manifest.href = '/companion.webmanifest'
    const icon = document.createElement('link')
    icon.rel = 'apple-touch-icon'
    icon.href = '/app_icon-180.png'
    document.head.append(manifest, icon)
    return () => { manifest.remove(); icon.remove() }
  }, [])
  const polling = useRef(false)
  const refreshQueued = useRef(false)
  const queuedForceFull = useRef(false)
  const queuedExplicitRefresh = useRef(false)
  const refreshNow = useRef<((forceFull?: boolean, explicit?: boolean) => Promise<void>) | null>(null)
  const companionBootId = useRef('')
  const companionStateRevision = useRef<number | null>(null)
  const automaticPollCount = useRef(0)
  const initialTabDecided = useRef(false)
  const manualTabChoice = useRef(false)
  const sessions = useAppStore((s) => s.sessions)
  const folders = useAppStore((s) => s.folders)
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const storeNewChatModalOpen = useAppStore((s) => s.isNewChatModalOpen)
  const setStoreNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const acpBindings = useAppStore((s) => s.acpBindingBySessionId)
  const cliBindings = useAppStore((s) => s.cliBindingBySessionId)
  const session = sessions.find((candidate) => candidate.id === activeSessionId)
  const currentStatus = session ? displayedChatStatus(
    cliBindings[session.id] ? [cliBindings[session.id]] : [],
    acpBindings[session.id] ? [acpBindings[session.id]] : [],
  ) : null
  const refreshManually = async () => {
    setRefreshing(true)
    try { await refreshNow.current?.(true, true) } finally { setRefreshing(false) }
  }
  const togglePin = async () => {
    if (!session || pinning) return
    setPinning(true)
    setPinError('')
    try {
      if (!await useAppStore.getState().setSessionPinned(session.id, !session.isPinned)) setPinError('Could not update pin. Try again.')
    } catch { setPinError('Could not update pin. Try again.') }
    finally { setPinning(false) }
  }
  const statusFor = (candidate: typeof sessions[number]) => displayedChatStatus(
    cliBindings[candidate.id] ? [cliBindings[candidate.id]] : [],
    acpBindings[candidate.id] ? [acpBindings[candidate.id]] : [],
  )
  const workspaceLabel = (candidate: typeof sessions[number]) => {
    const directory = candidate.workspaceDirectory?.trim() || ''
    return directory.split(/[\\/]/).filter(Boolean).pop() || 'Local workspace'
  }
  const projectLabel = (candidate: typeof sessions[number]) => {
    const folder = folders.find((item) => item.id === candidate.folderId)
    return folder?.name || 'Unsorted'
  }
  const projectKey = (candidate: typeof sessions[number]) => folders.some((folder) => folder.id === candidate.folderId) ? candidate.folderId : 'unsorted'
  const visibleSessions = sessions.filter((candidate) => projectFilter === 'all' || projectKey(candidate) === projectFilter)
  const activeSessions = visibleSessions.filter((candidate) => {
    const status = statusFor(candidate)
    return status?.type === 'processing' || status?.type === 'attention'
  })
  const recentSessions = visibleSessions.filter((candidate) => statusFor(candidate)?.type === 'done')
  const activityRow = (candidate: typeof sessions[number]) => {
    const status = statusFor(candidate)
    const statusLabel = status?.type === 'processing' ? 'Running' : status?.type === 'attention' ? 'Needs input' : 'Ready to review'
    const updatedAt = candidate.updatedAt ? new Date(candidate.updatedAt) : null
    const timestamp = updatedAt && !Number.isNaN(updatedAt.getTime())
      ? updatedAt.toLocaleDateString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })
      : 'Time unavailable'
    const attentionIcon = status?.type === 'attention' ? sidebarStatusIcon(status.kind, 14) : null
    const statusIcon = status?.type === 'processing'
      ? <span aria-hidden="true" />
      : status?.type === 'attention'
        ? attentionIcon
        : <Check size={14} aria-hidden="true" />
    return <button key={candidate.id} type="button" className="uam-companion-activity-row" data-session-id={candidate.id} aria-label={`Open ${candidate.name}`} onClick={() => { useAppStore.getState().setActiveSession?.(candidate.id); setConversationOpen(true) }}>
      <ProviderLogo providerId={candidate.providerId} size={24} className="uam-companion-provider" />
      <span className="uam-companion-row-copy"><strong>{candidate.name}</strong><small>{projectLabel(candidate)} · {workspaceLabel(candidate)}</small></span>
      <span className="uam-companion-row-state"><em className={`session-status session-status--${status?.type === 'processing' ? 'processing' : status?.type === 'attention' ? 'attention' : 'idle'}`} aria-label={statusLabel}>{statusIcon}</em><small className="uam-companion-review-status">{status?.type === 'done' ? 'Ready to review' : statusLabel}</small><small>{timestamp}</small></span>
      <ChevronRight size={18} aria-hidden />
    </button>
  }


  useEffect(() => {
    if (!token) return
    initialTabDecided.current = false
    manualTabChoice.current = false
    let cancelled = false
    const refresh = async (forceFull = false, explicit = false) => {
      if (cancelled || document.visibilityState === 'hidden') return
      if (polling.current) {
        refreshQueued.current = true
        queuedForceFull.current = queuedForceFull.current || forceFull
        queuedExplicitRefresh.current = queuedExplicitRefresh.current || explicit
        return
      }
      polling.current = true
      try {
        const fullRefresh = forceFull || automaticPollCount.current % 8 === 0
        automaticPollCount.current += 1
        const knownState = fullRefresh || !companionBootId.current || companionStateRevision.current === null
          ? {}
          : { knownBootId: companionBootId.current, knownStateRevision: companionStateRevision.current }
        const response = await sendToCEF<CppAppState | CppCompanionUnchangedState>({
          action: 'getInitialState',
          ...(Object.keys(knownState).length ? { payload: knownState } : {}),
        })
        if (cancelled) return
        if (!response.ok || !response.data) throw new Error(response.error || 'Could not connect to UAM.')
        const state = response.data
        if ('unchanged' in state && state.unchanged === true) {
          companionBootId.current = state.bootId
          companionStateRevision.current = state.stateRevision
          const current = useAppStore.getState()
          const selectedId = current.activeSessionId
          const selectedMessages = selectedId ? current.messages?.[selectedId] : undefined
          const selectedAcpBinding = selectedId ? current.acpBindingBySessionId?.[selectedId] : undefined
          const selectedCliBinding = selectedId ? current.cliBindingBySessionId?.[selectedId] : undefined
          const selectedTranscriptNeedsRefresh = selectedId && current.sessions.some((candidate) => candidate.id === selectedId) &&
            (explicit || selectedMessages === undefined || Boolean(
              selectedAcpBinding?.processing || selectedCliBinding?.processing || selectedMessages.at(-1)?.isStreaming
            ))
          // Stream tokens can change without changing the chat-list revision.
          if (selectedTranscriptNeedsRefresh && selectedId) {
            if (await current.loadSessionMessages(selectedId) === false) {
              throw new Error(useAppStore.getState().statusLine || 'Could not refresh chat history.')
            }
          }
          setLastRefreshedAt(new Date())
          setReady(true)
          setError('')
          return
        }
        const phoneState = useAppStore.getState()
        const selectedId = phoneState.activeSessionId
        const previousBootId = companionBootId.current
        const previousRevision = companionStateRevision.current
        const nextBootId = state.bootId ?? ''
        const nextRevision = typeof state.stateRevision === 'number' ? state.stateRevision : 0
        const bootChanged = Boolean(previousBootId && nextBootId && previousBootId !== nextBootId)
        const stateChanged = bootChanged || previousRevision === null || previousRevision !== nextRevision
        companionBootId.current = nextBootId
        companionStateRevision.current = nextRevision
        if (bootChanged || (fullRefresh && nextRevision <= phoneState.lastAppliedStateRevision) ||
            (previousRevision === null && nextRevision <= phoneState.lastAppliedStateRevision) ||
            (stateChanged && nextRevision < phoneState.lastAppliedStateRevision)) {
          useAppStore.setState({ lastAppliedStateRevision: -1 })
        }
        phoneState.loadFromCef({
          ...state,
          selectedChatId: selectedId,
          selectedChatIndex: -1,
          folders: state.folders.map((folder) => {
            const local = phoneState.folders.find((candidate) => candidate.id === folder.id)
            return local ? { ...folder, collapsed: !local.isExpanded } : folder
          }),
          resourceCollections: state.resourceCollections?.map((collection) => {
            const local = phoneState.resourceCollections.find((candidate) => candidate.id === collection.id)
            return local ? { ...collection, collapsed: local.collapsed } : collection
          }),
        })
        const current = useAppStore.getState()
        const selectedMessages = selectedId ? current.messages?.[selectedId] : undefined
        const selectedAcpBinding = selectedId ? current.acpBindingBySessionId?.[selectedId] : undefined
        const selectedCliBinding = selectedId ? current.cliBindingBySessionId?.[selectedId] : undefined
        const selectedTranscriptNeedsRefresh = selectedId && current.sessions.some((candidate) => candidate.id === selectedId) &&
          (explicit || selectedMessages === undefined || Boolean(
            selectedAcpBinding?.processing || selectedCliBinding?.processing || selectedMessages.at(-1)?.isStreaming
          ))
        if (selectedTranscriptNeedsRefresh && selectedId) {
          if (await current.loadSessionMessages(selectedId) === false) {
            throw new Error(useAppStore.getState().statusLine || 'Could not refresh chat history.')
          }
        }
        if (cancelled) return
        const refreshed = useAppStore.getState()
        if (!initialTabDecided.current) {
          initialTabDecided.current = true
          const hasActivity = refreshed.sessions.some((candidate) => displayedChatStatus(
            refreshed.cliBindingBySessionId[candidate.id] ? [refreshed.cliBindingBySessionId[candidate.id]] : [],
            refreshed.acpBindingBySessionId[candidate.id] ? [refreshed.acpBindingBySessionId[candidate.id]] : [],
          ))
          if (!manualTabChoice.current) setTab(hasActivity ? 'activity' : 'chats')
        }
        setLastRefreshedAt(new Date())
        setReady(true)
        setError('')
      } catch (failure) {
        if (!cancelled) setError(failure instanceof Error ? failure.message : 'Could not connect to UAM.')
      } finally {
        polling.current = false
        if (refreshQueued.current && !cancelled) {
          const forceQueuedRefresh = queuedForceFull.current
          const explicitQueuedRefresh = queuedExplicitRefresh.current
          refreshQueued.current = false
          queuedForceFull.current = false
          queuedExplicitRefresh.current = false
          await refresh(forceQueuedRefresh, explicitQueuedRefresh)
        }
      }
    }
    refreshNow.current = refresh
    void refresh(true)
    const timer = window.setInterval(() => void refresh(), 2000)
    const handleVisibilityChange = () => void refresh(true)
    document.addEventListener('visibilitychange', handleVisibilityChange)
    return () => {
      cancelled = true
      refreshQueued.current = false
      queuedForceFull.current = false
      queuedExplicitRefresh.current = false
      refreshNow.current = null
      window.clearInterval(timer)
      document.removeEventListener('visibilitychange', handleVisibilityChange)
    }
  }, [token])

  const disconnect = () => {
    window.localStorage.removeItem(TOKEN_KEY)
    sessionStorage.removeItem(TOKEN_KEY)
    setToken('')
    setReady(false)
    setError('')
    setConversationOpen(false)
    setPinError('')
    setTab('activity')
    useAppStore.setState({ activeSessionId: null, lastAppliedStateRevision: -1, sessions: [], folders: [], messages: {}, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  }

  return <div className="uam-companion uam-app">
    <header className="uam-companion-header">
      {conversationOpen && session && <IconButton icon={<ArrowLeft size={18} />} label="Back to chats" onClick={() => setConversationOpen(false)} />}
      {!conversationOpen && <span className="uam-companion-brand"><img src="/app_icon-180.png" alt="UAM" /><strong className="truncate">Universal Agent Manager</strong></span>}
      {conversationOpen && <strong className="truncate">{session?.name}</strong>}
      {conversationOpen && session && <>
        <IconButton icon={session.isPinned ? <PinOff size={16} /> : <Pin size={16} />} label={session.isPinned ? 'Unpin chat' : 'Pin chat'} active={Boolean(session.isPinned)} disabled={pinning} aria-busy={pinning || undefined} onClick={() => void togglePin()} />
        <span role="status" aria-label={currentStatus?.type === 'processing' ? 'Agent running' : currentStatus?.type === 'attention' ? `Needs attention: ${currentStatus.kind}` : currentStatus?.type === 'done' ? 'Done' : 'Idle'} className={`session-status session-status--${currentStatus?.type === 'processing' ? 'processing' : currentStatus?.type === 'attention' ? 'attention' : 'idle'}`}>
          {currentStatus?.type === 'attention' ? <CircleAlert size={16} /> : currentStatus?.type === 'done' ? <Check size={16} /> : <span />}
        </span>
      </>}
      {!conversationOpen && <IconButton icon={<Settings size={18} />} label="Settings" active={tab === 'settings'} onClick={() => { manualTabChoice.current = true; setTab('settings'); setPinError('') }} />}
    </header>
    {pinError && <div className="uam-companion-error" role="alert">{pinError}</div>}
    {error && <div className="uam-companion-error" role="alert">{error}</div>}
    {!token ? <form className="uam-companion-connect" onSubmit={(event) => {
      event.preventDefault()
      const nextToken = tokenInput.trim()
      if (!nextToken) return
      window.localStorage.setItem(TOKEN_KEY, nextToken)
      setToken(nextToken)
      setTokenInput('')
    }}>
      <label htmlFor="companion-token">Connection token</label>
      <input id="companion-token" type="password" autoComplete="off" value={tokenInput} onChange={(event) => setTokenInput(event.target.value)} required />
      <Button type="submit" variant="primary">Connect</Button>
    </form> : tab === 'settings' ? <main className="uam-companion-main uam-companion-settings">
      <h1>Settings</h1>
      <p role="status">{error ? 'Disconnected' : ready ? 'Connected' : 'Connecting…'}</p>
      <Button variant="secondary" disabled={refreshing} aria-busy={refreshing || undefined} onClick={() => void refreshManually()} leadingIcon={<RefreshCw size={16} />}>{refreshing ? 'Refreshing…' : 'Refresh chats and activity'}</Button>
      <Button variant="ghost" onClick={disconnect}>Logout</Button>
    </main> : ready ? <>
      <main className="uam-companion-main">
        {conversationOpen && session ? <ChatView key={session.id} session={session} /> : <div className="uam-companion-list"
          onClickCapture={(event) => {
            const target = event.target as Element
            if (target.closest('[data-session-id]') && !target.closest('button, input')) setConversationOpen(true)
          }}
          onKeyDownCapture={(event) => {
            if ((event.key === 'Enter' || event.key === ' ') && (event.target as Element).matches('[data-session-id]')) setConversationOpen(true)
          }}>
            <div className="flex items-center justify-between px-3 py-2">
              <h1 className="!p-0">{tab === 'activity' ? 'Activity' : 'Chats'}</h1>
            </div>
        {tab === 'activity' ? <>
          <div className="uam-companion-scope">
            <label className="uam-companion-scope-label" htmlFor="companion-project-filter">Project</label>
            <select id="companion-project-filter" aria-label="Filter activity by project" value={projectFilter} onChange={(event) => setProjectFilter(event.target.value)}>
              <option value="all">All projects</option>
              {folders.map((folder) => <option key={folder.id} value={folder.id}>{folder.name}</option>)}
              {sessions.some((candidate) => !candidate.folderId || !folders.some((folder) => folder.id === candidate.folderId)) && <option value="unsorted">Unsorted</option>}
            </select>
            <span className="uam-companion-refresh-stack">
              <button type="button" className="uam-companion-refresh" aria-label="Refresh activity" disabled={refreshing} onClick={() => void refreshManually()}><RefreshCw size={16} aria-hidden="true" /></button>
              <small role="status">{lastRefreshedAt ? `Updated ${lastRefreshedAt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : 'Not refreshed yet'}</small>
            </span>
          </div>
          <section className="uam-companion-section"><h2>Active</h2>{activeSessions.length ? activeSessions.map(activityRow) : <p className="uam-companion-empty">No active chats.</p>}</section>
          <section className="uam-companion-section"><h2>Recent output</h2>{recentSessions.length ? recentSessions.map(activityRow) : <p className="uam-companion-empty">No recent output.</p>}</section>
        </> : <>
            <input className="uam-companion-search" type="search" aria-label="Search chats" placeholder="Search chats" value={search} onChange={(event) => setSearch(event.target.value)} />
            <FolderTree searchQuery={search} />
          </>}
        </div>}
      </main>
      {storeNewChatModalOpen && <Suspense fallback={null}><NewChatModal companion onCreated={() => { setConversationOpen(true); void refreshNow.current?.(true, true) }} /></Suspense>}
    </> : <p className="p-4" role="status">{error ? 'Retrying connection…' : 'Loading chats…'}</p>}
    {token && <nav className="uam-companion-nav" aria-label="Companion navigation">
      <button type="button" aria-label="Activity" aria-current={tab === 'activity' && !conversationOpen ? 'page' : undefined} onClick={() => { manualTabChoice.current = true; setTab('activity'); setConversationOpen(false); setPinError('') }}><Activity size={21} /></button>
      <button type="button" className="uam-companion-new-chat" aria-label="New chat" onClick={() => setStoreNewChatModalOpen(true)}><Plus size={30} strokeWidth={2.5} /></button>
      <button type="button" aria-label="Chats" aria-current={tab === 'chats' && !conversationOpen ? 'page' : undefined} onClick={() => { manualTabChoice.current = true; setTab('chats'); setConversationOpen(false); setPinError('') }}><MessageSquare size={21} /></button>
      {tab !== 'settings' && <span className={`uam-companion-nav-indicator uam-companion-nav-indicator--${tab === 'chats' ? 'chats' : 'activity'}`} aria-hidden="true" />}
    </nav>}
  </div>
}
