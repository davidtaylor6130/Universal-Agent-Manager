import { useEffect } from 'react'
import { useAppStore } from '../../store/useAppStore'

export function MemoryScanModal() {
  const {
    isMemoryScanModalOpen,
    memoryScanCandidates,
    selectedMemoryScanChatIds,
    memoryScanLoading,
    memoryScanRunning,
    memoryScanError,
    closeMemoryScanModal,
    toggleMemoryScanChat,
    selectAllMemoryScanChats,
    selectNoMemoryScanChats,
    startMemoryScan,
  } = useAppStore()

  useEffect(() => {
    if (!isMemoryScanModalOpen) {
      return
    }

    const handler = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && !memoryScanRunning) {
        closeMemoryScanModal()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [closeMemoryScanModal, isMemoryScanModalOpen, memoryScanRunning])

  if (!isMemoryScanModalOpen) {
    return null
  }

  return (
    <div
      className="fixed inset-0 z-[60] flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(event) => {
        if (event.target === event.currentTarget && !memoryScanRunning) {
          closeMemoryScanModal()
        }
      }}
    >
      <div
        className="rounded-2xl shadow-2xl w-full max-w-4xl mx-4 animate-slide-in overflow-hidden flex flex-col"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
          maxHeight: 'calc(100vh - 2rem)',
        }}
      >
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <div>
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
              Scan Current Chats
            </div>
            <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
              Backfill durable memory from existing chat history.
            </div>
          </div>
          <button
            type="button"
            onClick={closeMemoryScanModal}
            disabled={memoryScanRunning}
            style={{
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: memoryScanRunning ? 'not-allowed' : 'pointer',
              fontFamily: 'inherit',
              fontSize: 12,
              opacity: memoryScanRunning ? 0.5 : 1,
            }}
          >
            ✕
          </button>
        </div>

        <div className="p-5 md:p-6 overflow-y-auto min-h-0">
          <div className="flex items-center justify-between gap-3 mb-4">
            <div className="text-sm" style={{ color: 'var(--text)' }}>
              {memoryScanCandidates.length} eligible chat{memoryScanCandidates.length === 1 ? '' : 's'}
            </div>
            <div className="flex items-center gap-2">
              <button
                type="button"
                onClick={selectAllMemoryScanChats}
                className="px-2 py-1 rounded-md text-xs"
                style={{ background: 'transparent', color: 'var(--text-2)', border: '1px solid var(--border)' }}
              >
                Select all
              </button>
              <button
                type="button"
                onClick={selectNoMemoryScanChats}
                className="px-2 py-1 rounded-md text-xs"
                style={{ background: 'transparent', color: 'var(--text-2)', border: '1px solid var(--border)' }}
              >
                Select none
              </button>
            </div>
          </div>

          {memoryScanError && (
            <div className="mb-4 rounded-md px-3 py-2 text-xs" style={{ background: 'color-mix(in srgb, var(--red) 14%, transparent)', color: 'var(--red)', border: '1px solid color-mix(in srgb, var(--red) 35%, var(--border))' }}>
              {memoryScanError}
            </div>
          )}

          {memoryScanLoading ? (
            <div className="text-sm" style={{ color: 'var(--text-3)' }}>
              Loading eligible chats...
            </div>
          ) : memoryScanCandidates.length === 0 ? (
            <div
              className="rounded-xl p-6 text-sm"
              style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text-3)' }}
            >
              No eligible chats are available to scan right now.
            </div>
          ) : (
            <div className="space-y-3">
              {memoryScanCandidates.map((candidate) => {
                const selected = selectedMemoryScanChatIds.includes(candidate.chatId)
                return (
                  <label
                    key={candidate.chatId}
                    className="flex items-start gap-3 rounded-xl p-4"
                    style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', cursor: 'pointer' }}
                  >
                    <input
                      type="checkbox"
                      checked={selected}
                      onChange={() => toggleMemoryScanChat(candidate.chatId)}
                      style={{ marginTop: 2 }}
                    />
                    <div className="min-w-0 flex-1">
                      <div className="flex items-center gap-2 flex-wrap">
                        <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                          {candidate.title}
                        </div>
                        {candidate.alreadyFullyProcessed && (
                          <span className="text-[10px] px-2 py-0.5 rounded-full" style={{ color: 'var(--text-2)', background: 'var(--surface)' }}>
                            Previously processed
                          </span>
                        )}
                      </div>
                      <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
                        {candidate.folderTitle || 'Unsorted'} • {candidate.providerId} • {candidate.messageCount} message{candidate.messageCount === 1 ? '' : 's'}
                      </div>
                      <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
                        Last memory scan: {candidate.memoryLastProcessedAt || 'Never'}
                      </div>
                    </div>
                  </label>
                )
              })}
            </div>
          )}
        </div>

        <div
          className="flex items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <button
            type="button"
            onClick={closeMemoryScanModal}
            disabled={memoryScanRunning}
            className="px-4 py-1.5 rounded-md text-xs"
            style={{
              background: 'transparent',
              color: 'var(--text-2)',
              border: '1px solid var(--border)',
              cursor: memoryScanRunning ? 'not-allowed' : 'pointer',
              opacity: memoryScanRunning ? 0.5 : 1,
            }}
          >
            Cancel
          </button>
          <button
            type="button"
            onClick={() => void startMemoryScan()}
            disabled={memoryScanRunning || selectedMemoryScanChatIds.length === 0}
            className="px-4 py-1.5 rounded-md text-xs font-medium"
            style={{
              background: 'var(--accent)',
              color: '#fff',
              border: 'none',
              cursor: memoryScanRunning || selectedMemoryScanChatIds.length === 0 ? 'not-allowed' : 'pointer',
              opacity: memoryScanRunning || selectedMemoryScanChatIds.length === 0 ? 0.5 : 1,
            }}
          >
            {memoryScanRunning ? 'Starting scan...' : `Scan ${selectedMemoryScanChatIds.length} chat${selectedMemoryScanChatIds.length === 1 ? '' : 's'}`}
          </button>
        </div>
      </div>
    </div>
  )
}
