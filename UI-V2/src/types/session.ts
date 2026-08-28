import type { MemoryLevel } from './memory'

export type ViewMode = 'chat' | 'cli'

export type ComputerUseControlState = 'running' | 'paused' | 'stopped'
export type ComputerUseBackend = 'auto' | 'provider' | 'uam'
export type ComputerUseEffectiveBackend = 'provider' | 'uam'

export interface ExecutionHost {
  id: string
  label: string
  transport: 'local' | 'ssh'
  sshAlias: string
  runnerStatus: 'uninstalled' | 'installing' | 'ready' | 'offline' | 'error'
  runnerVersion: string
  platform: string
  architecture: string
  lastSeenAt: string
}

export interface ComputerUseActionResult {
  ok: boolean
  error?: string
}

export interface ComputerUseHistoryEntry {
  time: string
  action: string
  status: string
  detail: string
}

export interface ComputerUseState {
  enabled: boolean
  state: ComputerUseControlState
  history: ComputerUseHistoryEntry[]
}

export interface Session {
  id: string
  executionHostId?: string
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
  computerUseEnabled?: boolean
  computerUseBackend?: ComputerUseBackend
  computerUseEffectiveBackend?: ComputerUseEffectiveBackend
  computerUseProviderAvailable?: boolean
  computerUseTargetKind?: 'window' | 'screen'
  computerUseTargetId?: string
  computerUseTargetTitle?: string
  computerUseTargetInputMode?: 'background' | 'foreground'
  computerUse?: ComputerUseState
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
