import { beforeEach, describe, expect, it, vi } from 'vitest'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from './chatGridStorage'

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
})
