export type ChatPaneCount = 1 | 2 | 4

export interface ChatGridLayout {
  paneCount: ChatPaneCount
  activePane: number
  sessionIds: string[]
  columnSizes: number[]
  rowSizes: number[]
}

const storageKey = 'uam-chat-grid-layout-v1'
const layoutEvent = 'uam-chat-grid-layout'

export const chatPaneColors = ['#f97316', '#ec4899', '#22c55e', '#a855f7'] as const

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
  window.dispatchEvent(new CustomEvent<ChatGridLayout>(layoutEvent, { detail: layout }))
}

export function subscribeChatGridLayout(listener: (layout: ChatGridLayout) => void): () => void {
  const handleLayout = (event: Event) => listener((event as CustomEvent<ChatGridLayout>).detail)
  window.addEventListener(layoutEvent, handleLayout)
  return () => window.removeEventListener(layoutEvent, handleLayout)
}

export function assignChatToPane(sessionId: string, paneIndex: number): ChatGridLayout {
  const layout = readChatGridLayout()
  const sessionIds = [...layout.sessionIds]
  while (sessionIds.length < layout.paneCount) sessionIds.push('')
  const previousPane = sessionIds.indexOf(sessionId)
  if (previousPane >= 0) sessionIds[previousPane] = sessionIds[paneIndex]
  sessionIds[paneIndex] = sessionId
  const next = { ...layout, activePane: paneIndex, sessionIds }
  writeChatGridLayout(next)
  return next
}

export function removeChatsFromGrid(sessionIdsToRemove: Iterable<string>): ChatGridLayout {
  const removed = new Set(sessionIdsToRemove)
  const layout = readChatGridLayout()
  const sessionIds = layout.sessionIds.map((id) => removed.has(id) ? '' : id)
  const activePane = sessionIds[layout.activePane]
    ? layout.activePane
    : Math.max(0, sessionIds.slice(0, layout.paneCount).findIndex(Boolean))
  const next = { ...layout, activePane, sessionIds }
  writeChatGridLayout(next)
  return next
}
