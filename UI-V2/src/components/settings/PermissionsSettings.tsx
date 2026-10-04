import { useEffect, useState } from 'react'
import { COMPUTER_USE_ENABLED, MOBILE_COMPANION_ENABLED } from '../../config/buildFeatures'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button } from '../ui'

interface PermissionState {
  platform: string
  microphone: string
  speechRecognition: string
  localNetwork: string
}
interface ComputerPermissions {
  screenRecording: boolean
  accessibility: boolean
  error?: string
}
const labels: Record<string, string> = {
  allowed: 'Allowed', denied: 'Denied', restricted: 'Restricted', not_requested: 'Not requested',
  system_managed: 'Check system settings', unsupported: 'Unavailable',
}

export function PermissionsSettings() {
  const [permissions, setPermissions] = useState<PermissionState | null>(null)
  const [computer, setComputer] = useState<ComputerPermissions | null>(null)
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const refresh = async () => {
    setBusy(true)
    setMessage('')
    try {
      const response = await sendToCEF<PermissionState>({ action: 'getAppPermissions' })
      if (!response.ok || !response.data) throw new Error(response.error || 'Permission checks require the desktop app.')
      setPermissions(response.data)
      if (COMPUTER_USE_ENABLED) {
        const result = await sendToCEF<ComputerPermissions>({ action: 'checkComputerUsePermissions' })
        if (!result.ok || !result.data) throw new Error(result.error || 'Could not check Computer Use permissions.')
        setComputer(result.data)
        if (result.data.error) setMessage(result.data.error)
      }
    } catch (error) { setMessage(error instanceof Error ? error.message : 'Could not check permissions.') }
    finally { setBusy(false) }
  }
  useEffect(() => { void refresh() }, [])
  const act = async (permission: string, helper: boolean, request: boolean) => {
    setBusy(true)
    setMessage('')
    try {
      const response = await sendToCEF({ action: helper
        ? request ? 'requestComputerUsePermission' : 'openComputerUseSystemSettings'
        : request ? 'requestAppPermission' : 'openAppPermissionSettings', payload: { permission } })
      if (!response.ok) throw new Error(response.error || 'Could not open permission settings.')
      setMessage(request ? 'Complete any system prompt, then refresh. If no prompt appears, check system settings.' : '')
    } catch (error) { setMessage(error instanceof Error ? error.message : 'Could not open permission settings.') }
    finally { setBusy(false) }
  }
  const rows = [
    ...(MOBILE_COMPANION_ENABLED ? [{ id: 'localNetwork', label: 'Local Network', purpose: 'Phone access', status: permissions?.localNetwork, helper: false }] : []),
    { id: 'microphone', label: 'Microphone', purpose: 'Voice input', status: permissions?.microphone, helper: false },
    { id: 'speechRecognition', label: 'Speech Recognition', purpose: 'Apple dictation', status: permissions?.speechRecognition, helper: false },
    ...(COMPUTER_USE_ENABLED ? [
      { id: 'screenRecording', label: 'Screen Recording', purpose: 'Computer Use', status: computer ? computer.screenRecording ? 'allowed' : 'denied' : undefined, helper: true },
      { id: 'accessibility', label: 'Accessibility', purpose: 'Computer Use', status: computer ? computer.accessibility ? 'allowed' : 'denied' : undefined, helper: true },
    ] : []),
  ]
  return <section className="space-y-4 text-xs">
    <div className="flex items-center justify-between gap-3">
      <h3 className="font-semibold" style={{ color: 'var(--text)' }}>App permissions</h3>
      <Button size="sm" variant="secondary" disabled={busy} onClick={() => void refresh()}>Refresh</Button>
    </div>
    <div className="overflow-x-auto">
      <table className="w-full text-left">
        <thead style={{ color: 'var(--text-2)' }}><tr>
          <th className="py-2 font-medium">Permission</th><th className="py-2 font-medium">Status</th><th className="py-2 font-medium text-right">Actions</th>
        </tr></thead>
        <tbody>{rows.map((row) => <tr key={row.id} style={{ borderTop: '1px solid var(--border)' }}>
          <td className="py-3 pr-3"><div style={{ color: 'var(--text)' }}>{row.label}</div><div className="mt-1" style={{ color: 'var(--text-3)' }}>{row.purpose}</div></td>
          <td className="py-3 pr-3" style={{ color: row.status === 'allowed' ? 'var(--green)' : row.status === 'denied' ? 'var(--red)' : 'var(--text-2)' }}>{row.status ? labels[row.status] ?? 'Not checked' : 'Not checked'}</td>
          <td className="py-3"><div className="flex justify-end gap-2">
            {row.status !== 'allowed' && <Button size="sm" variant="secondary" disabled={busy || permissions?.platform !== 'macos' || row.status === 'restricted' || row.status === 'unsupported'} onClick={() => void act(row.id, row.helper, true)}>Request</Button>}
            <Button size="sm" variant="ghost" disabled={busy || permissions?.platform !== 'macos'} onClick={() => void act(row.id, row.helper, false)}>System settings</Button>
          </div></td>
        </tr>)}</tbody>
      </table>
    </div>
    {MOBILE_COMPANION_ENABLED && <p style={{ color: 'var(--text-2)' }}>macOS does not expose the saved Local Network decision. Check it in System Settings.</p>}
    {message && <p role="status" style={{ color: 'var(--text-2)' }}>{message}</p>}
  </section>
}
