export type ViewMode = 'chat' | 'cli'

export interface Session {
  id: string
  name: string
  viewMode: ViewMode
  folderId: string | null
  workspaceDirectory?: string
  modelId?: string
  approvalMode?: string
  createdAt: Date
  updatedAt: Date
}

export interface Folder {
  id: string
  name: string
  parentId: string | null
  directory: string
  isExpanded: boolean
  createdAt: Date
}
