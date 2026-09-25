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
