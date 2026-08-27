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
  importedReadOnly?: boolean
  modelId?: string
  reviewerModelId?: string
  reasoningEffort?: string
  serviceTier?: string
  serviceTierExplicit?: boolean
  approvalMode?: string
  uamAgentId?: string
  uamControlEnabled?: boolean
  commandSafetyTier?: 'off' | 'acceptEdits' | 'aiReview' | 'yolo'
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

export interface WorkspaceFolderRecoveryChat {
  id: string
  title: string
  directory: string
  reason: string
}

export interface WorkspaceFolderRecoveryGroup {
  title: string
  directory: string
  existingFolderId: string
  chatIds: string[]
}

export interface WorkspaceFolderRecoveryPreview {
  groups: WorkspaceFolderRecoveryGroup[]
  missing: WorkspaceFolderRecoveryChat[]
  unavailable: WorkspaceFolderRecoveryChat[]
  noLocation: WorkspaceFolderRecoveryChat[]
}
