import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MemoryScanModal } from './MemoryScanModal'

describe('MemoryScanModal', () => {
  it('lists chats in one stable SettingsSection with Select all', () => {
    const candidate = (chatId: string) => ({ chatId, title: chatId, folderTitle: '', providerId: 'codex-cli', messageCount: 2, memoryLastProcessedAt: '', alreadyFullyProcessed: false })
    const selectAllMemoryScanChats = vi.fn(() => useAppStore.setState({ selectedMemoryScanChatIds: ['a', 'b'] }))
    useAppStore.setState({
      isMemoryScanModalOpen: true, memoryScanLoading: false, memoryScanRunning: false, memoryScanError: '',
      memoryScanCandidates: [candidate('a'), candidate('b')] as never, selectedMemoryScanChatIds: [],
      selectAllMemoryScanChats,
    })
    const container = document.createElement('div')
    document.body.appendChild(container)
    const root = createRoot(container)
    try {
      act(() => root.render(<MemoryScanModal />))
      const section = document.body.querySelector('section[aria-label="Chats"]')!
      expect(section.querySelector('.uam-set-section__desc')?.textContent).toBe('2 eligible · 0 selected')
      expect(section.querySelectorAll('.uam-set-row')).toHaveLength(3)
      act(() => Array.from(section.querySelectorAll('button')).find((button) => button.textContent?.trim() === 'Select all')!.click())
      expect(selectAllMemoryScanChats).toHaveBeenCalledTimes(1)
      // The region name stays fixed while the count changes.
      expect(document.body.querySelector('section[aria-label="Chats"] .uam-set-section__desc')?.textContent).toBe('2 eligible · 2 selected')
    } finally {
      act(() => root.unmount())
      container.remove()
    }
  })
})
