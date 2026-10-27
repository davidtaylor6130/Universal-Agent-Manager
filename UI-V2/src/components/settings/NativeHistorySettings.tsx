import { useEffect, useRef, useState } from 'react'
import { ChevronRight, Search, TriangleAlert } from 'lucide-react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { parseStartupNativeImportPage, sanitizeStartupNativeImportStatus, type StartupNativeImportPage } from '../../types/startupNativeImports'
import { ANTIGRAVITY_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, SUPPORTED_CLI_PROVIDER_IDS, normalizeCliProviderIdAlias, providerMetadataForId } from '../../utils/providerMetadata'
import { Button, Notice } from '../ui'
import { ProviderLogo } from '../shared/ProviderLogo'
import { ANTIGRAVITY_ENABLED, OPENCODE_ENABLED, SSH_ENABLED } from '../../config/buildFeatures'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

type IssueGroup = { provider: string; reason: string; items: string[] }

/** Folds per-conversation discovery messages into one line per provider and reason. */
export function groupDiscoveryIssues(messages: string[], providerNames: string[]): IssueGroup[] {
  const groups = new Map<string, IssueGroup>()
  for (const message of messages) {
    const match = /^(\S+) conversation (\S+): (.+)$/.exec(message)
    const first = message.split(' ')[0]
    const provider = match?.[1] ?? (providerNames.includes(first) ? first : '')
    const raw = match ? match[3] : message.slice(provider.length).trim()
    const trimmed = provider && raw.startsWith(`${provider} `) ? raw.slice(provider.length + 1) : raw
    const reason = trimmed.charAt(0).toUpperCase() + trimmed.slice(1)
    const key = `${provider}\u0000${reason}`
    const group = groups.get(key) ?? groups.set(key, { provider, reason, items: [] }).get(key)!
    if (match && !group.items.includes(match[2])) group.items.push(match[2])
  }
  return [...groups.values()]
}

/** Explicit discovery shares startup staging and its guarded selection/publication path. */
export function NativeHistorySettings() {
  const status = useAppStore((state) => state.startupNativeImports)
  const providers = useAppStore((state) => state.providers)
  const providerIds = [...new Set(providers.map((provider) => normalizeCliProviderIdAlias(provider.id)))].filter((id) => SUPPORTED_CLI_PROVIDER_IDS.includes(id))
  const providerNames = providerIds.map((id) => providerMetadataForId(id).shortName)
  const providerScope = providerNames.length > 1 ? `${providerNames.slice(0, -1).join(', ')} and ${providerNames[providerNames.length - 1]}` : providerNames[0]
  const [page, setPage] = useState<StartupNativeImportPage | null>(null)
  const [requesting, setRequesting] = useState(false)
  const [error, setError] = useState('')
  const pending = useRef(false)
  const mounted = useRef(false)
  const cacheRequest = useRef(0)
  const companion = isCompanionContext()
  useEffect(() => {
    mounted.current = true
    return () => { mounted.current = false; ++cacheRequest.current }
  }, [])
  useEffect(() => {
    if (companion || !status.generation) return
    let current = true
    const request = ++cacheRequest.current
    void (async () => {
      const response = await sendToCEF<unknown>({ action: 'getStartupNativeImports', payload: { offset: 0, limit: 1 } })
      if (!current || !mounted.current || request !== cacheRequest.current) return
      const data = response.ok ? parseStartupNativeImportPage(response.data) : null
      if (!data || data.generation !== status.generation) {
        setError(response.error || 'Discovery results could not be read. Reopen Chat Data to retry.')
        return
      }
      setPage(data); setError('')
    })()
    return () => { current = false }
  }, [companion, status.generation, status.phase, status.totalCount, status.importedCount])

  async function discover() {
    if (pending.current || companion) return
    pending.current = true; setRequesting(true); setError('')
    const before = useAppStore.getState().startupNativeImports
    try {
      const response = await sendToCEF<unknown>({ action: 'discoverSystemNativeImports', payload: {} })
      if (!mounted.current) return
      const data = response.ok ? parseStartupNativeImportPage(response.data) : null
      if (!data) { setError(response.error || 'Native history discovery could not start.'); return }
      // A state push may already have advanced this scan before its acknowledgement arrives.
      if (useAppStore.getState().startupNativeImports === before) {
        useAppStore.setState({ startupNativeImports: sanitizeStartupNativeImportStatus(data) })
      }
    } finally {
      pending.current = false
      if (mounted.current) setRequesting(false)
    }
  }
  const active = status.phase === 'scanning' || status.phase === 'importing'
  const current = page?.generation === status.generation ? page : null
  const issues = current ? groupDiscoveryIssues(current.errors, providerNames) : []
  const issueCount = current?.errors.length ?? 0
  const summary = companion ? 'Discover native histories in the desktop app.'
    : status.phase === 'scanning' ? 'Scanning native histories…'
    : status.phase === 'importing' ? `Importing selected histories… ${status.importedCount} imported.`
    : status.generation ? `${status.totalCount} histories awaiting review; ${status.importedCount} imported in this scan.`
    : 'Discovery does not import or resume chats.'
  const idForName = (name: string) => providerIds.find((id) => providerMetadataForId(id).shortName === name)
  return <SettingsSection title="Native provider history">
    <SettingRow title="Discover native histories" description="Scan this computer, then choose chats to import."
      status={<span role="status">{summary}</span>}
      control={<div className="flex flex-wrap gap-2">
        {status.phase === 'ready' && status.totalCount > 0 && <Button size="sm" variant="secondary" disabled={requesting || companion} onClick={() => useAppStore.getState().openStartupNativeImportReview()}>Review histories</Button>}
        <Button size="sm" variant="secondary" data-native-history-discovery disabled={requesting || active || companion} leadingIcon={<Search size={14} />} aria-busy={requesting || status.phase === 'scanning' || undefined} onClick={() => void discover()}>Discover histories</Button>
      </div>}>
      <ul className="uam-native-scope" aria-label={providerScope ? `Reads ${providerScope} native history stores for this user.` : 'Reads native history stores supported by this desktop build.'}>
        {providerIds.map((id) => {
          const name = providerMetadataForId(id).shortName
          const count = current ? current.errors.filter((message) => message.startsWith(`${name} `)).length : 0
          return <li key={id} className="uam-native-scope__chip" data-issues={count > 0 || undefined}>
            <ProviderLogo providerId={id} size={14} />{name}
            {count > 0 && <><span className="uam-native-scope__count" aria-hidden>{count}</span><span className="sr-only">, {count} {count === 1 ? 'issue' : 'issues'}</span></>}
          </li>
        })}
      </ul>
      {error && <Notice tone="error" title="Native history discovery" dismissible={false} dismissLabel="Dismiss discovery error">{error}</Notice>}
      {issueCount > 0 && <details className="uam-native-disclosure uam-native-disclosure--warning">
        <summary><TriangleAlert size={14} aria-hidden />{issueCount === 1 ? '1 discovery issue' : `${issueCount} discovery issues`}<ChevronRight size={14} className="uam-native-disclosure__chevron" aria-hidden /></summary>
        <ul className="uam-native-issues">{issues.map((group) => {
          const providerId = idForName(group.provider)
          return <li key={`${group.provider}:${group.reason}`}>
            <span className="uam-native-issues__provider">{providerId && <ProviderLogo providerId={providerId} size={14} />}{group.provider || 'Discovery'}</span>
            <span className="uam-native-issues__reason">{group.items.length > 0 ? `${group.items.length} conversation${group.items.length === 1 ? '' : 's'} skipped. ${group.reason}` : group.reason}</span>
            {group.items.length > 0 && <details className="uam-native-issues__ids"><summary>Show IDs</summary><ul>{group.items.map((item) => <li key={item}><code>{item}</code></li>)}</ul></details>}
          </li>
        })}</ul>
      </details>}
      <details className="uam-native-disclosure">
        <summary>How discovery works<ChevronRight size={14} className="uam-native-disclosure__chevron" aria-hidden /></summary>
        <ul className="uam-native-notes">
          <li>Nothing is imported until you select chats and confirm.</li>
          <li>Histories from unregistered local workspaces stay in review until you assign their source workspace.</li>
          <li>Existing chats and deleted or ignored sessions are excluded.{SSH_ENABLED && ' SSH hosts are outside this scan.'}</li>
          {OPENCODE_ENABLED && providerIds.includes(OPENCODE_CLI_PROVIDER_ID) && <li>OpenCode requires its configured CLI.</li>}
          {ANTIGRAVITY_ENABLED && providerIds.includes(ANTIGRAVITY_CLI_PROVIDER_ID) && <li>Antigravity reads settled CLI SQLite and supported legacy histories. Changing, nested or unrecognised native records remain unimported and report a retryable issue.</li>}
        </ul>
      </details>
    </SettingRow>
  </SettingsSection>
}
