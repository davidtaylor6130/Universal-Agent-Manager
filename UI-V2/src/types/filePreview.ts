export interface WorkspaceFilePreview {
  text: string
  path: string
  workspace: string
  executionHostId: string
  chatId?: string
  chatCreatedAt?: string
}

export function filePreviewResponse(value: unknown): WorkspaceFilePreview | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  if (typeof item.text !== 'string' || item.text.length > 1024 * 1024 || item.text.includes('\0') ||
    typeof item.path !== 'string' || !item.path || item.path.length > 16384 || /[\x00-\x1f\x7f]/.test(item.path) ||
    typeof item.workspace !== 'string' || !item.workspace || item.workspace.length > 16384 || /[\x00-\x1f\x7f]/.test(item.workspace) ||
    typeof item.executionHostId !== 'string' || !item.executionHostId || item.executionHostId.length > 256 || /[\x00-\x1f\x7f]/.test(item.executionHostId)) return null
  if ((item.chatId !== undefined && (typeof item.chatId !== 'string' || item.chatId.length > 128)) ||
    (item.chatCreatedAt !== undefined && (typeof item.chatCreatedAt !== 'string' || item.chatCreatedAt.length > 64))) return null
  return item as unknown as WorkspaceFilePreview
}
