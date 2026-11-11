import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MemoryScanModal } from './MemoryScanModal'
import { sendToCEF } from '../../ipc/cefBridge'

vi.mock('../../ipc/cefBridge', async (importOriginal) => ({ ...(await importOriginal<typeof import('../../ipc/cefBridge')>()), isCefContext: () => true, sendToCEF: vi.fn(() => Promise.resolve({ ok: false, error: 'not in test', requestId: 'r' })) }))

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

  it('bounds a silent eligible-chat read, offers Try again and ignores a reply for a closed chooser', async () => {
    vi.useFakeTimers()
    const container = document.createElement('div')
    document.body.appendChild(container)
    const root = createRoot(container)
    try {
      let lateReply: (value: unknown) => void = () => {}
      vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { lateReply = resolve }) as never)
      act(() => root.render(<MemoryScanModal />))
      await act(async () => { void useAppStore.getState().openMemoryScanModal() })
      expect(document.body.textContent).toContain('Loading eligible chats')
      await act(async () => { vi.advanceTimersByTime(30_000); await Promise.resolve() })
      expect(useAppStore.getState().memoryScanLoading).toBe(false)
      expect(useAppStore.getState().memoryScanError).toBe('')
      expect(document.body.textContent).toContain('Eligible chats could not be loaded.')

      const candidate = { chatId: 'a', title: 'Chat A', folderTitle: '', providerId: 'codex-cli', messageCount: 2, memoryLastProcessedAt: '', alreadyFullyProcessed: false }
      vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { candidates: [candidate] }, requestId: 'r' })
      await act(async () => { Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Try again')!.click(); await Promise.resolve() })
      expect(document.body.textContent).toContain('Chat A')

      // The first read answers after its deadline and a retry; it must not replace the newer list.
      await act(async () => { lateReply({ ok: true, data: { candidates: [] }, requestId: 'old' }); await Promise.resolve() })
      expect(useAppStore.getState().memoryScanCandidates).toHaveLength(1)
    } finally {
      act(() => root.unmount())
      container.remove()
      useAppStore.getState().closeMemoryScanModal()
      vi.useRealTimers()
    }
  })
})
