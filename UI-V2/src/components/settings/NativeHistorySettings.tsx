import { useEffect, useRef, useState } from 'react'
import { Search } from 'lucide-react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { parseStartupNativeImportPage, sanitizeStartupNativeImportStatus, type StartupNativeImportPage } from '../../types/startupNativeImports'
import { ANTIGRAVITY_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, SUPPORTED_CLI_PROVIDER_IDS, normalizeCliProviderIdAlias, providerMetadataForId } from '../../utils/providerMetadata'
import { Button, Notice } from '../ui'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

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
  const summary = companion ? 'Discover native histories in the desktop app.'
    : status.phase === 'scanning' ? 'Scanning native histories…'
    : status.phase === 'importing' ? `Importing selected histories… ${status.importedCount} imported.`
    : status.generation ? `${status.totalCount} histories awaiting review; ${status.importedCount} imported in this scan.`
    : 'Discovery does not import or resume chats.'
  return <SettingsSection title="Native provider history">
    <SettingRow title="Discover native histories" description="Scan this computer, then choose chats to import."
      status={<span role="status">{summary}</span>}
      control={<div className="flex flex-wrap gap-2">
        {status.phase === 'ready' && status.totalCount > 0 && <Button size="sm" variant="secondary" disabled={requesting || companion} onClick={() => useAppStore.getState().openStartupNativeImportReview()}>Review histories</Button>}
        <Button size="sm" variant="secondary" data-native-history-discovery disabled={requesting || active || companion} leadingIcon={<Search size={14} />} aria-busy={requesting || status.phase === 'scanning' || undefined} onClick={() => void discover()}>Discover histories</Button>
      </div>}>
      <p>{providerScope ? `Reads ${providerScope} native history stores for this user.` : 'Reads native history stores supported by this desktop build.'}{providerIds.includes(OPENCODE_CLI_PROVIDER_ID) && ' OpenCode requires its configured CLI.'}</p>
      <p>Histories from unregistered local workspaces stay in review until you assign their source workspace. New workspace entries are saved with selected imports. Existing chats and deleted or ignored sessions are excluded. SSH hosts are outside this scan.</p>
      {providerIds.includes(ANTIGRAVITY_CLI_PROVIDER_ID) && <p>Antigravity reads settled CLI SQLite and supported legacy histories. Changing, nested or unrecognised native records remain unimported and report a retryable issue.</p>}
      {error && <Notice tone="error" title="Native history discovery" dismissible={false} dismissLabel="Dismiss discovery error">{error}</Notice>}
      {current && current.errors.length > 0 && <Notice tone="warning" title="Discovery issues" dismissible={false} dismissLabel="Dismiss discovery issues">
        <ul>{current.errors.map((message, index) => <li key={index}>{message}</li>)}</ul>
      </Notice>}
    </SettingRow>
  </SettingsSection>
}
