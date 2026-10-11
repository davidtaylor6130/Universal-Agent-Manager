import { useEffect, useState } from 'react'
import { COMPUTER_USE_ENABLED, MOBILE_COMPANION_ENABLED } from '../../config/buildFeatures'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button, StatusDot, type StatusTone } from '../ui'
import './provider-settings.css'

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
  const toneFor = (status?: string): StatusTone => status === 'allowed' ? 'success' : status === 'denied' || status === 'restricted' ? 'error' : status === 'system_managed' ? 'warning' : 'neutral'
  return <section className="space-y-4 text-xs">
    <div className="flex items-center justify-between gap-3">
      <div>
        <h3 className="uam-prov-heading">App permissions</h3>
        <p className="uam-prov-sub">macOS asks for these when a feature needs them. Change them here or in System Settings.</p>
      </div>
      <Button size="sm" variant="secondary" disabled={busy} onClick={() => void refresh()}>Refresh</Button>
    </div>
    <div className="uam-prov-card overflow-x-auto">
      <table className="w-full text-left">
        <thead><tr style={{ borderBottom: '1px solid var(--border)' }}>
          <th className="uam-prov-label px-3 py-2 font-medium">Permission</th><th className="uam-prov-label px-3 py-2 font-medium">Status</th><th className="uam-prov-label px-3 py-2 font-medium text-right">Actions</th>
        </tr></thead>
        <tbody>{rows.map((row) => <tr key={row.id} style={{ borderTop: '1px solid var(--border)' }}>
          <td className="px-3 py-3"><div style={{ color: 'var(--text)' }}>{row.label}</div><div className="mt-1 uam-prov-sub">{row.purpose}</div></td>
          <td className="px-3 py-3"><span className="uam-prov-status" data-tone={toneFor(row.status)}><StatusDot tone={toneFor(row.status)}/>{row.status ? labels[row.status] ?? 'Not checked' : busy ? 'Checking…' : 'Not checked'}</span></td>
          <td className="py-3"><div className="flex justify-end gap-2">
            {row.status !== 'allowed' && <Button size="sm" variant="secondary" disabled={busy || permissions?.platform !== 'macos' || row.status === 'restricted' || row.status === 'unsupported'} onClick={() => void act(row.id, row.helper, true)}>Request</Button>}
            <Button size="sm" variant="ghost" disabled={busy || permissions?.platform !== 'macos'} onClick={() => void act(row.id, row.helper, false)}>System settings</Button>
          </div></td>
        </tr>)}</tbody>
      </table>
    </div>
    {MOBILE_COMPANION_ENABLED && <p className="uam-prov-sub">macOS does not expose the saved Local Network decision. Check it in System Settings.</p>}
    {message && <p role="status" className="uam-prov-sub">{message}</p>}
  </section>
}
