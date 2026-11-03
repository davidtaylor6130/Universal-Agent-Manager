import { describe, expect, it } from 'vitest'
import { canRouteAutomatically, routingConfigurationError, routingFolderRows, sanitizeExecutionRouting, sanitizeRepositoryMatches, shortRepositoryIdentity, type ExecutionRoutingSettings } from './executionRouting'
import type { ExecutionHost, Folder } from './session'
import { sanitizeCppChat } from '../store/cpp/sanitizers'
import { sessionFromCppChat } from '../store/cpp/reconcile'
const hosts: ExecutionHost[] = [{ id: 'ssh-a', label: 'Server', transport: 'ssh', sshAlias: 'server', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }]
const folders: Folder[] = [
  { id: 'source', name: 'Source', directory: '/source', parentId: null, isExpanded: true, createdAt: new Date() },
  { id: 'target', name: 'Target', directory: '/remote', executionHostId: 'ssh-a', parentId: null, isExpanded: true, createdAt: new Date() },
]
const settings: ExecutionRoutingSettings = { enabled: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 2, providers: [{ providerId: 'codex-cli', connectionIdentity: 'host-version', providerConfigurationIdentity: 'provider-version' }], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] }] }
describe('execution routing contracts', () => {
  it('reconciles changed native wait and unresolved ownership while retaining unchanged session references', () => {
    const raw = { id: 'chat', title: 'Chat', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z', folderId: 'target', executionHostId: 'ssh-a', workspaceDirectory: '/remote', routingMode: 'automatic', routingWaitReason: 'Computers are busy.', routingUnresolvedReservation: true, routingReservationPending: true, routingAssignedConnectionIdentity: 'host-v1', routingAssignedProviderConfigurationIdentity: 'provider-v1' }
    const first = sessionFromCppChat(sanitizeCppChat(raw)!, undefined, [])
    expect(first.routingWaitReason).toBe('Computers are busy.')
    expect(first.routingUnresolvedReservation).toBe(true)
    const settled = sessionFromCppChat(sanitizeCppChat({ ...raw, routingWaitReason: '', routingUnresolvedReservation: false })!, first, [])
    expect(settled).not.toBe(first)
    expect(settled.routingWaitReason).toBe('')
    expect(settled.routingUnresolvedReservation).toBe(false)
    expect(sessionFromCppChat(sanitizeCppChat({ ...raw, routingWaitReason: '', routingUnresolvedReservation: false })!, settled, [])).toBe(settled)
  })
  it('requires integer slots and an exact registered destination on the selected computer', () => {
    expect(routingConfigurationError(settings, hosts, folders)).toBe('')
    expect(routingConfigurationError({ ...settings, hosts: [{ ...settings.hosts[0], maxActiveTurns: 1.5 }] }, hosts, folders)).toContain('whole number')
    expect(routingConfigurationError(settings, hosts, folders.map((folder) => ({ ...folder, executionHostId: 'other' })))).toContain('correct computer')
    expect(canRouteAutomatically(settings, folders[0], 'codex-cli')).toBe(true)
    expect(canRouteAutomatically(settings, { ...folders[0], directory: '/different-base' }, 'codex-cli')).toBe(false)
    expect(canRouteAutomatically({ ...settings, enabled: false }, folders[0], 'codex-cli')).toBe(false)
  })
  it('whitelists bounded native settings without accepting extra payloads or fabricated confirmation', () => {
    const clean = sanitizeExecutionRouting({ ...settings, extra: 'secret', hosts: [{ ...settings.hosts[0], extra: 'secret', providers: [{ ...settings.hosts[0].providers[0], confirm: true, extra: 'secret' }] }] })
    expect(clean).toEqual(settings)
    expect(sanitizeExecutionRouting({ enabled: true, hosts: [{ ...settings.hosts[0], maxActiveTurns: Infinity }] }).hosts).toEqual([])
  })
})

describe('repository matching contracts', () => {
  it('shortens git and svn identities and keeps only well-formed matches', () => {
    expect(shortRepositoryIdentity('git@github.com:owner/repo.git')).toBe('github.com/owner/repo')
    expect(shortRepositoryIdentity('https://github.com/owner/repo.git')).toBe('github.com/owner/repo')
    expect(shortRepositoryIdentity('svn://svn.example.com/repos/app/')).toBe('svn.example.com/repos/app')
    expect(sanitizeRepositoryMatches({ workspaces: [
      { folderId: 'a', repository: { kind: 'hg', identity: 'x' }, matches: [{ executionHostId: 'ssh-a', remoteDirectory: '/r', source: 'automatic' }, { executionHostId: '', source: 'manual' }] },
      { folderId: '', repository: null, matches: [] },
    ] })).toEqual([{ folderId: 'a', repository: null, matches: [{ executionHostId: 'ssh-a', remoteDirectory: '/r', source: 'automatic' }] }])
    expect(sanitizeRepositoryMatches(null)).toEqual([])
    expect(sanitizeExecutionRouting({ enabled: true, hosts: [], repositoryMatching: true }).repositoryMatching).toBe(true)
    expect(sanitizeExecutionRouting({ enabled: true, hosts: [], repositoryMatching: 'yes' })).not.toHaveProperty('repositoryMatching')
  })
  it('routes by automatic repository match only when the capability is on, and manual overrides still count', () => {
    const match = { folderId: 'source', repository: null, matches: [{ executionHostId: 'ssh-a', remoteDirectory: '/remote', source: 'automatic' as const }] }
    const bare = { ...settings, hosts: [{ ...settings.hosts[0], workspaceMappings: [] }] }
    expect(canRouteAutomatically(bare, folders[0], 'codex-cli', match)).toBe(false)
    expect(canRouteAutomatically({ ...bare, repositoryMatching: true }, folders[0], 'codex-cli', match)).toBe(true)
    expect(canRouteAutomatically({ ...bare, repositoryMatching: true }, folders[0], 'codex-cli')).toBe(false)
    expect(canRouteAutomatically({ ...bare, repositoryMatching: false }, folders[0], 'codex-cli', match)).toBe(false)
    expect(canRouteAutomatically({ ...bare, repositoryMatching: true }, folders[0], 'codex-cli', { ...match, matches: [{ ...match.matches[0], source: 'manual' }] })).toBe(false)
    expect(canRouteAutomatically({ ...settings, repositoryMatching: true }, folders[0], 'codex-cli')).toBe(true)
  })
  it('treats a cleared automatic-match entry as no override', () => {
    const cleared = { ...settings, hosts: [{ ...settings.hosts[0], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: '', useAutomaticMatch: true as const }] }] }
    expect(routingConfigurationError(cleared, hosts, folders)).toBe('')
    expect(canRouteAutomatically(cleared, folders[0], 'codex-cli')).toBe(false)
  })
})

describe('routing folder rows', () => {
  const two: ExecutionHost[] = [hosts[0], { ...hosts[0], id: 'ssh-b', label: 'Other' }]
  const local = (id: string) => ({ id, name: id, directory: `/${id}`, parentId: null, isExpanded: true, createdAt: new Date() })
  const sources = [local('plain'), local('mapped'), local('broken'), local('moved')]
  const all: Folder[] = [...sources, { ...folders[1], id: 'remote-a' }, { ...folders[1], id: 'remote-b', directory: '/remote-b', executionHostId: 'ssh-b' }]
  const routing: ExecutionRoutingSettings = { enabled: true, hosts: [
    { hostId: 'ssh-a', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [
      { sourceFolderId: 'broken', sourceDirectory: '/broken', destinationFolderId: 'gone' },
      { sourceFolderId: 'moved', sourceDirectory: '/old', destinationFolderId: 'remote-a' },
    ] },
    { hostId: 'ssh-b', enabled: true, maxActiveTurns: 1, providers: [], workspaceMappings: [
      { sourceFolderId: 'mapped', sourceDirectory: '/mapped', destinationFolderId: 'remote-b' },
      { sourceFolderId: 'broken', sourceDirectory: '/broken', destinationFolderId: 'remote-a' },
    ] },
  ] }
  const repositories = [{ folderId: 'mapped', repository: { kind: 'git' as const, identity: 'git@github.com:o/r.git' }, matches: [{ executionHostId: 'ssh-a', remoteDirectory: '/checkout', source: 'automatic' as const }] }]

  it('puts problem workspaces first, then mapped ones, and collects the rest as not mapped', () => {
    const { groups, unmapped } = routingFolderRows(sources, two, routing, repositories, all)
    expect(groups.map((group) => group.folder.id)).toEqual(['broken', 'moved', 'mapped'])
    expect(unmapped.map((group) => group.folder.id)).toEqual(['plain'])
    expect(groups[0].entries).toEqual([
      { hostId: 'ssh-a', kind: 'chosen', target: '', error: 'Folder no longer exists' },
      { hostId: 'ssh-b', kind: 'chosen', target: '/remote', destinationFolderId: 'remote-a', error: 'Folder is on another computer' },
    ])
    expect(groups[1].entries[0].error).toBe('Workspace folder moved')
    expect(groups[2]).toMatchObject({ repository: { identity: 'git@github.com:o/r.git' }, entries: [
      { hostId: 'ssh-a', kind: 'repository', target: '/checkout' },
      { hostId: 'ssh-b', kind: 'chosen', target: '/remote-b', destinationFolderId: 'remote-b' },
    ] })
  })

  it('ignores repository matches when matching is off and agrees with page validation', () => {
    const { groups } = routingFolderRows(sources, two, routing, null, all)
    expect(groups.find((group) => group.folder.id === 'mapped')!.entries.map((entry) => entry.kind)).toEqual(['chosen'])
    expect(routingConfigurationError(routing, two, all)).not.toBe('')
    const fixed = { ...routing, hosts: routing.hosts.map((host) => ({ ...host, workspaceMappings: host.workspaceMappings.filter((mapping) => mapping.sourceFolderId === 'mapped') })) }
    expect(routingFolderRows(sources, two, fixed, null, all).groups.flatMap((group) => group.entries).some((entry) => entry.error)).toBe(false)
    expect(routingConfigurationError(fixed, two, all)).toBe('')
    const cleared = { ...fixed, hosts: [{ ...fixed.hosts[0], workspaceMappings: [{ sourceFolderId: 'mapped', sourceDirectory: '/mapped', destinationFolderId: '', useAutomaticMatch: true as const }] }] }
    expect(routingFolderRows(sources, two, cleared, repositories, all).groups[0].entries).toEqual([{ hostId: 'ssh-a', kind: 'repository', target: '/checkout' }])
  })
})
