export type ChatPaneCount = 1 | 2 | 4

export interface ChatGridLayout {
  paneCount: ChatPaneCount
  activePane: number
  sessionIds: string[]
  columnSizes: number[]
  rowSizes: number[]
}

const storageKey = 'uam-chat-grid-layout-v1'

export const defaultChatGridLayout: ChatGridLayout = {
  paneCount: 1,
  activePane: 0,
  sessionIds: [],
  columnSizes: [50, 50],
  rowSizes: [50, 50],
}

function paneCount(value: unknown): ChatPaneCount {
  return value === 2 || value === 4 ? value : 1
}

function sizes(value: unknown): number[] {
  return Array.isArray(value) && value.length === 2 && value.every((size) => typeof size === 'number' && Number.isFinite(size))
    ? value
    : [50, 50]
}

export function readChatGridLayout(): ChatGridLayout {
  try {
    const parsed = JSON.parse(localStorage.getItem(storageKey) ?? '{}')
    const count = paneCount(parsed.paneCount)
    return {
      paneCount: count,
      activePane: Math.min(count - 1, Math.max(0, Number(parsed.activePane) || 0)),
      sessionIds: Array.isArray(parsed.sessionIds)
        ? parsed.sessionIds.slice(0, 4).map((id: unknown) => typeof id === 'string' ? id : '')
        : [],
      columnSizes: sizes(parsed.columnSizes),
      rowSizes: sizes(parsed.rowSizes),
    }
  } catch {
    return defaultChatGridLayout
  }
}

export function writeChatGridLayout(layout: ChatGridLayout): void {
  try {
    localStorage.setItem(storageKey, JSON.stringify(layout))
  } catch {
    // The in-memory layout still works when storage is unavailable.
  }
}
