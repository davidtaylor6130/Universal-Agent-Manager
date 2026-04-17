import { useCallback, useState } from 'react'
import { SidebarHeader } from '../sidebar/SidebarHeader'
import { FolderTree } from '../sidebar/FolderTree'
import { ChatSearchBar } from '../sidebar/ChatSearchBar'

export function Sidebar() {
  const [searchQuery, setSearchQuery] = useState('')
  const clearSearch = useCallback(() => setSearchQuery(''), [])

  return (
    <div className="flex flex-col h-full overflow-hidden" style={{ background: 'var(--sidebar-bg)' }}>
      <SidebarHeader />
      <ChatSearchBar
        value={searchQuery}
        onChange={setSearchQuery}
        onClear={clearSearch}
      />
      <div
        className="flex-1 overflow-y-auto overflow-x-hidden py-2"
        style={{ borderTop: '1px solid var(--border)' }}
      >
        <FolderTree searchQuery={searchQuery} />
      </div>
    </div>
  )
}
