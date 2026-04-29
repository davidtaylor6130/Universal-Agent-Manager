import { useCallback, useEffect, useState } from 'react'
import { FolderTree } from '../sidebar/FolderTree'
import { ChatSearchBar } from '../sidebar/ChatSearchBar'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'

interface SearchChatMessagesResponse {
  chatIds?: string[]
}

export function Sidebar() {
  const [searchQuery, setSearchQuery] = useState('')
  const [deepSearch, setDeepSearch] = useState(false)
  const [deepSearchSessionIds, setDeepSearchSessionIds] = useState<string[] | undefined>(undefined)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const clearSearch = useCallback(() => setSearchQuery(''), [])

  useEffect(() => {
    const query = searchQuery.trim()
    if (!deepSearch || !query) {
      setDeepSearchSessionIds(undefined)
      return
    }

    let cancelled = false
    const timer = window.setTimeout(() => {
      void sendToCEF<SearchChatMessagesResponse>({
        action: 'searchChatMessages',
        payload: { query },
      }).then((response) => {
        if (cancelled) return
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
        onChange={setSearchQuery}
        onClear={clearSearch}
        onToggleDeepSearch={() => setDeepSearch((enabled) => !enabled)}
      />
      <div
        className="flex-1 overflow-y-auto overflow-x-hidden py-2"
        style={{ borderTop: '1px solid var(--border)' }}
      >
        <FolderTree searchQuery={searchQuery} deepSearchSessionIds={deepSearch ? deepSearchSessionIds : undefined} />
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
