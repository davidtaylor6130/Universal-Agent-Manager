import { useEffect, useState } from 'react'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { Button } from '../ui'

type Connection = { providerId: string; status: string; detail: string }

/** A missing UAM connection is actionable without interrupting the provider's work. */
export function ProviderMcpWarning() {
  const session = useAppStore(state => state.sessions.find(item => item.id === state.activeSessionId))
  const centralConfiguration = useAppStore(state => state.centralProviderConfiguration)
  const servers = useAppStore(state => state.mcpServers)
  const runtimes = useAppStore(state => state.acpBindingBySessionId)
  const runtime = session ? runtimes[session.id] : undefined
  const [notice, setNotice] = useState<{ key: string; connection: Connection } | null>(null)
  const [dismissed, setDismissed] = useState<Map<string, { chatId: string; runtimeId: string }>>(() => new Map())
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ key: string; text: string } | null>(null)
  const [refresh, setRefresh] = useState(0)
  const scopeKey = session ? JSON.stringify([session.id, session.providerId, session.executionHostId ?? 'local', session.workspaceDirectory ?? '']) : ''
  const runtimeId = runtime?.sessionId ?? ''
  const key = JSON.stringify([scopeKey, runtimeId])
  const isDismissed = dismissed.get(scopeKey)?.runtimeId === runtimeId
  const configurationSignature = JSON.stringify([
    centralConfiguration.uamControlEnabled,
    (centralConfiguration.mcpOverrides ?? []).filter(override => override.providerId === session?.providerId &&
      override.executionHostId === (session?.executionHostId ?? 'local') && override.serverId === 'uam-mcp'),
    servers.map(server => [server.id, server.enabled, server.executionHostId, server.workspaceDirectory]),
  ])

  useEffect(() => {
    setDismissed(previous => {
      if ([...previous.values()].every(item => runtimes[item.chatId]?.running)) return previous
      return new Map([...previous].filter(([, item]) => runtimes[item.chatId]?.running))
    })
  }, [runtimes])

  useEffect(() => {
    let canceled = false
    if (!session?.providerId || session.viewMode === 'cli' || isDismissed) return
    void sendToCEF<{ executionHostId: string; providers: Connection[] }>({ action: 'getProviderMcpStatus', payload: { chatId: session.id, executionHostId: session.executionHostId ?? 'local', workspaceDirectory: session.workspaceDirectory ?? '' } })
      .then(response => {
        if (canceled) return
        if (!response.ok || response.data?.executionHostId !== (session.executionHostId ?? 'local') || !Array.isArray(response.data.providers)) {
          setNotice(null)
          return
        }
        const connection = response.data.providers.find(item => item.providerId === session.providerId)
        setNotice(connection && ['missing', 'failed'].includes(connection.status) ? { key, connection } : null)
      }).catch(() => { if (!canceled) setNotice(null) })
    return () => { canceled = true }
  }, [key, session?.viewMode, session?.uamControlEnabled, session?.workspaceDirectory, configurationSignature, runtime?.running, runtime?.sessionId, runtime?.lifecycleState, isDismissed, refresh])
  useEffect(() => {
    if (!session?.providerId || session.viewMode === 'cli' || isDismissed || (!notice && !runtime?.running)) return
    const timer = window.setInterval(() => setRefresh(value => value + 1), notice?.key === key ? 5000 : 30000)
    return () => window.clearInterval(timer)
  }, [notice, key, session?.viewMode, runtime?.running, isDismissed])

  if (!notice || notice.key !== key || isDismissed) return null
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
      <Button size="sm" disabled={busy} onClick={() => { if (session) setDismissed(previous => new Map(previous).set(scopeKey, { chatId: session.id, runtimeId })) }}>Continue without it</Button>
    </div>
  </div>
}
