import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  assignChatToPane, chatGridLeaves, chatPaneColors, clearChatLeaf, closeChatLeaf, defaultChatGridLayout, MAX_CHAT_PANES,
  readChatGridLayout, readChatViewMode, removeChatsFromGrid, resizeChatSplit, setChatInLeaf, splitChatLeaf,
  subscribeChatGridLayout, writeChatGridLayout,
  writeChatViewMode,
} from './chatGridStorage'

describe('chat grid storage', () => {
  const values = new Map<string, string>()
  beforeEach(() => {
    values.clear()
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
    } })
  })

  it('round trips an asymmetric recursive split tree', () => {
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    layout = splitChatLeaf(layout, layout.activeLeafId, 'vertical')
    const split = layout.root.type === 'split' ? layout.root : null
    layout = resizeChatSplit(layout, split!.id, [35, 65])
    writeChatGridLayout(layout)
    expect(readChatGridLayout()).toEqual(layout)
    expect(chatGridLeaves(layout.root)).toHaveLength(3)
  })

  it('migrates the legacy four-pane layout with assignments and sizes', () => {
    localStorage.setItem('uam-chat-grid-layout-v1', JSON.stringify({
      paneCount: 4, activePane: 3, sessionIds: ['one', 'two', 'three', 'four'],
      columnSizes: [40, 60], rowSizes: [30, 70],
    }))
    const layout = readChatGridLayout()
    expect(chatGridLeaves(layout.root).map((leaf) => leaf.sessionId)).toEqual(['one', 'three', 'two', 'four'])
    expect(layout.activeLeafId).toBe('leaf-4')
    expect(layout.root).toMatchObject({ type: 'split', direction: 'horizontal', sizes: [40, 60] })
  })

  it('falls back when storage is malformed or unavailable', () => {
    localStorage.setItem('uam-chat-grid-layout-v2', '{')
    expect(readChatGridLayout()).toEqual(defaultChatGridLayout)
    vi.spyOn(localStorage, 'setItem').mockImplementation(() => { throw new Error('denied') })
    expect(() => writeChatGridLayout(defaultChatGridLayout)).not.toThrow()
  })

  it('persists terminal-first mode per chat and removes it with deleted chats', () => {
    writeChatViewMode('chat-1', 'cli')
    expect(readChatViewMode('chat-1')).toBe('cli')
    removeChatsFromGrid(['chat-1'])
    expect(readChatViewMode('chat-1')).toBeNull()
  })

  it('swaps an existing chat into a leaf and notifies subscribers', () => {
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    const [first, second] = chatGridLeaves(layout.root)
    layout = setChatInLeaf(setChatInLeaf(layout, 'one', first.id), 'two', second.id)
    writeChatGridLayout(layout)
    const listener = vi.fn()
    const unsubscribe = subscribeChatGridLayout(listener)
    const next = assignChatToPane('one', second.id)
    expect(chatGridLeaves(next.root).map((leaf) => leaf.sessionId)).toEqual(['two', 'one'])
    expect(listener).toHaveBeenCalledWith(next)
    unsubscribe()
  })

  it('clears deleted chats, collapses closed parents, and enforces the pane cap', () => {
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(layout, 'delete', leaves[0].id)
    writeChatGridLayout(layout)
    expect(chatGridLeaves(removeChatsFromGrid(['delete']).root)[0].sessionId).toBe('')
    expect(chatGridLeaves(closeChatLeaf(layout, leaves[1].id).root)).toHaveLength(1)
    while (chatGridLeaves(layout.root).length < MAX_CHAT_PANES) layout = splitChatLeaf(layout, layout.activeLeafId, 'vertical')
    expect(splitChatLeaf(layout, layout.activeLeafId, 'horizontal')).toBe(layout)
    expect(chatGridLeaves(layout.root)).toHaveLength(9)
    expect(chatPaneColors).toHaveLength(MAX_CHAT_PANES)
  })

  it('clears only the requested leaf without collapsing or swapping through another empty pane', () => {
    let layout = splitChatLeaf(defaultChatGridLayout, 'leaf-1', 'horizontal')
    layout = splitChatLeaf(layout, layout.activeLeafId, 'vertical')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(layout, 'one', leaves[0].id)
    layout = setChatInLeaf(layout, 'two', leaves[2].id)

    const next = clearChatLeaf(layout, leaves[0].id)

    expect(chatGridLeaves(next.root).map((leaf) => leaf.sessionId)).toEqual(['', '', 'two'])
    expect(chatGridLeaves(next.root)).toHaveLength(3)
    expect(next.activeLeafId).toBe(leaves[0].id)
  })
})
