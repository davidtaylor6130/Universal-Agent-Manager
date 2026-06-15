export interface MarkdownStoreEntry {
  id: string
  title: string
  maker: string
  review: string
  dateCreated: string
  dateUpdated: string
  preview: string
  filePath: string
}

export interface MarkdownStoreDraft {
  title: string
  maker: string
  review: string
  body: string
}
