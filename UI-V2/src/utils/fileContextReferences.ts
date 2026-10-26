import type { ContextReference } from '../types/contextReference'
import { filePreviewResponse } from '../types/filePreview'
import { useAppStore } from '../store/useAppStore'
import { captureContextReference } from './contextReferences'
import { contextReferenceOwner } from './contextReferenceOwner'

/** Explicit candidates only; detecting a mention never reads a file. Quote paths containing spaces. */
export function draftFileMentions(text: string): string[] {
  const paths = new Set<string>()
  for (const match of text.matchAll(/(?:^|\s)@(?:"([^"\r\n]+)"|([^\s"]+))/g)) {
    const path = match[1] ?? match[2]
    if (path.length <= 4096 && !/[\x00-\x1f\x7f]/.test(path)) paths.add(path)
    if (paths.size === 4) break
  }
  return [...paths]
}

/** Only the native workspace reader supplies file bytes and resolved identity; labels grant no read authority. */
export async function captureWorkspaceFileReference(chatId: string, path: string): Promise<ContextReference> {
  const owner = contextReferenceOwner(chatId)
  if (!owner?.workspace) throw new Error('Choose a chat with a workspace before referencing a file.')
  const file = filePreviewResponse(await useAppStore.getState().getWorkspaceFilePreview(chatId, path))
  // The native service resolves/expands the configured workspace and fences its own result.
  // Retain that returned identity; frontend path text is only a configuration-change witness.
  if (contextReferenceOwner(chatId)?.key !== owner.key || !file || file.executionHostId !== owner.host ||
    file.chatId !== chatId || !file.chatCreatedAt || Date.parse(file.chatCreatedAt) !== owner.birth) {
    throw new Error('The file workspace or host changed. Reference the file again.')
  }
  const snapshot = captureContextReference({
    kind: 'file', sourceChatId: chatId, sourceChatCreatedAt: file.chatCreatedAt, sourcePath: file.path, executionHostId: file.executionHostId,
    workspaceDirectory: file.workspace, capturedAt: new Date().toISOString(),
    label: file.path.split(/[\\/]/).pop()?.slice(0, 160) || 'file', text: file.text,
  })
  if (!snapshot) throw new Error('This file cannot be retained as referenced context.')
  return snapshot
}
