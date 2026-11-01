import { useEffect } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, IconButton, Overlay, Switch, useOverlayState } from '../ui'
import { reportAppFailure } from '../../utils/appFailure'
import { providerMetadataForId } from '../../utils/providerMetadata'
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

  // The scan stays open after a failure, so the alert is the report and Start scan is the retry.
  useEffect(() => {
    if (memoryScanError) reportAppFailure('Memory scan did not run.', memoryScanError)
  }, [memoryScanError])

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
    <Overlay open zIndex={60} onBackdropMouseDown={() => { if (!memoryScanRunning) closeMemoryScanModal() }}>
      <div
        role="dialog"
        aria-modal="true"
        aria-label="Scan current chats"
        tabIndex={-1}
        className="uam-scan-dialog uam-mcp-dialog"
      >
        <div className="uam-scan-head">
          <div className="uam-scan-title min-w-0">Scan current chats</div>
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

          {memoryScanLoading ? (
            <SettingsLoading label="Loading eligible chats" rows={3} />
          ) : memoryScanCandidates.length === 0 ? (
            <p className="text-sm" style={{ color: 'var(--text-3)' }}>No eligible chats are available to scan right now.</p>
          ) : (
            <div className="uam-set-card">
              {memoryScanCandidates.map((candidate) => {
                const selected = selectedMemoryScanChatIds.includes(candidate.chatId)
                const providerName = providerMetadataForId(candidate.providerId).name
                return (
                  <SettingRow
                    key={candidate.chatId}
                    title={<span
                      className="uam-scan-title-row"
                      title={`${providerName} · Last scanned ${candidate.memoryLastProcessedAt || 'not yet'}`}
                    >
                      {candidate.title}
                      {candidate.alreadyFullyProcessed && <span className="uam-scan-chip">Previously processed</span>}
                    </span>}
                    description={`${candidate.folderTitle || 'Unsorted'} · ${candidate.messageCount} message${candidate.messageCount === 1 ? '' : 's'}`}
                    control={<Switch hideLabel label={`Include ${candidate.title}`} checked={selected} disabled={memoryScanRunning} onChange={() => toggleMemoryScanChat(candidate.chatId)} />}
                  />
                )
              })}
            </div>
          )}
          {!memoryScanLoading && memoryScanCandidates.length > 0 && (
            <p className="text-xs" style={{ color: 'var(--text-3)' }}>Only the chats you select are read.</p>
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
            loading={memoryScanRunning}
            onClick={() => void startMemoryScan()}
            disabled={memoryScanRunning || selectedMemoryScanChatIds.length === 0}
          >
            {memoryScanRunning ? 'Starting scan…' : `Scan ${selectedMemoryScanChatIds.length} chat${selectedMemoryScanChatIds.length === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>
    </Overlay>
  )
}
