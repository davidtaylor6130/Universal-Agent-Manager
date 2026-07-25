import type { MemoryLevel } from './memory'

export type ViewMode = 'chat' | 'cli'

export interface Session {
  id: string
  name: string
  viewMode: ViewMode
  folderId: string | null
  isPinned?: boolean
  providerId?: string
  parentChatId?: string
  branchRootChatId?: string
  branchFromMessageIndex?: number
  branchMessageEdited?: boolean
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
  commandSafetyTier?: 'off' | 'acceptEdits' | 'low' | 'medium' | 'high' | 'yolo'
  memoryEnabled?: boolean
  memoryLevel?: MemoryLevel
  smallModelMode?: boolean
  memoryLastProcessedMessageCount?: number
  memoryLastProcessedAt?: string
  messageCount?: number
  messagesDigest?: string
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
  missing?: boolean
  createdAt: Date
}
