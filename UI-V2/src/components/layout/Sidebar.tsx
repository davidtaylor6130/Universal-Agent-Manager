import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { FolderTree } from '../sidebar/FolderTree'
import { ChatSearchBar } from '../sidebar/ChatSearchBar'
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
      return
    }

    let cancelled = false
    const requestId = createRequestId('searchChatMessages')
    latestDeepSearchRequestRef.current = requestId
    const timer = window.setTimeout(() => {
      void sendToCEF<SearchChatMessagesResponse>({
        action: 'searchChatMessages',
        payload: { query, requestId },
        requestId,
      }).then((response) => {
        if (cancelled || latestDeepSearchRequestRef.current !== response.requestId) return
        setDeepSearchSessionIds(response.ok && Array.isArray(response.data?.chatIds) ? response.data.chatIds : [])
      })
    }, 180)

    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [deepSearch, searchQuery])

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
      <div
        className="flex-1 overflow-y-auto overflow-x-hidden py-2"
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
        <button
          type="button"
          className="uam-primary-button w-full justify-center"
          onClick={() => setNewChatModalOpen(true)}
        >
          <span aria-hidden="true">+</span>
          <span>New Chat</span>
        </button>
      </div>
    </div>
  )
}
