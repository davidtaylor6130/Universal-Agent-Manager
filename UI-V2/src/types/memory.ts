export type MemoryScopeType = 'all' | 'global' | 'folder'
export type MemoryEntryTargetScopeType = 'global' | 'folder'
export type MemoryLevel = 'off' | 'strict' | 'balanced' | 'open'
export const MEMORY_LEVEL_OPTIONS: Array<{ id: MemoryLevel; label: string; detail: string }> = [
  { id: 'off', label: 'Off', detail: 'Do not create automatic memories.' },
  { id: 'strict', label: 'Strict', detail: 'Save only critical, high-confidence durable memories.' },
  { id: 'balanced', label: 'Balanced', detail: 'Save useful medium- and high-confidence memories.' },
  { id: 'open', label: 'Open', detail: 'Save every valid, non-sensitive worker candidate.' },
]

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
  memoryLevel: MemoryLevel
  memoryLastProcessedAt: string
  alreadyFullyProcessed: boolean
}
