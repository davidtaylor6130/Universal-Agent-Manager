import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { createRequestId, isCompanionContext, sendToCEF, type CEFRequest, type CEFResponse } from '../../ipc/cefBridge'
import { Button, ConfirmDialog } from '../ui'
import { SettingRow, SettingsLoading, SettingsSection } from './SettingsPrimitives'
import { observesFolderChoice, parseDataFolderSettings, readUnconfirmedFolderChoice, retainUnconfirmedFolderChoice,
  readUnconfirmedFolderRestart, retainUnconfirmedFolderRestart,
  type DataFolderSettings, type UnconfirmedFolderChoice } from './dataFolderSettings'

export interface DataFolderSettingsHandle { requestLeave: (next: () => void) => void }

async function folderRequest<T = unknown>(request: CEFRequest): Promise<CEFResponse<T>> {
  let timeout: ReturnType<typeof setTimeout> | undefined
  try {
    return await Promise.race([sendToCEF<T>(request, false), new Promise<CEFResponse<T>>(resolve => {
      timeout = setTimeout(() => resolve({ ok: false, error: 'The folder request timed out. Its outcome may be unknown; refresh the folder state.' }), 15000)
    })])
  } finally { clearTimeout(timeout) }
}

export const DataFolderSettingsSection = forwardRef<DataFolderSettingsHandle>(function DataFolderSettingsSection(_props, ref) {
  const [settings, setSettings] = useState<DataFolderSettings | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [candidate, setCandidate] = useState('')
  const [unconfirmed, setUnconfirmed] = useState(readUnconfirmedFolderChoice)
  const [restartConfirmation, setRestartConfirmation] = useState(false)
  const [restartAttempt, setRestartAttempt] = useState(readUnconfirmedFolderRestart)
  const busyRef = useRef(false)
  const alive = useRef(true)
  const companion = isCompanionContext()
  const remember = (value: UnconfirmedFolderChoice | null) => {
    retainUnconfirmedFolderChoice(value)
    if (alive.current) setUnconfirmed(value)
  }
  const accept = (next: DataFolderSettings) => {
    if (!alive.current) return
    setSettings(next)
    const retained = readUnconfirmedFolderChoice()
    if (retained && observesFolderChoice(next, retained)) remember(null)
  }
  const start = () => {
    if (busyRef.current) return false
    busyRef.current = true; setBusy(true); setError('')
    return true
  }
  const finish = () => { busyRef.current = false; if (alive.current) setBusy(false) }
  const refresh = async () => {
    if (!start()) return
    try {
      const response = await folderRequest({ action: 'getDataFolderSettings' })
      const next = response.ok ? parseDataFolderSettings(response.data) : null
      if (next) accept(next)
      else if (alive.current) setError(response.error || 'The current folder state could not be confirmed. No choice was applied.')
    } catch { if (alive.current) setError('The current folder state could not be confirmed. No choice was applied.') }
    finally { finish() }
  }
  useEffect(() => {
    alive.current = true
    if (!companion) void refresh()
    return () => { alive.current = false }
  }, [companion])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (!busyRef.current && !candidate && !restartConfirmation) next() } }))
  const choose = async () => {
    if (!settings || !settings.canChange || unconfirmed || restartAttempt || !start()) return
    try {
      // Native pickers may remain open for as long as the user needs; only data operations time out.
      const response = await sendToCEF<{ selectedPath: string }>({ action: 'browseFolderDirectory', payload: { currentValue: settings.currentFolder } }, false)
      if (!alive.current) return
      if (!response.ok) setError(response.error || 'The folder picker could not be opened.')
      else if (typeof response.data?.selectedPath === 'string' && response.data.selectedPath) setCandidate(response.data.selectedPath)
    } catch { if (alive.current) setError('The folder picker could not be opened.') }
    finally { finish() }
  }
  const change = async (action: UnconfirmedFolderChoice['action']) => {
    if (!settings || !settings.canChange || unconfirmed || restartAttempt || !start()) return
    const attempt = { action, folder: action === 'chooseDataFolder' ? candidate : '', expectedRevision: settings.revision, operationId: createRequestId('data-folder') }
    remember(attempt)
    try {
      const response = await folderRequest<{ committed: boolean; settings: unknown; error: string }>({ action,
        payload: { expectedRevision: attempt.expectedRevision, operationId: attempt.operationId, ...(action === 'chooseDataFolder' ? { folder: attempt.folder } : {}) } })
      const next = response.ok ? parseDataFolderSettings(response.data?.settings) : null
      if (next && typeof response.data?.committed === 'boolean') {
        accept(next)
        if (next.transitionState !== 'publicationUnknown' && (!response.data.committed || next.lastOperationId === attempt.operationId)) remember(null)
        if (alive.current) {
          if (response.data.committed || next.transitionState === 'publicationUnknown') setCandidate('')
          else setError(response.data.error || 'The folder choice was not confirmed. Your current folder is still in use.')
        }
      } else if (alive.current) { setCandidate(''); setError(response.error || 'The save outcome is unknown. Refresh the folder state before another change.') }
    } catch { if (alive.current) { setCandidate(''); setError('The save outcome is unknown. Refresh the folder state before another change.') } }
    finally { finish() }
  }
  const restart = async () => {
    if (!settings?.restartReady || !settings.canChange || !settings.pendingFolder || unconfirmed || restartAttempt || !start()) return
    const attempt = { revision: settings.revision, folder: settings.pendingFolder }
    retainUnconfirmedFolderRestart(attempt); setRestartAttempt(attempt)
    try {
      const response = await folderRequest({ action: 'restartToApplyDataFolder', payload: { expectedRevision: attempt.revision } })
      if (alive.current) {
        setRestartConfirmation(false)
        if (!response.ok) setError(response.error || 'The restart request was not confirmed. Reopen the application before trying again.')
      }
    } catch { if (alive.current) { setRestartConfirmation(false); setError('The restart outcome is unknown. Reopen the application before trying again.') } }
    finally { finish() }
  }
  if (companion) return <SettingsSection title="Application data folder"><SettingRow title="Desktop folder choice" description="Change the data folder on the main computer." /></SettingsSection>
  if (!settings && busy) return <SettingsLoading label="Loading application data folder" rows={1} />
  const blocked = busy || !settings?.canChange || Boolean(unconfirmed) || Boolean(restartAttempt)
  return <SettingsSection title="Application data folder" actions={<Button size="sm" disabled={busy} onClick={() => void refresh()}>Refresh folder state</Button>}>
    {settings && <SettingRow title="Current folder" description={settings.externalOverride ? 'Set by the startup environment.' : 'Chats and application history are stored here.'}
      control={<Button size="sm" disabled={blocked || Boolean(settings.pendingFolder)} onClick={() => void choose()}>Choose folder…</Button>}>
      <p className="text-sm break-all" style={{ color: 'var(--text-2)' }}>{settings.currentFolder}</p>
    </SettingRow>}
    {settings?.pendingFolder && <SettingRow title={settings.transitionState === 'publicationUnknown' ? 'Unconfirmed folder choice' : 'Saved folder choice'}
      description="Your current folder remains in use."
      control={<div className="flex flex-wrap gap-2"><Button size="sm" disabled={blocked} onClick={() => void change('cancelDataFolderChoice')}>Cancel folder change</Button>
        <Button size="sm" disabled={blocked || !settings.restartReady} onClick={() => setRestartConfirmation(true)} title="Native validation must permit the guarded transfer before restart is available.">Restart to use folder</Button></div>}>
      <p className="text-sm break-all" style={{ color: 'var(--text-2)' }}>{settings.pendingFolder}</p>
    </SettingRow>}
    {settings && <p role="status" aria-live="polite" className="p-3 text-xs" style={{ color: 'var(--text-2)' }}>{settings.message}</p>}
    {unconfirmed && <p role="status" aria-live="polite" className="p-3 text-xs" style={{ color: 'var(--text-2)' }}>Unconfirmed {unconfirmed.action === 'chooseDataFolder' ? 'folder choice' : 'cancellation'}{unconfirmed.folder ? `: ${unconfirmed.folder}` : ''}. The request may have reached UAM. Refresh its state before making another change.</p>}
    {restartAttempt && <p role="status" aria-live="polite" className="p-3 text-xs" style={{ color: 'var(--text-2)' }}>Restart requested for {restartAttempt.folder}. This does not confirm migration. Folder changes remain held until the application reopens.</p>}
    {error && <p role="alert" className="p-3 text-xs" style={{ color: 'var(--text)' }}><strong>Error: </strong>{error}</p>}
    <ConfirmDialog open={Boolean(candidate)} title="Save this data-folder choice?" tone="primary" confirmLabel="Save folder choice" busy={busy}
      error={error || undefined} onCancel={() => { if (!busyRef.current) setCandidate('') }} onConfirm={() => { if (!unconfirmed) void change('chooseDataFolder') }}>
      <p className="break-all">{candidate}</p>
      <p className="mt-2">This saves a pending choice. Your current folder remains in use. Restart-to-apply becomes available only when native validation permits the guarded transfer.</p>
    </ConfirmDialog>
    <ConfirmDialog open={restartConfirmation} title="Restart to use this folder?" tone="primary" confirmLabel="Restart and transfer" busy={busy}
      error={error || undefined} onCancel={() => { if (!busyRef.current) setRestartConfirmation(false) }} onConfirm={() => void restart()}>
      <p className="break-all">From: {settings?.currentFolder}</p><p className="mt-2 break-all">To: {settings?.pendingFolder}</p>
      <p className="mt-2">UAM must stop all writers, own both folders and verify the copy before using the destination. The original profile stays retained for recovery. Unconfirmed ownership or publication holds the transition.</p>
    </ConfirmDialog>
  </SettingsSection>
})
