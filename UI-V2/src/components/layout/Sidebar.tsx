import { SidebarHeader } from '../sidebar/SidebarHeader'
import { FolderTree } from '../sidebar/FolderTree'

export function Sidebar() {
  return (
    <div className="flex flex-col h-full overflow-hidden" style={{ background: 'var(--sidebar-bg)' }}>
      <SidebarHeader />
      <div
        className="flex-1 overflow-y-auto overflow-x-hidden py-2"
        style={{ borderTop: '1px solid var(--border)' }}
      >
        <FolderTree />
      </div>
    </div>
  )
}
