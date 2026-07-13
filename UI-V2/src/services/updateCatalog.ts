import type { CliVersionManager } from '../store/cpp/types'
import type { Provider } from '../types/provider'

const CACHE_KEY = 'uam-update-catalog-v1'
export const UPDATE_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000

const providerPackages: Record<string, string> = {
  'gemini-cli': '@google/gemini-cli',
  'codex-cli': '@openai/codex',
  'claude-cli': '@anthropic-ai/claude-code',
  'opencode-cli': 'opencode-ai',
  'copilot-cli': '@github/copilot',
}

export interface LatestUpdateCatalog {
  checkedAt: string
  uam: { version: string; url: string }
  providers: Record<string, { version: string; url: string }>
}

export interface AvailableUpdate {
  id: string
  providerId?: string
  name: string
  currentVersion: string
  latestVersion: string
  url: string
  installable: boolean
}

function cleanVersion(value: string): string {
  return value.trim().replace(/^[vV]/, '').split('-')[0]
}

export function compareVersions(left: string, right: string): number {
  const a = cleanVersion(left).split('.').map((part) => Number.parseInt(part, 10) || 0)
  const b = cleanVersion(right).split('.').map((part) => Number.parseInt(part, 10) || 0)
  for (let index = 0; index < Math.max(a.length, b.length); index += 1) {
    const difference = (a[index] ?? 0) - (b[index] ?? 0)
    if (difference !== 0) return difference
  }
  return 0
}

export function readCachedUpdateCatalog(): LatestUpdateCatalog | null {
  if (typeof window === 'undefined') return null
  try {
    const parsed = JSON.parse(window.localStorage.getItem(CACHE_KEY) ?? '') as LatestUpdateCatalog
    return parsed?.uam && parsed?.providers && parsed?.checkedAt ? parsed : null
  } catch {
    return null
  }
}

function cacheUpdateCatalog(catalog: LatestUpdateCatalog) {
  try {
    window.localStorage.setItem(CACHE_KEY, JSON.stringify(catalog))
  } catch {
    // The live result remains usable when storage is unavailable.
  }
}

export async function fetchLatestUpdateCatalog(): Promise<LatestUpdateCatalog> {
  const providerEntries = Object.entries(providerPackages)
  const [releaseResult, ...providerResults] = await Promise.allSettled([
    fetch('https://api.github.com/repos/davidtaylor6130/Universal-Agent-Manager/releases/latest', {
      headers: { Accept: 'application/vnd.github+json' },
    }),
    ...providerEntries.map(([, packageName]) =>
      fetch(`https://registry.npmjs.org/${encodeURIComponent(packageName)}/latest`)
    ),
  ])
  const releaseResponse = releaseResult.status === 'fulfilled' ? releaseResult.value : null
  const release = releaseResponse?.ok
    ? await releaseResponse.json() as { tag_name?: string; html_url?: string }
    : {}

  const providers: LatestUpdateCatalog['providers'] = {}
  for (let index = 0; index < providerEntries.length; index += 1) {
    const [providerId, packageName] = providerEntries[index]
    const result = providerResults[index]
    if (result.status !== 'fulfilled' || !result.value.ok) continue
    const response = result.value
    const payload = await response.json() as { version?: string }
    if (!payload.version) continue
    providers[providerId] = {
      version: payload.version,
      url: `https://www.npmjs.com/package/${packageName}`,
    }
  }

  if (!release.tag_name && Object.keys(providers).length === 0) {
    throw new Error('Update services are currently unavailable.')
  }

  const catalog: LatestUpdateCatalog = {
    checkedAt: new Date().toISOString(),
    uam: {
      version: release.tag_name ?? '',
      url: release.html_url || 'https://github.com/davidtaylor6130/Universal-Agent-Manager/releases/latest',
    },
    providers,
  }
  cacheUpdateCatalog(catalog)
  return catalog
}

export function availableUpdates(
  catalog: LatestUpdateCatalog | null,
  appVersion: string,
  versionManager: CliVersionManager,
  providers: Provider[],
  dismissedVersions: Record<string, string>,
): AvailableUpdate[] {
  if (!catalog) return []
  const updates: AvailableUpdate[] = []
  const uamVersion = cleanVersion(catalog.uam.version)
  if (compareVersions(uamVersion, appVersion) > 0 && dismissedVersions.uam !== uamVersion) {
    updates.push({
      id: 'uam',
      name: 'Universal Agent Manager',
      currentVersion: cleanVersion(appVersion),
      latestVersion: uamVersion,
      url: catalog.uam.url,
      installable: false,
    })
  }

  for (const state of versionManager.providers) {
    const latest = catalog.providers[state.providerId]
    const currentVersion = cleanVersion(state.installedVersion)
    if (!currentVersion) continue
    const preferredVersion = state.preferredVersion === 'latest' ? '' : cleanVersion(state.preferredVersion)
    const latestVersion = state.status === 'unsupported' && preferredVersion
      ? preferredVersion
      : cleanVersion(latest?.version ?? '')
    if (!latestVersion) continue
    if (state.status !== 'unsupported' && compareVersions(latestVersion, currentVersion) <= 0) continue
    if (dismissedVersions[state.providerId] === latestVersion) continue
    const provider = providers.find((entry) => entry.id === state.providerId)
    updates.push({
      id: state.providerId,
      providerId: state.providerId,
      name: provider?.shortName || provider?.name || state.providerId,
      currentVersion,
      latestVersion,
      url: latest?.url || `https://www.npmjs.com/package/${providerPackages[state.providerId] ?? ''}`,
      installable: state.preferredVersion === 'latest' ||
        state.providerId === 'gemini-cli' ||
        state.availableVersions.some((option) => cleanVersion(option.version) === latestVersion),
    })
  }
  return updates
}
