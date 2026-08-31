export type ChatSplitDirection = 'horizontal' | 'vertical'

export interface ChatPaneLeaf {
  type: 'leaf'
  id: string
  sessionId: string
}

export interface ChatPaneSplit {
  type: 'split'
  id: string
  direction: ChatSplitDirection
  sizes: [number, number]
  children: [ChatPaneNode, ChatPaneNode]
}

export type ChatPaneNode = ChatPaneLeaf | ChatPaneSplit

export interface ChatGridLayout {
  version: 2
  root: ChatPaneNode
  activeLeafId: string
}

const storageKey = 'uam-chat-grid-layout-v2'
const legacyStorageKey = 'uam-chat-grid-layout-v1'
const viewModeStorageKey = 'uam-chat-view-modes-v1'
const layoutEvent = 'uam-chat-grid-layout'

export const MAX_CHAT_PANES = 9
export const chatPaneColors = ['#f97316', '#ec4899', '#22c55e', '#a855f7', '#06b6d4', '#eab308', '#3b82f6', '#ef4444', '#84cc16'] as const

let cachedViewModesRaw: string | null | undefined
let cachedViewModes: Record<string, 'chat' | 'cli'> = {}

function readChatViewModes(): Record<string, 'chat' | 'cli'> {
  let raw: string | null | undefined
  try {
    raw = localStorage.getItem(viewModeStorageKey)
    if (raw === cachedViewModesRaw) return cachedViewModes
    const value = JSON.parse(raw ?? '{}') as Record<string, unknown>
    cachedViewModes = Object.fromEntries(Object.entries(value).filter(([, mode]) => mode === 'chat' || mode === 'cli')) as Record<string, 'chat' | 'cli'>
  } catch {
    cachedViewModes = {}
  }
  cachedViewModesRaw = raw
  return cachedViewModes
}

export function readChatViewMode(sessionId: string): 'chat' | 'cli' | null {
  return readChatViewModes()[sessionId] ?? null
}

export function writeChatViewMode(sessionId: string, viewMode: 'chat' | 'cli'): void {
  try {
    localStorage.setItem(viewModeStorageKey, JSON.stringify({ ...readChatViewModes(), [sessionId]: viewMode }))
  } catch {
    // The current pane still changes when storage is unavailable.
  }
}

function removeChatViewModes(sessionIds: Set<string>): void {
  const modes = readChatViewModes()
  let changed = false
  for (const sessionId of sessionIds) changed = delete modes[sessionId] || changed
  if (changed) {
    try { localStorage.setItem(viewModeStorageKey, JSON.stringify(modes)) } catch { /* Best effort. */ }
  }
}

export const defaultChatGridLayout: ChatGridLayout = {
  version: 2,
  root: { type: 'leaf', id: 'leaf-1', sessionId: '' },
  activeLeafId: 'leaf-1',
}

export function chatGridLeaves(node: ChatPaneNode): ChatPaneLeaf[] {
  return node.type === 'leaf' ? [node] : [...chatGridLeaves(node.children[0]), ...chatGridLeaves(node.children[1])]
}

function validSizes(value: unknown): [number, number] {
  if (!Array.isArray(value) || value.length !== 2 || value.some((size) => typeof size !== 'number' || !Number.isFinite(size) || size <= 0)) return [50, 50]
  const total = value[0] + value[1]
  return total > 0 ? [value[0] * 100 / total, value[1] * 100 / total] : [50, 50]
}

function parseNode(value: unknown, ids: Set<string>, depth = 0): ChatPaneNode | null {
  if (!value || typeof value !== 'object' || depth > MAX_CHAT_PANES) return null
  const node = value as Partial<ChatPaneNode>
  const id = typeof node.id === 'string' ? node.id.trim() : ''
  if (!id || ids.has(id)) return null
  ids.add(id)
  if (node.type === 'leaf') return { type: 'leaf', id, sessionId: typeof node.sessionId === 'string' ? node.sessionId : '' }
  if (node.type !== 'split') return null
  const children = (node as Partial<ChatPaneSplit>).children
  if (!Array.isArray(children) || children.length !== 2) return null
  const first = parseNode(children[0], ids, depth + 1)
  const second = parseNode(children[1], ids, depth + 1)
  if (!first || !second) return null
  return {
    type: 'split', id,
    direction: (node as Partial<ChatPaneSplit>).direction === 'vertical' ? 'vertical' : 'horizontal',
    sizes: validSizes((node as Partial<ChatPaneSplit>).sizes),
    children: [first, second],
  }
}

function migrateLegacy(value: unknown): ChatGridLayout | null {
  if (!value || typeof value !== 'object') return null
  const legacy = value as { paneCount?: unknown; activePane?: unknown; sessionIds?: unknown; columnSizes?: unknown; rowSizes?: unknown }
  const count = legacy.paneCount === 2 || legacy.paneCount === 4 ? legacy.paneCount : 1
  const ids = Array.isArray(legacy.sessionIds) ? legacy.sessionIds.map((id) => typeof id === 'string' ? id : '') : []
  const leaves = Array.from({ length: count }, (_, index): ChatPaneLeaf => ({ type: 'leaf', id: `leaf-${index + 1}`, sessionId: ids[index] ?? '' }))
  const columns = validSizes(legacy.columnSizes)
  const rows = validSizes(legacy.rowSizes)
  const root: ChatPaneNode = count === 1
    ? leaves[0]
    : count === 2
      ? { type: 'split', id: 'split-1', direction: 'horizontal', sizes: columns, children: [leaves[0], leaves[1]] }
      : {
          type: 'split', id: 'split-1', direction: 'horizontal', sizes: columns,
          children: [
            { type: 'split', id: 'split-2', direction: 'vertical', sizes: rows, children: [leaves[0], leaves[2]] },
            { type: 'split', id: 'split-3', direction: 'vertical', sizes: rows, children: [leaves[1], leaves[3]] },
          ],
        }
  const activeIndex = Math.min(count - 1, Math.max(0, Number(legacy.activePane) || 0))
  return { version: 2, root, activeLeafId: leaves[activeIndex].id }
}

export function readChatGridLayout(): ChatGridLayout {
  try {
    const raw = localStorage.getItem(storageKey)
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<ChatGridLayout>
      const root = parseNode(parsed.root, new Set())
      if (parsed.version === 2 && root && chatGridLeaves(root).length <= MAX_CHAT_PANES) {
        const leaves = chatGridLeaves(root)
        return { version: 2, root, activeLeafId: leaves.some((leaf) => leaf.id === parsed.activeLeafId) ? parsed.activeLeafId! : leaves[0].id }
      }
    }
    const legacyRaw = localStorage.getItem(legacyStorageKey)
    const migrated = legacyRaw ? migrateLegacy(JSON.parse(legacyRaw)) : null
    if (migrated) return migrated
  } catch {
    // Fall through to a fresh single pane.
  }
  return defaultChatGridLayout
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

function mapNode(node: ChatPaneNode, leafId: string, update: (leaf: ChatPaneLeaf) => ChatPaneNode): ChatPaneNode {
  if (node.type === 'leaf') return node.id === leafId ? update(node) : node
  const first = mapNode(node.children[0], leafId, update)
  const second = mapNode(node.children[1], leafId, update)
  return first === node.children[0] && second === node.children[1] ? node : { ...node, children: [first, second] }
}

function nextId(root: ChatPaneNode, prefix: 'leaf' | 'split'): string {
  const ids = new Set<string>()
  const visit = (node: ChatPaneNode) => {
    ids.add(node.id)
    if (node.type === 'split') node.children.forEach(visit)
  }
  visit(root)
  let number = 1
  while (ids.has(`${prefix}-${number}`)) number += 1
  return `${prefix}-${number}`
}

export function setChatInLeaf(layout: ChatGridLayout, sessionId: string, leafId: string): ChatGridLayout {
  const target = chatGridLeaves(layout.root).find((leaf) => leaf.id === leafId)
  if (!target) return layout
  const previous = chatGridLeaves(layout.root).find((leaf) => leaf.sessionId === sessionId)
  let root = layout.root
  if (previous && previous.id !== leafId) root = mapNode(root, previous.id, (leaf) => ({ ...leaf, sessionId: target.sessionId }))
  root = mapNode(root, leafId, (leaf) => ({ ...leaf, sessionId }))
  return { ...layout, root, activeLeafId: leafId }
}

export function assignChatToPane(sessionId: string, leafId: string): ChatGridLayout {
  const next = setChatInLeaf(readChatGridLayout(), sessionId, leafId)
  writeChatGridLayout(next)
  return next
}

export function splitChatLeaf(layout: ChatGridLayout, leafId: string, direction: ChatSplitDirection): ChatGridLayout {
  if (chatGridLeaves(layout.root).length >= MAX_CHAT_PANES) return layout
  const newLeafId = nextId(layout.root, 'leaf')
  const splitId = nextId(layout.root, 'split')
  const root = mapNode(layout.root, leafId, (leaf) => ({
    type: 'split', id: splitId, direction, sizes: [50, 50],
    children: [leaf, { type: 'leaf', id: newLeafId, sessionId: '' }],
  }))
  return root === layout.root ? layout : { ...layout, root, activeLeafId: newLeafId }
}

export function resizeChatSplit(layout: ChatGridLayout, splitId: string, sizes: number[]): ChatGridLayout {
  const normalized = validSizes(sizes)
  const update = (node: ChatPaneNode): ChatPaneNode => {
    if (node.type === 'leaf') return node
    if (node.id === splitId) return Math.abs(node.sizes[0] - normalized[0]) < 0.01 && Math.abs(node.sizes[1] - normalized[1]) < 0.01
      ? node
      : { ...node, sizes: normalized }
    const children: [ChatPaneNode, ChatPaneNode] = [update(node.children[0]), update(node.children[1])]
    return children[0] === node.children[0] && children[1] === node.children[1] ? node : { ...node, children }
  }
  const root = update(layout.root)
  return root === layout.root ? layout : { ...layout, root }
}

export function closeChatLeaf(layout: ChatGridLayout, leafId: string): ChatGridLayout {
  if (layout.root.type === 'leaf') return { ...layout, root: { ...layout.root, sessionId: '' } }
  const remove = (node: ChatPaneNode): ChatPaneNode | null => {
    if (node.type === 'leaf') return node.id === leafId ? null : node
    const first = remove(node.children[0])
    const second = remove(node.children[1])
    if (!first) return second
    if (!second) return first
    return first === node.children[0] && second === node.children[1] ? node : { ...node, children: [first, second] }
  }
  const root = remove(layout.root)
  if (!root) return defaultChatGridLayout
  const leaves = chatGridLeaves(root)
  return { ...layout, root, activeLeafId: leaves.some((leaf) => leaf.id === layout.activeLeafId) ? layout.activeLeafId : leaves[0].id }
}

export function clearChatLeaf(layout: ChatGridLayout, leafId: string): ChatGridLayout {
  const root = mapNode(layout.root, leafId, (leaf) => leaf.sessionId ? { ...leaf, sessionId: '' } : leaf)
  return root === layout.root ? layout : { ...layout, root, activeLeafId: leafId }
}

export function removeChatsFromGrid(sessionIdsToRemove: Iterable<string>): ChatGridLayout {
  const removed = new Set(sessionIdsToRemove)
  removeChatViewModes(removed)
  const layout = readChatGridLayout()
  const clear = (node: ChatPaneNode): ChatPaneNode => {
    if (node.type === 'leaf') return removed.has(node.sessionId) ? { ...node, sessionId: '' } : node
    const children: [ChatPaneNode, ChatPaneNode] = [clear(node.children[0]), clear(node.children[1])]
    return children[0] === node.children[0] && children[1] === node.children[1] ? node : { ...node, children }
  }
  const next = { ...layout, root: clear(layout.root) }
  writeChatGridLayout(next)
  return next
}

export function clearMissingChats(layout: ChatGridLayout, existingSessionIds: Iterable<string>): ChatGridLayout {
  const existing = new Set(existingSessionIds)
  const clear = (node: ChatPaneNode): ChatPaneNode => {
    if (node.type === 'leaf') return node.sessionId && !existing.has(node.sessionId) ? { ...node, sessionId: '' } : node
    const children: [ChatPaneNode, ChatPaneNode] = [clear(node.children[0]), clear(node.children[1])]
    return children[0] === node.children[0] && children[1] === node.children[1] ? node : { ...node, children }
  }
  const root = clear(layout.root)
  return root === layout.root ? layout : { ...layout, root }
}
