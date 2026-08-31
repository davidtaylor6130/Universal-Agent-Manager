import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Plus } from 'lucide-react'
import { FolderTree } from '../sidebar/FolderTree'
import { ChatSearchBar } from '../sidebar/ChatSearchBar'
import { Button } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { createRequestId, sendToCEF } from '../../ipc/cefBridge'
import type { ChatSearchFilters, ChatStatusFilterId } from '../sidebar/chatSearch'
import { DEFAULT_PROVIDER_ID } from '../../utils/providerMetadata'

interface SearchChatMessagesResponse {
  chatIds?: string[]
}

export function Sidebar() {
  const [searchQuery, setSearchQuery] = useState('')
  const [deepSearch, setDeepSearch] = useState(false)
  const [deepSearchSessionIds, setDeepSearchSessionIds] = useState<string[] | undefined>(undefined)
  const [deepSearchError, setDeepSearchError] = useState('')
  const [deepSearchLoading, setDeepSearchLoading] = useState(false)
  const [deepSearchRetry, setDeepSearchRetry] = useState(0)
  const [filters, setFilters] = useState<ChatSearchFilters>({ providerIds: [], statusIds: [] })
  const latestDeepSearchRequestRef = useRef('')
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const sessions = useAppStore((s) => s.sessions)
  const providers = useAppStore((s) => s.providers)
  const clearSearch = useCallback(() => setSearchQuery(''), [])
  const providerOptions = useMemo(() => {
    const labels = new Map(providers.map((provider) => [provider.id, provider.shortName || provider.name || provider.id]))
    for (const session of sessions) {
      const providerId = session.providerId || DEFAULT_PROVIDER_ID
      if (!labels.has(providerId)) {
        labels.set(providerId, providerId)
      }
    }
    return Array.from(labels.entries())
      .map(([id, label]) => ({ id, label }))
      .sort((a, b) => a.label.localeCompare(b.label))
  }, [providers, sessions])
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
      })
    }, 180)

    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [deepSearch, deepSearchRetry, searchQuery])

  return (
    <div className="flex flex-col h-full overflow-hidden" style={{ background: 'var(--sidebar-bg)' }}>
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
      {deepSearchLoading && <div role="status" className="px-3 pb-2 text-xs" style={{ color: 'var(--text-3)' }}>Searching message contents…</div>}
      {deepSearchError && (
        <div role="alert" className="flex items-center justify-between gap-2 px-3 pb-2 text-xs" style={{ color: 'var(--red)' }}>
          <span>{deepSearchError}</span>
          <Button size="sm" variant="ghost" onClick={() => setDeepSearchRetry((value) => value + 1)}>Retry</Button>
        </div>
      )}
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
          onClick={() => setNewChatModalOpen(true)}
        >
          New Chat
        </Button>
      </div>
    </div>
  )
}
