import { SSH_ENABLED } from '../config/buildFeatures'

/** SSH hosts exist only in builds that include SSH; the check folds to false otherwise. */
export function isSshHost(host?: { transport?: string } | null): boolean {
  return SSH_ENABLED && host?.transport === 'ssh'
}

export function isRemoteDirectoryBrowseAvailable(host: { runnerStatus: string; runnerVersion: string }) {
  return host.runnerVersion.trim().length > 0 && ['ready', 'offline', 'error'].includes(host.runnerStatus)
}

export function isAbsoluteRemoteWorkspace(platform: string | undefined, value: string) {
  const path = value.trim()
  return platform?.toLowerCase() === 'windows'
    ? /^[a-z]:[\\/]/i.test(path) || /^\\\\/.test(path)
    : path.startsWith('/')
}

export function defaultRemoteBrowsePath(platform: string | undefined) {
  return platform?.toLowerCase() === 'windows' ? 'C:\\' : '/'
}
