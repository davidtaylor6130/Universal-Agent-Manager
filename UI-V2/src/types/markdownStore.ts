export interface MarkdownStoreEntry {
  id: string
  title: string
  maker: string
  review: string
  dateCreated: string
  dateUpdated: string
  preview: string
  body?: string
  favorite?: boolean
  sourceProvider?: string
  sourcePath?: string
  commandName?: string
  group?: string
  filePath: string
}

export interface MarkdownStoreDraft {
  title: string
  maker: string
  review: string
  body: string
}

export interface MarkdownStoreImportCandidate {
  id: string
  title: string
  maker: string
  review: string
  preview: string
  sourceProvider: string
  sourcePath: string
  supported: boolean
  validationError: string
  collisionPath: string
}

export type MarkdownStoreConflictAction = 'skip' | 'replace' | 'separate'

export interface MarkdownStoreImportResult {
  sourcePath: string
  status: 'imported' | 'skipped' | 'error'
  message: string
  entry?: MarkdownStoreEntry
}
