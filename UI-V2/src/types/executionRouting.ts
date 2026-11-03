import type { ExecutionHost, Folder, Session } from './session'
import { isSshHost } from '../utils/remoteWorkspace'

export interface RoutingProviderConfirmation {
  providerId: string
  connectionIdentity: string
  providerConfigurationIdentity: string
  confirm?: boolean
}
/** A manual override of the remote checkout. `useAutomaticMatch: true` clears an override and returns the folder to repository matching. */
export interface RoutingWorkspaceMapping { sourceFolderId: string; sourceDirectory: string; destinationFolderId: string; useAutomaticMatch?: true }
export interface RoutingHostPolicy {
  hostId: string
  enabled: boolean
  maxActiveTurns: number
  providers: RoutingProviderConfirmation[]
  workspaceMappings: RoutingWorkspaceMapping[]
}
export interface ExecutionRoutingSettings { enabled: boolean; hosts: RoutingHostPolicy[]; repositoryMatching?: boolean }
/** Response of `listRepositoryMatches`: each local workspace's repository and its automatic or manual checkout per SSH computer. */
export interface RepositoryMatch { executionHostId: string; remoteDirectory: string | null; source: 'automatic' | 'manual' }
export interface WorkspaceRepositoryMatches { folderId: string; repository: { kind: 'git' | 'svn'; identity: string } | null; matches: RepositoryMatch[] }
export interface RoutingReservation {
  chatId: string; executionHostId: string; connectionIdentity: string
  providerConfigurationIdentity: string; destinationFolderId: string; destinationDirectory: string
}

/** Accept only bounded routing fields from native settings, including server-issued confirmation identities. */
export function sanitizeExecutionRouting(value: unknown): ExecutionRoutingSettings {
  if (!value || typeof value !== 'object') return { enabled: false, hosts: [] }
  const input = value as Record<string, unknown>
  const hosts: RoutingHostPolicy[] = []
  if (Array.isArray(input.hosts)) for (const raw of input.hosts.slice(0, 128)) {
    if (!raw || typeof raw !== 'object') continue
    const host = raw as Record<string, unknown>
    if (typeof host.hostId !== 'string' || !host.hostId || host.hostId.length > 256 || host.hostId === 'local' || hosts.some((item) => item.hostId === host.hostId) ||
        !Number.isSafeInteger(host.maxActiveTurns) || (host.maxActiveTurns as number) < 0 || (host.maxActiveTurns as number) > 1024) continue
    const providers: RoutingProviderConfirmation[] = []
    if (Array.isArray(host.providers)) for (const p of host.providers.slice(0, 64)) {
      if (p && typeof p.providerId === 'string' && typeof p.connectionIdentity === 'string' && typeof p.providerConfigurationIdentity === 'string' &&
          p.providerId.length <= 256 && p.connectionIdentity.length <= 128 && p.providerConfigurationIdentity.length <= 128 && !providers.some((item) => item.providerId === p.providerId))
        providers.push({ providerId: p.providerId, connectionIdentity: p.connectionIdentity, providerConfigurationIdentity: p.providerConfigurationIdentity })
    }
    const workspaceMappings: RoutingWorkspaceMapping[] = []
    if (Array.isArray(host.workspaceMappings)) for (const m of host.workspaceMappings.slice(0, 256)) {
      if (m && typeof m.sourceFolderId === 'string' && m.sourceFolderId.length <= 256 && typeof m.sourceDirectory === 'string' && m.sourceDirectory.length <= 4096 && typeof m.destinationFolderId === 'string' && m.destinationFolderId.length <= 256)
        workspaceMappings.push({ sourceFolderId: m.sourceFolderId, sourceDirectory: m.sourceDirectory, destinationFolderId: m.destinationFolderId, ...(m.useAutomaticMatch === true ? { useAutomaticMatch: true as const } : {}) })
    }
    hosts.push({ hostId: host.hostId, enabled: host.enabled === true, maxActiveTurns: host.maxActiveTurns as number, providers, workspaceMappings })
  }
  return { enabled: input.enabled === true, hosts, ...(input.repositoryMatching === true ? { repositoryMatching: true } : {}) }
}

export function sanitizeRepositoryMatches(value: unknown): WorkspaceRepositoryMatches[] {
  const list = value && typeof value === 'object' ? (value as { workspaces?: unknown }).workspaces : undefined
  const workspaces: WorkspaceRepositoryMatches[] = []
  if (Array.isArray(list)) for (const raw of list.slice(0, 512)) {
    if (!raw || typeof raw !== 'object') continue
    const item = raw as Record<string, unknown>
    if (typeof item.folderId !== 'string' || !item.folderId) continue
    const repo = item.repository as Record<string, unknown> | null | undefined
    const repository = repo && (repo.kind === 'git' || repo.kind === 'svn') && typeof repo.identity === 'string' && repo.identity
      ? { kind: repo.kind as 'git' | 'svn', identity: repo.identity.slice(0, 1024) } : null
    const matches: RepositoryMatch[] = []
    if (Array.isArray(item.matches)) for (const m of item.matches.slice(0, 128)) {
      if (m && typeof m === 'object' && typeof (m as RepositoryMatch).executionHostId === 'string' && (m as RepositoryMatch).executionHostId &&
          ((m as RepositoryMatch).source === 'automatic' || (m as RepositoryMatch).source === 'manual'))
        matches.push({ executionHostId: (m as RepositoryMatch).executionHostId, remoteDirectory: typeof (m as RepositoryMatch).remoteDirectory === 'string' ? (m as RepositoryMatch).remoteDirectory : null, source: (m as RepositoryMatch).source })
    }
    workspaces.push({ folderId: item.folderId, repository, matches })
  }
  return workspaces
}

/** Plain-language repository label: `git@github.com:owner/repo.git` and `https://github.com/owner/repo` both become `github.com/owner/repo`. */
export function shortRepositoryIdentity(identity: string): string {
  return identity.trim()
    .replace(/^[a-z][a-z0-9+.-]*:\/\//i, '')
    .replace(/^[^@/]+@/, '')
    .replace(/^([^/:]+):(?!\d)/, '$1/')
    .replace(/\.git\/?$/, '')
    .replace(/\/+$/, '')
}

export function routingConfigurationError(settings: ExecutionRoutingSettings, hosts: ExecutionHost[], folders: Folder[]): string {
  if (settings.hosts.length > 128 || new Set(settings.hosts.map((host) => host.hostId)).size !== settings.hosts.length) return 'Select each SSH computer once.'
  for (const policy of settings.hosts) {
    if (!hosts.some((host) => host.id === policy.hostId && isSshHost(host))) return 'Select a registered SSH computer.'
    if (!Number.isSafeInteger(policy.maxActiveTurns) || policy.maxActiveTurns < (policy.enabled ? 1 : 0) || policy.maxActiveTurns > 1024) return 'Concurrent sessions must be a whole number from 1 to 1024 for enabled computers.'
    if (policy.workspaceMappings.some((mapping) => routingMappingProblem(mapping, policy.hostId, folders))) return 'Choose registered source and destination workspaces on the correct computer.'
  }
  return ''
}

/** Why one mapping cannot be used, or '' when it is valid. Page validation and the folder rows share it so they never disagree. */
export function routingMappingProblem(mapping: RoutingWorkspaceMapping, hostId: string, folders: Folder[]): string {
  const source = folders.find((folder) => folder.id === mapping.sourceFolderId)
  if (!source || !source.directory) return 'Workspace no longer exists'
  if (source.directory !== mapping.sourceDirectory) return 'Workspace folder moved'
  if (mapping.useAutomaticMatch) return ''
  const destination = folders.find((folder) => folder.id === mapping.destinationFolderId)
  if (!destination || !destination.directory) return 'Folder no longer exists'
  if (destination.executionHostId !== hostId) return 'Folder is on another computer'
  return ''
}

/** One computer a workspace runs on: the repository match, or a chosen folder (which wins). `error` is the mapping's problem. */
export interface RoutingFolderEntry { hostId: string; kind: 'repository' | 'chosen'; target: string; destinationFolderId?: string; error?: string }
export interface RoutingFolderGroup { folder: Folder; repository: WorkspaceRepositoryMatches['repository']; entries: RoutingFolderEntry[] }

/**
 * The Folders list: one group per local workspace with one entry per computer it is mapped to.
 * Groups with a problem come first, then the other mapped groups (local order kept); workspaces with no entries are `unmapped`.
 * Pass `repositories` only while repository matching is on.
 */
export function routingFolderRows(localFolders: Folder[], sshHosts: ExecutionHost[], settings: ExecutionRoutingSettings, repositories: WorkspaceRepositoryMatches[] | null, folders: Folder[]): { groups: RoutingFolderGroup[]; unmapped: RoutingFolderGroup[] } {
  const all = localFolders.map((folder): RoutingFolderGroup => {
    const workspace = repositories?.find((item) => item.folderId === folder.id)
    const entries: RoutingFolderEntry[] = []
    for (const host of sshHosts) {
      const mappings = settings.hosts.find((policy) => policy.hostId === host.id)?.workspaceMappings.filter((mapping) => mapping.sourceFolderId === folder.id) ?? []
      const mapping = mappings.find((item) => item.sourceDirectory === folder.directory) ?? mappings[0]
      const error = mapping ? routingMappingProblem(mapping, host.id, folders) : ''
      const chosen = mapping && !mapping.useAutomaticMatch
      const destination = chosen ? folders.find((item) => item.id === mapping.destinationFolderId) : undefined
      const target = chosen ? destination?.directory ?? '' : workspace?.matches.find((match) => match.executionHostId === host.id && match.source === 'automatic' && match.remoteDirectory)?.remoteDirectory ?? ''
      if (!error && !target) continue
      entries.push({ hostId: host.id, kind: chosen ? 'chosen' : 'repository', target, ...(destination ? { destinationFolderId: destination.id } : {}), ...(error ? { error } : {}) })
    }
    return { folder, repository: workspace?.repository ?? null, entries }
  })
  const problem = (group: RoutingFolderGroup) => group.entries.some((entry) => entry.error)
  return {
    groups: [...all.filter((group) => group.entries.length && problem(group)), ...all.filter((group) => group.entries.length && !problem(group))],
    unmapped: all.filter((group) => !group.entries.length),
  }
}

/** With repository matching on, a computer qualifies by an automatic repository match or a manual override (override wins). Without it, manual mappings only. */
export function canRouteAutomatically(settings: ExecutionRoutingSettings, folder: Folder | undefined, providerId: string, workspace?: WorkspaceRepositoryMatches): boolean {
  if (!settings.enabled || !folder) return false
  const automaticHosts = settings.repositoryMatching === true
    ? new Set(workspace?.matches.filter((match) => match.source === 'automatic' && match.remoteDirectory).map((match) => match.executionHostId))
    : new Set<string>()
  return settings.hosts.some((host) => host.enabled && host.maxActiveTurns > 0 &&
    host.providers.some((provider) => provider.providerId === providerId && provider.connectionIdentity && provider.providerConfigurationIdentity) &&
    (automaticHosts.has(host.hostId) || host.workspaceMappings.some((mapping) => !mapping.useAutomaticMatch && mapping.sourceFolderId === folder.id && mapping.sourceDirectory === folder.directory)))
}

export function routingReservation(session: Session): RoutingReservation | null {
  if (!session.routingUnresolvedReservation || !session.routingReservationPending || !session.executionHostId || session.executionHostId === 'local' ||
      !session.routingAssignedConnectionIdentity || !session.routingAssignedProviderConfigurationIdentity || !session.folderId || !(session.workspaceWorktreeDirectory || session.workspaceDirectory)) return null
  return { chatId: session.id, executionHostId: session.executionHostId, connectionIdentity: session.routingAssignedConnectionIdentity,
    providerConfigurationIdentity: session.routingAssignedProviderConfigurationIdentity, destinationFolderId: session.folderId, destinationDirectory: session.workspaceWorktreeDirectory || session.workspaceDirectory! }
}
