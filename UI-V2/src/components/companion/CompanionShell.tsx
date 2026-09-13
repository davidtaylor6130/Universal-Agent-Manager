import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { ArrowLeft, Activity, MessageSquare, RefreshCw, Settings, Pin, PinOff, CircleAlert, Check } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import type { CppAppState } from '../../store/cpp/types'
import { ChatView } from '../views/ChatView'
import { FolderTree } from '../sidebar/FolderTree'
import { SessionItem } from '../sidebar/SessionItem'
import { displayedChatStatus } from '../sidebar/chatSearch'
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
  const refreshNow = useRef<(() => Promise<void>) | null>(null)
  const initialTabDecided = useRef(false)
  const manualTabChoice = useRef(false)
  const sessions = useAppStore((s) => s.sessions)
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
    try { await refreshNow.current?.() } finally { setRefreshing(false) }
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
  const activeSessions = sessions.filter((candidate) => displayedChatStatus(
    cliBindings[candidate.id] ? [cliBindings[candidate.id]] : [],
    acpBindings[candidate.id] ? [acpBindings[candidate.id]] : [],
  ))


  useEffect(() => {
    if (!token) return
    initialTabDecided.current = false
    manualTabChoice.current = false
    let cancelled = false
    const refresh = async () => {
      if (cancelled || polling.current || document.visibilityState === 'hidden') return
      polling.current = true
      try {
        const response = await sendToCEF<CppAppState>({ action: 'getInitialState' })
        if (cancelled) return
        if (!response.ok || !response.data) throw new Error(response.error || 'Could not connect to UAM.')
        const phoneState = useAppStore.getState()
        const selectedId = phoneState.activeSessionId
        // Serialized full snapshots remain authoritative even when a restarted desktop reuses a revision.
        if (typeof response.data.stateRevision === 'number' &&
            response.data.stateRevision <= phoneState.lastAppliedStateRevision) {
          useAppStore.setState({ lastAppliedStateRevision: -1 })
        }
        phoneState.loadFromCef({
          ...response.data,
          selectedChatId: selectedId,
          selectedChatIndex: -1,
          folders: response.data.folders.map((folder) => {
            const local = phoneState.folders.find((candidate) => candidate.id === folder.id)
            return local ? { ...folder, collapsed: !local.isExpanded } : folder
          }),
          resourceCollections: response.data.resourceCollections?.map((collection) => {
            const local = phoneState.resourceCollections.find((candidate) => candidate.id === collection.id)
            return local ? { ...collection, collapsed: local.collapsed } : collection
          }),
        })
        const current = useAppStore.getState()
        if (selectedId && current.sessions.some((candidate) => candidate.id === selectedId)) {
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
        setReady(true)
        setError('')
      } catch (failure) {
        if (!cancelled) setError(failure instanceof Error ? failure.message : 'Could not connect to UAM.')
      } finally {
        polling.current = false
      }
    }
    refreshNow.current = refresh
    void refresh()
    const timer = window.setInterval(() => void refresh(), 2000)
    document.addEventListener('visibilitychange', refresh)
    return () => {
      cancelled = true
      refreshNow.current = null
      window.clearInterval(timer)
      document.removeEventListener('visibilitychange', refresh)
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
      <strong className="truncate">{conversationOpen && session ? session.name : 'UAM'}</strong>
      {conversationOpen && session && <>
        <IconButton icon={session.isPinned ? <PinOff size={16} /> : <Pin size={16} />} label={session.isPinned ? 'Unpin chat' : 'Pin chat'} active={Boolean(session.isPinned)} disabled={pinning} aria-busy={pinning || undefined} onClick={() => void togglePin()} />
        <span role="status" aria-label={currentStatus?.type === 'processing' ? 'Agent running' : currentStatus?.type === 'attention' ? `Needs attention: ${currentStatus.kind}` : currentStatus?.type === 'done' ? 'Done' : 'Idle'} className={`session-status session-status--${currentStatus?.type === 'processing' ? 'processing' : currentStatus?.type === 'attention' ? 'attention' : 'idle'}`}>
          {currentStatus?.type === 'attention' ? <CircleAlert size={16} /> : currentStatus?.type === 'done' ? <Check size={16} /> : <span />}
        </span>
      </>}
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
              {tab === 'chats' && <Button size="sm" variant="primary" onClick={() => setStoreNewChatModalOpen(true)}>New chat</Button>}
            </div>
          {tab === 'activity' ? activeSessions.length ? activeSessions.map((candidate) => <SessionItem key={candidate.id} sessionId={candidate.id} session={candidate} familySessionIds={[candidate.id]} />) : <p className="px-3">No active chats.</p> : <>
            <input className="uam-companion-search" type="search" aria-label="Search chats" placeholder="Search chats" value={search} onChange={(event) => setSearch(event.target.value)} />
            <FolderTree searchQuery={search} />
          </>}
        </div>}
      </main>
      {storeNewChatModalOpen && <Suspense fallback={null}><NewChatModal companion onCreated={() => { setConversationOpen(true); void refreshNow.current?.() }} /></Suspense>}
    </> : <p className="p-4" role="status">{error ? 'Retrying connection…' : 'Loading chats…'}</p>}
    {token && <nav className="uam-companion-nav" aria-label="Companion navigation">
      {(['activity', 'chats', 'settings'] as const).map((item) => <button key={item} type="button" aria-current={tab === item && !conversationOpen ? 'page' : undefined} onClick={() => { manualTabChoice.current = true; setTab(item); setConversationOpen(false); setPinError('') }}>
        {item === 'activity' ? <Activity size={18} /> : item === 'chats' ? <MessageSquare size={18} /> : <Settings size={18} />}{item === 'activity' ? 'Activity' : item === 'chats' ? 'Chats' : 'Settings'}
      </button>)}
    </nav>}
  </div>
}
