import { useEffect, useState } from 'react'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { Button } from '../ui'

type Connection = { providerId: string; status: string; detail: string }

/** A missing UAM connection is actionable without interrupting the provider's work. */
export function ProviderMcpWarning() {
  const session = useAppStore(state => state.sessions.find(item => item.id === state.activeSessionId))
  const [notice, setNotice] = useState<{ key: string; connection: Connection } | null>(null)
  const [dismissed, setDismissed] = useState<Set<string>>(() => new Set())
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ key: string; text: string } | null>(null)
  const [refresh, setRefresh] = useState(0)
  const key = session ? JSON.stringify([session.id, session.providerId, session.executionHostId ?? 'local', session.workspaceDirectory ?? '']) : ''

  useEffect(() => {
    let canceled = false
    if (!session?.providerId || session.viewMode === 'cli' || dismissed.has(key)) return
    void sendToCEF<{ providers: Connection[] }>({ action: 'getProviderMcpStatus', payload: { chatId: session.id, executionHostId: session.executionHostId ?? 'local', workspaceDirectory: session.workspaceDirectory ?? '' } })
      .then(response => {
        if (canceled || !response.ok) return
        const connection = response.data?.providers.find(item => item.providerId === session.providerId)
        setNotice(connection && ['missing', 'disabled', 'failed'].includes(connection.status) ? { key, connection } : null)
      }).catch(() => { /* Settings exposes diagnostic failures; don't turn an unavailable status check into a missing-connection claim. */ })
    return () => { canceled = true }
  }, [key, session?.viewMode, session?.uamControlEnabled, session?.workspaceDirectory, dismissed, refresh])
  useEffect(() => {
    if (!notice || notice.key !== key || dismissed.has(key)) return
    const timer = window.setTimeout(() => setRefresh(value => value + 1), 5000)
    return () => window.clearTimeout(timer)
  }, [notice, key, dismissed, refresh])

  if (!notice || notice.key !== key || dismissed.has(key)) return null
  return <div role="alert" className="shrink-0 px-4 py-3 text-sm" style={{ borderBottom: '2px solid #ef4444', background: 'var(--bg)', color: 'var(--text)' }}>
    <strong style={{ color: '#ff5555' }}>WARNING: UAM MCP tools are unavailable.</strong>
    <p className="mt-1">{result?.key === key ? result.text : notice.connection.detail}</p>
    <div className="mt-2 flex flex-wrap gap-2">
      <Button size="sm" disabled={busy} onClick={() => {
        if (!session) return
        const selected = session; const submittedKey = key
        setBusy(true)
        void (async () => {
          try {
            const store = useAppStore.getState()
            const saved = await store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: true })
            if (!saved.ok) throw Error(saved.error || 'Could not enable the UAM MCP setup.')
            const current = useAppStore.getState().sessions.find(item => item.id === selected.id)
            if (!current || current.providerId !== selected.providerId || current.executionHostId !== selected.executionHostId || current.workspaceDirectory !== selected.workspaceDirectory) throw Error('This chat changed while saving. Check its current provider settings.')
            if (!await store.setSessionUamControlEnabled(selected.id, true, true)) throw Error('Could not enable UAM tools for this chat. Open provider settings for details.')
            setResult({ key: submittedKey, text: 'UAM MCP setup enabled. It applies when this provider session next starts; current work keeps running.' })
          } catch (error) { setResult({ key: submittedKey, text: error instanceof Error ? error.message : 'Could not enable UAM MCP setup.' }) }
          finally { setBusy(false) }
        })()
      }}>Enable UAM connection</Button>
      <Button size="sm" onClick={() => useAppStore.getState().setSettingsOpen(true)}>View settings</Button>
      <Button size="sm" disabled={busy} onClick={() => setDismissed(previous => new Set(previous).add(key))}>Continue without it</Button>
    </div>
  </div>
}
