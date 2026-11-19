import { useEffect, useRef } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, IconButton, Notice, Overlay, Swap, Switch, useOverlayState } from '../ui'
import { reportAppFailure } from '../../utils/appFailure'
import { providerMetadataForId } from '../../utils/providerMetadata'
import { SettingRow, SettingsLoading, SettingsSection } from './SettingsPrimitives'
import './provider-settings.css'

export function MemoryScanModal() {
  const overlayState = useOverlayState()
  const dialogRef = useRef<HTMLDivElement>(null)
  const isMemoryScanModalOpen = useAppStore((s) => s.isMemoryScanModalOpen)
  const memoryScanCandidates = useAppStore(useShallow((s) => s.memoryScanCandidates))
  const selectedMemoryScanChatIds = useAppStore(useShallow((s) => s.selectedMemoryScanChatIds))
  const memoryScanLoading = useAppStore((s) => s.memoryScanLoading)
  const memoryScanLoadError = useAppStore((s) => s.memoryScanLoadError)
  const openMemoryScanModal = useAppStore((s) => s.openMemoryScanModal)
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
        ref={dialogRef}
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
          <Swap swapKey={memoryScanLoading ? 'loading' : memoryScanLoadError ? 'error' : memoryScanCandidates.length === 0 ? 'empty' : 'list'}>
          {memoryScanLoading ? (
            <SettingsLoading label="Loading eligible chats" rows={3} />
          ) : memoryScanLoadError ? (
            <Notice tone="error" dismissible={false} title="Eligible chats could not be loaded."
              actions={<Button size="sm" onClick={() => { dialogRef.current?.focus(); void openMemoryScanModal() }}>Try again</Button>}>{memoryScanLoadError}</Notice>
          ) : (
            <SettingsSection
              title="Chats"
              description={`${memoryScanCandidates.length} eligible${memoryScanCandidates.length > 0 ? ` · ${selectedMemoryScanChatIds.length} selected` : ''}`}
              actions={<>
                <Button variant="ghost" size="sm" onClick={selectAllMemoryScanChats} disabled={memoryScanLoading || memoryScanRunning || memoryScanCandidates.length === 0}>
                  Select all
                </Button>
                <Button variant="ghost" size="sm" onClick={selectNoMemoryScanChats} disabled={memoryScanLoading || memoryScanRunning || memoryScanCandidates.length === 0}>
                  Select none
                </Button>
              </>}
            >
              {memoryScanCandidates.length === 0 ? (
                <p className="text-sm" style={{ color: 'var(--text-3)' }}>No eligible chats are available to scan right now.</p>
              ) : <>
                {memoryScanCandidates.map((candidate) => {
                  const selected = selectedMemoryScanChatIds.includes(candidate.chatId)
                  const providerName = providerMetadataForId(candidate.providerId).name
                  return (
                    <SettingRow
                      key={candidate.chatId}
                      title={<span
                        className="uam-scan-title-row"
                        title={`${providerName} · ${candidate.memoryLastProcessedAt ? `Last scanned ${candidate.memoryLastProcessedAt}` : 'Not scanned yet'}`}
                      >
                        {candidate.title}
                        {candidate.alreadyFullyProcessed && <span className="uam-scan-chip">Previously processed</span>}
                      </span>}
                      control={<>
                        <span className="text-xs" style={{ color: 'var(--text-3)' }}>{`${candidate.folderTitle || 'Unsorted'} · ${candidate.messageCount} message${candidate.messageCount === 1 ? '' : 's'}`}</span>
                        <Switch hideLabel label={`Include ${candidate.title}`} checked={selected} disabled={memoryScanRunning} onChange={() => toggleMemoryScanChat(candidate.chatId)} />
                      </>}
                    />
                  )
                })}
                <SettingRow title={<span className="text-xs" style={{ color: 'var(--text-3)' }}>Only the chats you select are read.</span>} />
              </>}
            </SettingsSection>
          )}
          </Swap>
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
