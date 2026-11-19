import type { CliVersionManager } from '../store/cpp/types'
import type { Provider } from '../types/provider'
import type { ExecutionHost } from '../types/session'
import { CLAUDE_ENABLED, CODEX_ENABLED, COPILOT_ENABLED, GEMINI_ENABLED, OPENCODE_ENABLED } from '../config/buildFeatures'
import { CLAUDE_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, GEMINI_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID } from '../utils/providerMetadata'
import { isSshHost } from '../utils/remoteWorkspace'

const CACHE_KEY = 'uam-update-catalog-v1'
export const UPDATE_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000
export const UPDATE_REQUEST_TIMEOUT_MS = 15_000
const RELEASE_NOTES_EXCERPT_LIMIT = 500

const providerPackages: Record<string, {
  npmPackage: string
  homebrew: { kind: 'formula' | 'cask'; name: string }
  releaseRepository: string
  releaseTagPrefix: string
}> = {
  ...(GEMINI_ENABLED ? { [GEMINI_CLI_PROVIDER_ID]: { npmPackage: '@google/gemini-cli', homebrew: { kind: 'formula' as const, name: 'gemini-cli' }, releaseRepository: 'google-gemini/gemini-cli', releaseTagPrefix: 'v' } } : {}),
  ...(CODEX_ENABLED ? { [CODEX_CLI_PROVIDER_ID]: { npmPackage: '@openai/codex', homebrew: { kind: 'cask' as const, name: 'codex' }, releaseRepository: 'openai/codex', releaseTagPrefix: 'rust-v' } } : {}),
  ...(CLAUDE_ENABLED ? { [CLAUDE_CLI_PROVIDER_ID]: { npmPackage: '@anthropic-ai/claude-code', homebrew: { kind: 'cask' as const, name: 'claude-code' }, releaseRepository: 'anthropics/claude-code', releaseTagPrefix: 'v' } } : {}),
  ...(OPENCODE_ENABLED ? { [OPENCODE_CLI_PROVIDER_ID]: { npmPackage: 'opencode-ai', homebrew: { kind: 'formula' as const, name: 'opencode' }, releaseRepository: 'anomalyco/opencode', releaseTagPrefix: 'v' } } : {}),
  ...(COPILOT_ENABLED ? { [COPILOT_CLI_PROVIDER_ID]: { npmPackage: '@github/copilot', homebrew: { kind: 'cask' as const, name: 'copilot-cli' }, releaseRepository: 'github/copilot-cli', releaseTagPrefix: 'v' } } : {}),
}

export interface LatestUpdateCatalog {
  checkedAt: string
  uam: { version: string; url: string; releaseNotesExcerpt?: string }
  providers: Record<string, {
    version: string
    url: string
    releaseNotesExcerpt?: string
    homebrew?: { version: string; url: string; releaseNotesExcerpt?: string }
  }>
}

export interface AvailableUpdate {
  id: string
  providerId?: string
  remoteHostId?: string
  executionHostId?: string
  name: string
  /** `name` without the computer suffix, for surfaces that show the computer separately. */
  shortName?: string
  currentVersion: string
  latestVersion: string
  /** Display text for latestVersion when it differs; latestVersion stays the dismissal and install key. */
  latestDisplay?: string
  url: string
  installable: boolean
  releaseNotesExcerpt?: string
}

function releaseNotesExcerpt(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined
  const text = value.slice(0, 10_000)
    .replace(/<(script|style)\b[^>]*>[\s\S]*?<\/\1\s*>/gi, ' ')
    .replace(/<[^>]*>/g, ' ')
    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/^\s{0,3}(?:#{1,6}\s+|[-*+]\s+|>\s*)/gm, '')
    .replace(/[`*_~]/g, '')
    .replace(/[\s\u0000-\u001f\u007f]+/g, ' ')
    .trim()
  return text ? text.slice(0, RELEASE_NOTES_EXCERPT_LIMIT).trimEnd() : undefined
}

export function cliProviderUpdateId(providerId: string, executionHostId?: string): string {
  return executionHostId ? JSON.stringify([executionHostId, providerId]) : providerId
}

function cleanVersion(value: string): string {
  return value.trim().replace(/^[vV]/, '').split('+')[0]
}

export function compareVersions(left: string, right: string): number {
  const [aCore, ...aPreParts] = cleanVersion(left).split('-')
  const [bCore, ...bPreParts] = cleanVersion(right).split('-')
  const aPre = aPreParts.join('-')
  const bPre = bPreParts.join('-')
  const a = aCore.split('.').map((part) => Number.parseInt(part, 10) || 0)
  const b = bCore.split('.').map((part) => Number.parseInt(part, 10) || 0)
  for (let index = 0; index < Math.max(a.length, b.length); index += 1) {
    const difference = (a[index] ?? 0) - (b[index] ?? 0)
    if (difference !== 0) return difference
  }
  if (!aPre || !bPre) return aPre ? -1 : bPre ? 1 : 0
  const aParts = aPre.split(/[.-]/)
  const bParts = bPre.split(/[.-]/)
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

/** This app's local build label (X.Y.Z-alpha-Q) from its UI build ID; empty for release builds. */
export function localBuildLabel(appVersion: string, uiBuildId: string): string {
  const local = uiBuildId.match(/^(\d+\.\d+\.\d+-alpha-[1-9]\d*)-/)?.[1]
  return local?.startsWith(`${cleanVersion(appVersion)}-alpha-`) ? local : ''
}

export function availableRemoteHelperUpdates(
  appVersion: string,
  expectedProtocolVersion: number,
  executionHosts: ExecutionHost[],
  dismissedVersions: Record<string, string>,
  appBuildLabel = '',
): AvailableUpdate[] {
  const latestVersion = cleanVersion(appVersion)
  if (!latestVersion || expectedProtocolVersion <= 0) return []
  return executionHosts.flatMap((host) => {
    const currentVersion = cleanVersion(host.runnerVersion)
    const id = `remote-helper-${host.id}`
    // A rebuilt helper can keep its version; the bundled checksum identifies the build, and an unrecorded installed checksum is unknown.
    const bundledBuild = host.bundledRunnerSha256 ?? ''
    const buildOutdated = Boolean(bundledBuild) && host.runnerSha256 !== bundledBuild
    const versionOutdated = !currentVersion || compareVersions(latestVersion, currentVersion) > 0
    const protocolOutdated = (host.runnerProtocolVersion ?? 0) < expectedProtocolVersion
    // Only a build-only offer carries the build in its dismissal key, so earlier version/protocol dismissals still hold.
    const buildOnly = buildOutdated && !versionOutdated && !protocolOutdated
    const updateDetail = ` · helper protocol ${expectedProtocolVersion}${buildOnly ? ` · build ${bundledBuild.slice(0, 12)}` : ''}`
    const updateVersion = `${latestVersion}${updateDetail}`
    if (!isSshHost(host) || (!versionOutdated && !protocolOutdated && !buildOutdated) || dismissedVersions[id] === updateVersion) return []
    return [{
      id,
      remoteHostId: host.id,
      name: `${host.label} SSH helper`,
      shortName: 'SSH helper',
      currentVersion: `${host.runnerBuildLabel || currentVersion || 'Not detected'}${buildOutdated ? ` · build ${host.runnerSha256?.slice(0, 12) || 'unknown'}` : ''}`,
      latestVersion: updateVersion,
      ...(appBuildLabel ? { latestDisplay: `${appBuildLabel}${updateDetail}` } : {}),
      url: '',
      installable: true,
    }]
  })
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
    if (!isCatalog(parsed)) return null
    const sanitizeEntry = (entry: { version: string; url: string; releaseNotesExcerpt?: unknown }) => {
      const { releaseNotesExcerpt: cachedExcerpt, ...fields } = entry
      const excerpt = releaseNotesExcerpt(cachedExcerpt)
      return { ...fields, ...(excerpt ? { releaseNotesExcerpt: excerpt } : {}) }
    }
    const providers = Object.fromEntries(Object.entries(parsed.providers).map(([id, provider]) => {
      const { homebrew, ...entry } = provider
      return [id, { ...sanitizeEntry(entry), ...(homebrew ? { homebrew: sanitizeEntry(homebrew) } : {}) }]
    }))
    return { ...parsed, uam: sanitizeEntry(parsed.uam), providers }
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
  try {
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
    const [releaseResult, ...results] = await Promise.allSettled(requests)
    if (timedOut) throw new Error('Update check timed out.')
    const providerResults = results.slice(0, providerEntries.length)
    const homebrewResults = results.slice(providerEntries.length)
    const releaseResponse = releaseResult.status === 'fulfilled' ? releaseResult.value : null
    let release: { tag_name?: string; html_url?: string; body?: unknown } = {}
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
      if (typeof payload?.version !== 'string' || !payload.version.trim()) continue
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
      const version = typeof payload?.version === 'string' ? payload.version :
        typeof payload?.versions?.stable === 'string' ? payload.versions.stable : ''
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

    if (!release.tag_name && Object.keys(providers).length === 0) throw new Error('Update services are currently unavailable.')

    await Promise.all(providerEntries.map(async ([providerId, providerPackage]) => {
      const provider = providers[providerId]
      if (!provider) return
      const entries = [provider, ...(provider.homebrew ? [provider.homebrew] : [])]
      for (const version of new Set(entries.map((entry) => cleanVersion(entry.version)).filter(Boolean))) {
        if (controller.signal.aborted) break
        try {
          const tag = providerPackage.releaseTagPrefix + version
          const response = await fetch(`https://api.github.com/repos/${providerPackage.releaseRepository}/releases/tags/${encodeURIComponent(tag)}`, {
            headers: { Accept: 'application/vnd.github+json' }, signal: controller.signal,
          })
          if (!response.ok) continue
          const release: unknown = await response.json()
          if (!release || typeof release !== 'object' || Array.isArray(release)) continue
          const payload = release as { tag_name?: unknown; body?: unknown; draft?: unknown }
          if (payload.tag_name !== tag || payload.draft === true) continue
          const excerpt = releaseNotesExcerpt(payload.body)
          if (excerpt) for (const entry of entries) if (cleanVersion(entry.version) === version) entry.releaseNotesExcerpt = excerpt
        } catch {
          // Optional notes must not discard successful package-version checks.
        }
      }
    }))

    const cachedUam = readCachedUpdateCatalog()?.uam
    const excerpt = release.tag_name ? releaseNotesExcerpt(release.body) : cachedUam?.releaseNotesExcerpt
    const catalog: LatestUpdateCatalog = {
      checkedAt: new Date().toISOString(),
      uam: {
        version: release.tag_name || cachedUam?.version || '',
        url: release.html_url || 'https://github.com/davidtaylor6130/Universal-Agent-Manager/releases/latest',
        ...(excerpt ? { releaseNotesExcerpt: excerpt } : {}),
      },
      providers,
    }
    cacheUpdateCatalog(catalog)
    return catalog
  } finally {
    window.clearTimeout(timer)
  }
}

/** Automatic provider updates install only npm releases at least this old, leaving time for a bad publish to be pulled. */
export const AUTOMATIC_UPDATE_MIN_AGE_DAYS = 7
const DAY_MS = 24 * 60 * 60 * 1000

/** Publish times (ms) of a provider package's versions that npm still lists and has not deprecated. */
export type NpmReleaseHistory = Record<string, number>

/** Reads publish times from the full npm document; abbreviated metadata omits them. Null when unavailable. */
export async function fetchNpmReleaseHistory(providerId: string, signal?: AbortSignal): Promise<NpmReleaseHistory | null> {
  const npmPackage = providerPackages[providerId]?.npmPackage
  if (!npmPackage) return null
  try {
    const response = await fetch(`https://registry.npmjs.org/${encodeURIComponent(npmPackage)}`, { signal })
    if (!response.ok) return null
    const payload: unknown = await response.json()
    if (!payload || typeof payload !== 'object') return null
    const { versions, time } = payload as { versions?: unknown; time?: unknown }
    if (!versions || typeof versions !== 'object' || !time || typeof time !== 'object') return null
    const times = time as Record<string, unknown>
    // A fully unpublished package keeps only `time.unpublished`; nothing in it is installable.
    if (times.unpublished) return {}
    const history: NpmReleaseHistory = {}
    for (const [version, manifest] of Object.entries(versions as Record<string, unknown>)) {
      if (!manifest || typeof manifest !== 'object' || (manifest as { deprecated?: unknown }).deprecated) continue
      const published = typeof times[version] === 'string' ? Date.parse(times[version] as string) : Number.NaN
      if (Number.isFinite(published)) history[version] = published
    }
    return history
  } catch {
    return null
  }
}

/**
 * Picks the version an automatic update may install: the newest stable, listed, non-deprecated release newer than
 * the installed one and no newer than the offered one, published at least the minimum age ago. With `exactOnly`
 * (installers that can only move to their current release, such as Homebrew) only the offered version qualifies.
 * Empty when nothing qualifies yet.
 */
export function automaticUpdateVersion(
  history: NpmReleaseHistory,
  currentVersion: string,
  offeredVersion: string,
  exactOnly: boolean,
  now = Date.now(),
  minAgeDays = AUTOMATIC_UPDATE_MIN_AGE_DAYS,
): string {
  const cutoff = now - minAgeDays * DAY_MS
  let chosen = ''
  for (const [version, published] of Object.entries(history)) {
    const clean = cleanVersion(version)
    if (published > cutoff || clean.includes('-') || compareVersions(clean, currentVersion) <= 0 || compareVersions(clean, offeredVersion) > 0) continue
    if (exactOnly && compareVersions(clean, offeredVersion) !== 0) continue
    if (!chosen || compareVersions(clean, chosen) > 0) chosen = clean
  }
  return chosen
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
      ...(catalog.uam.releaseNotesExcerpt ? { releaseNotesExcerpt: catalog.uam.releaseNotesExcerpt } : {}),
    })
  }

  for (const state of [...versionManager.providers, ...(versionManager.remoteProviders ?? [])]) {
    const catalogProvider = catalog.providers[state.providerId]
    const latest = state.installMethod === 'homebrew-formula' || state.installMethod === 'homebrew-cask'
      ? catalogProvider?.homebrew
      : catalogProvider
    const currentVersion = cleanVersion(state.installedVersion)
    if (!currentVersion) continue
    const latestVersion = cleanVersion(latest?.version ?? '')
    if (!latestVersion) continue
    if (compareVersions(latestVersion, currentVersion) <= 0) continue
    const id = cliProviderUpdateId(state.providerId, state.executionHostId)
    if (dismissedVersions[id] === latestVersion) continue
    const provider = providers.find((entry) => entry.id === state.providerId)
    updates.push({
      id,
      providerId: state.providerId,
      ...(state.executionHostId ? { executionHostId: state.executionHostId } : {}),
      name: `${provider?.shortName || provider?.name || state.providerId}${state.executionHostId ? ` · ${state.executionHostName || state.executionHostId}` : ''}`,
      shortName: provider?.shortName || provider?.name || state.providerId,
      currentVersion,
      latestVersion,
      url: latest?.url || `https://www.npmjs.com/package/${providerPackages[state.providerId]?.npmPackage ?? ''}`,
      ...(latest?.releaseNotesExcerpt ? { releaseNotesExcerpt: latest.releaseNotesExcerpt } : {}),
      installable: state.preferredVersion === 'latest' ||
        state.providerId === GEMINI_CLI_PROVIDER_ID ||
        state.availableVersions.some((option) => cleanVersion(option.version) === latestVersion),
    })
  }
  return updates
}
