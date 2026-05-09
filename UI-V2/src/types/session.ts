export type ViewMode = 'chat' | 'cli'

export interface Session {
  id: string
  name: string
  viewMode: ViewMode
  folderId: string | null
  isPinned?: boolean
	  providerId?: string
	  workspaceDirectory?: string
	  workspaceIsolationKind?: string
	  workspaceSourceDirectory?: string
	  workspaceBaseRef?: string
	  workspaceBranchName?: string
	  workspaceWorktreeDirectory?: string
	  modelId?: string
  reasoningEffort?: string
  serviceTier?: string
  approvalMode?: string
  autoApproveCommands?: boolean
  memoryEnabled?: boolean
  memoryLastProcessedMessageCount?: number
  memoryLastProcessedAt?: string
  createdAt: Date
  updatedAt: Date
  lastOpenedAt?: Date
}

export interface Folder {
  id: string
  name: string
  parentId: string | null
  directory: string
  isExpanded: boolean
  createdAt: Date
}
