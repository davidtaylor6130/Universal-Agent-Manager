import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Plus } from 'lucide-react'
import { FolderTree } from '../sidebar/FolderTree'
import { ChatSearchBar } from '../sidebar/ChatSearchBar'
import { Button } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { createRequestId, sendToCEF } from '../../ipc/cefBridge'
import type { ChatSearchFilters, ChatStatusFilterId } from '../sidebar/chatSearch'
import { DEFAULT_PROVIDER_ID, providerDisabledOn } from '../../utils/providerMetadata'
import './SidebarGlide.css'
import { shortcutLabel } from '../../utils/shortcuts'
import type { ChatSearchHit, SearchChatMessagesResponse } from '../../types/chatSearch'
import { sanitizeChatSearchHits } from '../../utils/chatSearchResults'

export function Sidebar() {
  const [searchQuery, setSearchQuery] = useState('')
  const [deepSearch, setDeepSearch] = useState(false)
  const [deepSearchSessionIds, setDeepSearchSessionIds] = useState<string[] | undefined>(undefined)
  const [deepSearchError, setDeepSearchError] = useState('')
  const [deepSearchHits, setDeepSearchHits] = useState<ChatSearchHit[]>([])
  const [hitRequestId, setHitRequestId] = useState('')
  const [deepSearchLoading, setDeepSearchLoading] = useState(false)
  const [deepSearchRetry, setDeepSearchRetry] = useState(0)
  const [filters, setFilters] = useState<ChatSearchFilters>({ providerIds: [], statusIds: [] })
  const latestDeepSearchRequestRef = useRef('')
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const sessions = useAppStore((s) => s.sessions)
  const providers = useAppStore((s) => s.providers)
  const providerAvailability = useAppStore((s) => s.providerAvailability)
  const jumpToChatSearchHit = useAppStore((s) => s.jumpToChatSearchHit)
  const clearChatSearchNavigation = useAppStore((s) => s.clearChatSearchNavigation)
  const searchNavigation = useAppStore((s) => s.chatSearchNavigation)
  const clearSearch = useCallback(() => setSearchQuery(''), [])
  const providerOptions = useMemo(() => {
    // A provider disabled on this computer stays filterable only while chats still use it.
    const labels = new Map(providers.filter((provider) => !providerDisabledOn(providerAvailability, provider.id, 'local') || sessions.some((session) => (session.providerId || DEFAULT_PROVIDER_ID) === provider.id))
      .map((provider) => [provider.id, provider.shortName || provider.name || provider.id]))
    for (const session of sessions) {
      const providerId = session.providerId || DEFAULT_PROVIDER_ID
      if (!labels.has(providerId)) {
        labels.set(providerId, providerId)
      }
    }
    return Array.from(labels.entries())
      .map(([id, label]) => ({ id, label }))
      .sort((a, b) => a.label.localeCompare(b.label))
  }, [providers, providerAvailability, sessions])
  const toggleProviderFilter = useCallback((providerId: string) => {
    setFilters((current) => ({
      ...current,
      providerIds: current.providerIds.includes(providerId)
        ? current.providerIds.filter((id) => id !== providerId)
        : [...current.providerIds, providerId],
    }))
  }, [])
  const toggleStatusFilter = useCallback((statusId: ChatStatusFilterId) => {
    setFilters((current) => ({
      ...current,
      statusIds: current.statusIds.includes(statusId)
        ? current.statusIds.filter((id) => id !== statusId)
        : [...current.statusIds, statusId],
    }))
  }, [])
  const clearFilters = useCallback(() => setFilters({ providerIds: [], statusIds: [] }), [])

  useEffect(() => {
    const previousRequestId = latestDeepSearchRequestRef.current
    if (previousRequestId) void clearChatSearchNavigation(previousRequestId)
    latestDeepSearchRequestRef.current = ''
    setDeepSearchHits([])
    setHitRequestId('')
    const query = searchQuery.trim()
    if (!deepSearch || !query) {
      setDeepSearchSessionIds(undefined)
      setDeepSearchError('')
      setDeepSearchLoading(false)
      return
    }

    let cancelled = false
    setDeepSearchError('')
    setDeepSearchLoading(true)
    setDeepSearchSessionIds(undefined)
    const requestId = createRequestId('searchChatMessages')
    latestDeepSearchRequestRef.current = requestId
    const timer = window.setTimeout(() => {
      void sendToCEF<SearchChatMessagesResponse>({
        action: 'searchChatMessages',
        payload: { query, requestId },
        requestId,
      }).then((response) => {
        if (cancelled || latestDeepSearchRequestRef.current !== response.requestId) return
        setDeepSearchLoading(false)
        if (!response.ok || !Array.isArray(response.data?.chatIds)) {
          setDeepSearchError(response.error || 'Message search failed. Try again.')
          return
        }
        setDeepSearchSessionIds(response.data.chatIds)
        setDeepSearchHits(sanitizeChatSearchHits(response.data.hits, new Set(response.data.chatIds)))
        setHitRequestId(requestId)
      })
    }, 180)

    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [clearChatSearchNavigation, deepSearch, deepSearchRetry, searchQuery])

  return (
    <div className="uam-sidebar-glide flex flex-col h-full overflow-hidden" style={{ background: 'var(--sidebar-bg)' }}>
      <ChatSearchBar
        value={searchQuery}
        deepSearch={deepSearch}
        filters={filters}
        providerOptions={providerOptions}
        onChange={setSearchQuery}
        onClear={clearSearch}
        onToggleDeepSearch={() => setDeepSearch((enabled) => !enabled)}
        onToggleProviderFilter={toggleProviderFilter}
        onToggleStatusFilter={toggleStatusFilter}
        onClearFilters={clearFilters}
      />
      {deepSearchLoading && <div role="status" className="uam-sb-search-status px-3 pb-2 text-xs">Searching message contents…</div>}
      {deepSearchError && (
        <div role="alert" className="uam-sb-search-error flex items-center justify-between gap-2 px-3 pb-2 text-xs">
          <span>{deepSearchError}</span>
          <Button size="sm" variant="ghost" onClick={() => setDeepSearchRetry((value) => value + 1)}>Retry</Button>
        </div>
      )}
      {deepSearchHits.length > 0 && <div aria-label="Message matches" className="uam-sb-hits max-h-40 overflow-y-auto px-3 pb-2">
        <div className="uam-sb-hits__label" aria-hidden>Message matches</div>
        {deepSearchHits.filter((hit) => sessions.some((session) => session.id === hit.chatId)).map((hit) => {
          const session = sessions.find((candidate) => candidate.id === hit.chatId)!
          const source = hit.field === 'thoughts' ? 'Thoughts' : hit.field === 'planSummary' ? 'Plan' : 'Message'
          return <button key={`${hit.chatId}:${hit.messageIndex}:${hit.field}`} type="button"
            className="uam-sb-hit block w-full rounded-md px-2 py-1 text-left text-xs" 
            disabled={searchNavigation?.phase === 'loading'}
            onClick={() => {
              void jumpToChatSearchHit(hit, hitRequestId).then((opened) => {
                if (!opened && latestDeepSearchRequestRef.current === hitRequestId) setDeepSearchError('Search result changed. Search again to open the message.')
              })
            }}>
            <span className="block truncate"><span className="font-medium">{session.name}</span><span style={{ color: 'var(--text-3)' }}> · {source} {hit.messageIndex + 1}</span></span>
            <span className="uam-sb-hit__excerpt block line-clamp-2">{hit.excerpt}</span>
          </button>
        })}
      </div>}
      <div
        data-testid="sidebar-tree-scroll"
        className="flex-1 overflow-y-auto overflow-x-hidden py-1"
        style={{ borderTop: '1px solid var(--border)' }}
      >
        <FolderTree searchQuery={searchQuery} deepSearchSessionIds={deepSearch ? deepSearchSessionIds : undefined} filters={filters} />
      </div>
      <div
        className="flex-shrink-0 p-3"
        style={{
          borderTop: '1px solid var(--border)',
          background: 'color-mix(in srgb, var(--sidebar-bg) 92%, var(--surface))',
        }}
      >
        <Button
          variant="primary"
          size="lg"
          block
          leadingIcon={<Plus size={16} />}
          title={`New chat (${shortcutLabel('newChat')})`}
          onClick={() => setNewChatModalOpen(true)}
        >
          New Chat
          <kbd className="uam-btn__kbd" aria-hidden>{shortcutLabel('newChat')}</kbd>
        </Button>
      </div>
    </div>
  )
}
