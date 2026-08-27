import type { CliVersionManager } from '../store/cpp/types'
import type { Provider } from '../types/provider'

const CACHE_KEY = 'uam-update-catalog-v1'
export const UPDATE_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000
export const UPDATE_REQUEST_TIMEOUT_MS = 15_000

const providerPackages: Record<string, {
  npmPackage: string
  homebrew: { kind: 'formula' | 'cask'; name: string }
}> = {
  'gemini-cli': { npmPackage: '@google/gemini-cli', homebrew: { kind: 'formula', name: 'gemini-cli' } },
  'codex-cli': { npmPackage: '@openai/codex', homebrew: { kind: 'cask', name: 'codex' } },
  'claude-cli': { npmPackage: '@anthropic-ai/claude-code', homebrew: { kind: 'cask', name: 'claude-code' } },
  'opencode-cli': { npmPackage: 'opencode-ai', homebrew: { kind: 'formula', name: 'opencode' } },
  'copilot-cli': { npmPackage: '@github/copilot', homebrew: { kind: 'cask', name: 'copilot-cli' } },
}

export interface LatestUpdateCatalog {
  checkedAt: string
  uam: { version: string; url: string }
  providers: Record<string, {
    version: string
    url: string
    homebrew?: { version: string; url: string }
  }>
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
  return value.trim().replace(/^[vV]/, '').split('+')[0]
}

export function compareVersions(left: string, right: string): number {
  const [aCore, aPre = ''] = cleanVersion(left).split('-', 2)
  const [bCore, bPre = ''] = cleanVersion(right).split('-', 2)
  const a = aCore.split('.').map((part) => Number.parseInt(part, 10) || 0)
  const b = bCore.split('.').map((part) => Number.parseInt(part, 10) || 0)
  for (let index = 0; index < Math.max(a.length, b.length); index += 1) {
    const difference = (a[index] ?? 0) - (b[index] ?? 0)
    if (difference !== 0) return difference
  }
  if (!aPre || !bPre) return aPre ? -1 : bPre ? 1 : 0
  const aParts = aPre.split('.')
  const bParts = bPre.split('.')
  for (let index = 0; index < Math.max(aParts.length, bParts.length); index += 1) {
    if (aParts[index] === undefined) return -1
    if (bParts[index] === undefined) return 1
    if (aParts[index] === bParts[index]) continue
    const aNumber = /^\d+$/.test(aParts[index]) ? Number(aParts[index]) : null
    const bNumber = /^\d+$/.test(bParts[index]) ? Number(bParts[index]) : null
    if (aNumber !== null && bNumber !== null) return aNumber - bNumber
    if (aNumber !== null) return -1
    if (bNumber !== null) return 1
    return aParts[index] < bParts[index] ? -1 : 1
  }
  return 0
}

function isCatalog(value: unknown): value is LatestUpdateCatalog {
  if (!value || typeof value !== 'object') return false
  const catalog = value as Partial<LatestUpdateCatalog>
  if (typeof catalog.checkedAt !== 'string' || !Number.isFinite(Date.parse(catalog.checkedAt))) return false
  if (!catalog.uam || typeof catalog.uam.version !== 'string' || typeof catalog.uam.url !== 'string') return false
  if (!catalog.providers || typeof catalog.providers !== 'object' || Array.isArray(catalog.providers)) return false
  return Object.values(catalog.providers).every((provider) =>
    Boolean(provider) &&
    typeof provider.version === 'string' &&
    typeof provider.url === 'string' &&
    (!provider.homebrew || (typeof provider.homebrew.version === 'string' && typeof provider.homebrew.url === 'string'))
  )
}

export function readCachedUpdateCatalog(): LatestUpdateCatalog | null {
  if (typeof window === 'undefined') return null
  try {
    const parsed: unknown = JSON.parse(window.localStorage.getItem(CACHE_KEY) ?? '')
    return isCatalog(parsed) ? parsed : null
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
  const homebrewEntries = providerEntries
  const controller = new AbortController()
  let timedOut = false
  const timer = window.setTimeout(() => {
    timedOut = true
    controller.abort()
  }, UPDATE_REQUEST_TIMEOUT_MS)
  const requests = [
    fetch('https://api.github.com/repos/davidtaylor6130/Universal-Agent-Manager/releases/latest', {
      headers: { Accept: 'application/vnd.github+json' },
      signal: controller.signal,
    }),
    ...providerEntries.map(([, providerPackage]) =>
      fetch(`https://registry.npmjs.org/${encodeURIComponent(providerPackage.npmPackage)}/latest`, { signal: controller.signal })
    ),
    ...homebrewEntries.map(([, providerPackage]) =>
      fetch(`https://formulae.brew.sh/api/${providerPackage.homebrew.kind}/${providerPackage.homebrew.name}.json`, { signal: controller.signal })
    ),
  ]
  const [releaseResult, ...results] = await Promise.allSettled(requests).finally(() => window.clearTimeout(timer))
  if (timedOut) throw new Error('Update check timed out.')
  const providerResults = results.slice(0, providerEntries.length)
  const homebrewResults = results.slice(providerEntries.length)
  const releaseResponse = releaseResult.status === 'fulfilled' ? releaseResult.value : null
  let release: { tag_name?: string; html_url?: string } = {}
  try {
    if (releaseResponse?.ok) release = await releaseResponse.json()
  } catch {
    // Keep the independent provider results.
  }

  const providers: LatestUpdateCatalog['providers'] = {}
  for (let index = 0; index < providerEntries.length; index += 1) {
    const [providerId, providerPackage] = providerEntries[index]
    const result = providerResults[index]
    if (result.status !== 'fulfilled' || !result.value.ok) continue
    const response = result.value
    let payload: { version?: string }
    try {
      payload = await response.json()
    } catch {
      continue
    }
    if (!payload.version) continue
    providers[providerId] = {
      version: payload.version,
      url: `https://www.npmjs.com/package/${providerPackage.npmPackage}`,
    }
  }

  for (let index = 0; index < homebrewEntries.length; index += 1) {
    const [providerId, providerPackage] = homebrewEntries[index]
    const result = homebrewResults[index]
    if (result.status !== 'fulfilled' || !result.value.ok) continue
    let payload: { version?: string; versions?: { stable?: string } }
    try {
      payload = await result.value.json()
    } catch {
      continue
    }
    const version = payload.version || payload.versions?.stable || ''
    if (!version) continue
    providers[providerId] ??= {
      version: '',
      url: `https://www.npmjs.com/package/${providerPackage.npmPackage}`,
    }
    providers[providerId].homebrew = {
      version,
      url: `https://formulae.brew.sh/${providerPackage.homebrew.kind}/${providerPackage.homebrew.name}`,
    }
  }

  if (!release.tag_name) throw new Error('UAM update service is currently unavailable.')

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
    const catalogProvider = catalog.providers[state.providerId]
    const latest = state.installMethod === 'homebrew-formula' || state.installMethod === 'homebrew-cask'
      ? catalogProvider?.homebrew
      : catalogProvider
    const currentVersion = cleanVersion(state.installedVersion)
    if (!currentVersion) continue
    const latestVersion = cleanVersion(latest?.version ?? '')
    if (!latestVersion) continue
    if (compareVersions(latestVersion, currentVersion) <= 0) continue
    if (dismissedVersions[state.providerId] === latestVersion) continue
    const provider = providers.find((entry) => entry.id === state.providerId)
    updates.push({
      id: state.providerId,
      providerId: state.providerId,
      name: provider?.shortName || provider?.name || state.providerId,
      currentVersion,
      latestVersion,
      url: latest?.url || `https://www.npmjs.com/package/${providerPackages[state.providerId]?.npmPackage ?? ''}`,
      installable: state.preferredVersion === 'latest' ||
        state.providerId === 'gemini-cli' ||
        state.availableVersions.some((option) => cleanVersion(option.version) === latestVersion),
    })
  }
  return updates
}
