export interface ContextReference {
  id: string
  kind: 'message' | 'terminal' | 'file' | 'branch' | 'worktree'
  sourceChatId: string
  /** Historical source birth; quoted data never grants control of that chat. */
  sourceChatCreatedAt?: string
  sourceMessageIndex?: number
  sourceMessageId?: string
  terminalId?: string
  /** Captured display provenance; native workspace reads independently validate paths. */
  sourcePath?: string
  branchName?: string
  worktreeDirectory?: string
  sourceDirectory?: string
  baseRef?: string
  nativeSessionId?: string
  executionHostId: string
  workspaceDirectory: string
  capturedAt: string
  label: string
  text: string
  truncated: boolean
}
