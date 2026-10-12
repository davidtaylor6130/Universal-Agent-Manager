import { useEffect } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, IconButton, Notice, useOverlayState } from '../ui'
import { SettingRow, SettingsLoading } from './SettingsPrimitives'
import './provider-settings.css'

export function MemoryScanModal() {
  const overlayState = useOverlayState()
  const isMemoryScanModalOpen = useAppStore((s) => s.isMemoryScanModalOpen)
  const memoryScanCandidates = useAppStore(useShallow((s) => s.memoryScanCandidates))
  const selectedMemoryScanChatIds = useAppStore(useShallow((s) => s.selectedMemoryScanChatIds))
  const memoryScanLoading = useAppStore((s) => s.memoryScanLoading)
  const memoryScanRunning = useAppStore((s) => s.memoryScanRunning)
  const memoryScanError = useAppStore((s) => s.memoryScanError)
  const closeMemoryScanModal = useAppStore((s) => s.closeMemoryScanModal)
  const toggleMemoryScanChat = useAppStore((s) => s.toggleMemoryScanChat)
  const selectAllMemoryScanChats = useAppStore((s) => s.selectAllMemoryScanChats)
  const selectNoMemoryScanChats = useAppStore((s) => s.selectNoMemoryScanChats)
  const startMemoryScan = useAppStore((s) => s.startMemoryScan)

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

  if (!isMemoryScanModalOpen && overlayState === 'open') {
    return null
  }

  return (
    <div
      className="uam-overlay uam-overlay--dim fixed inset-0 z-[60] flex items-center justify-center"
      data-state={overlayState}
      onClick={(event) => {
        if (event.target === event.currentTarget && !memoryScanRunning) {
          closeMemoryScanModal()
        }
      }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label="Scan current chats"
        tabIndex={-1}
        className="uam-scan-dialog uam-mcp-dialog"
      >
        <div className="uam-scan-head">
          <div className="min-w-0">
            <div className="uam-scan-title">Scan current chats</div>
            <div className="uam-prov-sub">Add durable memory from existing chat history. Only the chats you select are read.</div>
          </div>
          <IconButton
            icon={<X size={16} />}
            label="Close scan modal"
            onClick={closeMemoryScanModal}
            disabled={memoryScanRunning}
          />
        </div>

        <div className="uam-scan-body">
          <div className="uam-scan-toolbar">
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>
              {memoryScanCandidates.length} eligible chat{memoryScanCandidates.length === 1 ? '' : 's'}
              {memoryScanCandidates.length > 0 && <span style={{ color: 'var(--text-3)' }}> · {selectedMemoryScanChatIds.length} selected</span>}
            </div>
            <div className="flex items-center gap-2">
              <Button variant="ghost" size="sm" onClick={selectAllMemoryScanChats} disabled={memoryScanLoading || memoryScanRunning || memoryScanCandidates.length === 0}>
                Select all
              </Button>
              <Button variant="ghost" size="sm" onClick={selectNoMemoryScanChats} disabled={memoryScanLoading || memoryScanRunning || memoryScanCandidates.length === 0}>
                Select none
              </Button>
            </div>
          </div>

          {memoryScanError && (
            <Notice tone="error" dismissLabel="Dismiss scan error">{memoryScanError}</Notice>
          )}

          {memoryScanLoading ? (
            <SettingsLoading label="Loading eligible chats" rows={3} />
          ) : memoryScanCandidates.length === 0 ? (
            <div className="uam-store-empty">
              <div className="uam-store-empty__desc">No eligible chats are available to scan right now.</div>
            </div>
          ) : (
            <div className="uam-set-card">
              {memoryScanCandidates.map((candidate) => {
                const selected = selectedMemoryScanChatIds.includes(candidate.chatId)
                const inputId = `memory-scan-${candidate.chatId}`
                return (
                  <SettingRow
                    key={candidate.chatId}
                    htmlFor={inputId}
                    title={<span className="uam-scan-title-row">
                      {candidate.title}
                      {candidate.alreadyFullyProcessed && <span className="uam-scan-chip">Previously processed</span>}
                    </span>}
                    description={`${candidate.folderTitle || 'Unsorted'} · ${candidate.messageCount} message${candidate.messageCount === 1 ? '' : 's'}`}
                    status={<span>Provider <span className="uam-prov-detail">{candidate.providerId}</span> · Last scanned {candidate.memoryLastProcessedAt || 'never'}</span>}
                    control={<input
                      id={inputId}
                      type="checkbox"
                      checked={selected}
                      disabled={memoryScanRunning}
                      onChange={() => toggleMemoryScanChat(candidate.chatId)}
                    />}
                  />
                )
              })}
            </div>
          )}
        </div>

        <div className="uam-scan-foot">
          <Button
            variant="secondary"
            size="sm"
            onClick={closeMemoryScanModal}
            disabled={memoryScanRunning}
          >
            Cancel
          </Button>
          <Button
            variant="primary"
            size="sm"
            onClick={() => void startMemoryScan()}
            disabled={memoryScanRunning || selectedMemoryScanChatIds.length === 0}
          >
            {memoryScanRunning ? 'Starting scan…' : `Scan ${selectedMemoryScanChatIds.length} chat${selectedMemoryScanChatIds.length === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>
    </div>
  )
}
