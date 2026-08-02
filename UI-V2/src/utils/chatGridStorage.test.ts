import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  assignChatToPane,
  defaultChatGridLayout,
  readChatGridLayout,
  removeChatsFromGrid,
  subscribeChatGridLayout,
  writeChatGridLayout,
} from './chatGridStorage'

describe('chat grid storage', () => {
  const values = new Map<string, string>()

  beforeEach(() => {
    values.clear()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => values.get(key) ?? null,
        setItem: (key: string, value: string) => values.set(key, value),
      },
    })
  })

  it('round trips a four-pane layout', () => {
    const layout = {
      ...defaultChatGridLayout,
      paneCount: 4 as const,
      activePane: 3,
      sessionIds: ['one', 'two', 'three', 'four'],
      columnSizes: [40, 60],
    }
    writeChatGridLayout(layout)
    expect(readChatGridLayout()).toEqual(layout)
  })

  it('falls back when storage is malformed or unavailable', () => {
    localStorage.setItem('uam-chat-grid-layout-v1', '{')
    expect(readChatGridLayout()).toEqual(defaultChatGridLayout)
    vi.spyOn(localStorage, 'setItem').mockImplementation(() => { throw new Error('denied') })
    expect(() => writeChatGridLayout(defaultChatGridLayout)).not.toThrow()
  })

  it('swaps an existing chat into a focused pane and notifies the grid', () => {
    writeChatGridLayout({
      ...defaultChatGridLayout,
      paneCount: 4,
      sessionIds: ['one', 'two', 'three', 'four'],
    })
    const listener = vi.fn()
    const unsubscribe = subscribeChatGridLayout(listener)

    const layout = assignChatToPane('one', 2)

    expect(layout.activePane).toBe(2)
    expect(layout.sessionIds).toEqual(['three', 'two', 'one', 'four'])
    expect(listener).toHaveBeenCalledWith(layout)
    unsubscribe()
  })

  it('keeps the active pane inside the visible grid when deleted chats leave only a hidden slot', () => {
    writeChatGridLayout({
      ...defaultChatGridLayout,
      paneCount: 2,
      activePane: 0,
      sessionIds: ['delete-one', 'delete-two', '', 'hidden-chat'],
    })

    const layout = removeChatsFromGrid(['delete-one', 'delete-two'])

    expect(layout.activePane).toBe(0)
    expect(layout.sessionIds).toEqual(['', '', '', 'hidden-chat'])
  })
})
