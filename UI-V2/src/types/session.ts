export type ViewMode = 'structured' | 'cli' | 'coding-agent'

export interface Session {
  id: string
  name: string
  viewMode: ViewMode
  folderId: string | null
  createdAt: Date
  updatedAt: Date
}

export interface Folder {
  id: string
  name: string
  parentId: string | null
  isExpanded: boolean
  createdAt: Date
}
