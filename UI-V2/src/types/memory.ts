export type MemoryScopeType = 'all' | 'global' | 'folder'
export type MemoryEntryTargetScopeType = 'global' | 'folder'

export interface MemoryScope {
  scopeType: MemoryScopeType
  folderId: string
  label: string
  rootPath: string
  rootCount?: number
}

export interface MemoryEntry {
  id: string
  title: string
  category: string
  scope: string
  confidence: string
  sourceChatId: string
  lastObserved: string
  occurrenceCount: number
  preview: string
  filePath: string
  scopeType?: MemoryEntryTargetScopeType
  folderId?: string
  scopeLabel?: string
  rootPath?: string
}

export interface MemoryEntryDraft {
  category: string
  title: string
  memory: string
  evidence: string
  confidence: string
  sourceChatId: string
  targetScopeType?: MemoryEntryTargetScopeType
  targetFolderId?: string
}

export interface MemoryScanCandidate {
  chatId: string
  title: string
  folderId: string
  folderTitle: string
  providerId: string
  messageCount: number
  memoryEnabled: boolean
  memoryLastProcessedAt: string
  alreadyFullyProcessed: boolean
}
