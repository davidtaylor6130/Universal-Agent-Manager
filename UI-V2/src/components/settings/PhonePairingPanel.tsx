import { useEffect, useRef, useState } from 'react'
import { QRCodeSVG } from 'qrcode.react'
import { QrCode } from 'lucide-react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { Button } from '../ui'
import { SettingRow } from './SettingsPrimitives'

interface PhonePairingPanelProps {
  url: string
  enabled: boolean
}

/** The desktop reveals its existing companion credential only after a deliberate action. */
export function PhonePairingPanel({ url, enabled }: PhonePairingPanelProps) {
  const [payload, setPayload] = useState('')
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const generation = useRef(0)
  const nativePhoneUrl = (() => {
    try {
      const parsed = new URL(url)
      return parsed.protocol === 'https:' && !parsed.username && !parsed.password && !parsed.search && !parsed.hash
        && ['', '/', '/companion', '/companion/', '/api', '/api/'].includes(parsed.pathname)
    } catch { return false }
  })()

  useEffect(() => {
    generation.current += 1
    setPayload('')
    setBusy(false)
    setMessage('')
    return () => { generation.current += 1 }
  }, [url, enabled])

  useEffect(() => {
    const hidden = () => {
      if (document.visibilityState === 'hidden') {
        generation.current += 1
        setPayload('')
        setBusy(false)
      }
    }
    document.addEventListener('visibilitychange', hidden)
    return () => document.removeEventListener('visibilitychange', hidden)
  }, [])

  useEffect(() => {
    if (!payload) return
    const timer = window.setTimeout(() => { generation.current += 1; setPayload('') }, 120000)
    return () => window.clearTimeout(timer)
  }, [payload])

  const show = async () => {
    if (!enabled || !nativePhoneUrl || busy || isCompanionContext()) return
    const request = ++generation.current
    setBusy(true)
    setMessage('')
    try {
      const response = await sendToCEF<{ token?: string; url?: string; certificate?: string }>({ action: 'getCompanionToken' })
      if (generation.current !== request) return
      if (!response.ok || !/^[a-fA-F0-9]{64}$/.test(response.data?.token ?? '')) {
        setMessage('Pairing code unavailable. Check Phone Access configuration.')
        return
      }
      if (response.data?.url !== url) { setMessage('The phone address changed. Wait for phone access to become ready, then try again.'); return }
      const certificate = response.data.certificate
      if (certificate !== undefined && (certificate.length > 11000 || !/^[A-Za-z0-9+/]+={0,2}$/.test(certificate))) { setMessage('Pairing certificate unavailable. Retry phone access.'); return }
      setPayload(JSON.stringify({ ...(certificate ? { certificate } : {}), type: 'uam-companion', version: 1, url, token: response.data!.token, name: 'Main Universal Agent Manager computer' }))
    } catch {
      if (generation.current === request) setMessage('Could not load the pairing code. Try again.')
    } finally {
      if (generation.current === request) setBusy(false)
    }
  }

  if (isCompanionContext()) return null
  return <SettingRow
    title="Pair iPhone"
    description={!enabled ? 'Available when phone access is ready.' : !nativePhoneUrl ? 'A secure phone address is required for pairing.' : 'Scan this in the Universal Agent Manager iPhone app.'}
    control={<Button size="sm" variant="secondary" leadingIcon={<QrCode size={14} aria-hidden />} disabled={!enabled || !nativePhoneUrl || busy}
      onClick={() => { if (payload) { generation.current += 1; setPayload('') } else void show() }}>
      {busy ? 'Loading…' : payload ? 'Hide pairing code' : 'Show pairing code'}
    </Button>}
  >
    {payload && <div className="flex flex-col items-start gap-2 py-2">
      <QRCodeSVG value={payload} size={232} level="M" marginSize={4} title="Universal Agent Manager iPhone pairing code" style={{ maxWidth: '100%', height: 'auto' }} />
      <p className="text-xs" style={{ color: 'var(--text-2)' }}>This code grants access to Universal Agent Manager. It hides after two minutes; hiding does not revoke existing connections.</p>
    </div>}
    {message && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{message}</p>}
  </SettingRow>
}
