export function isAbsoluteRemoteWorkspace(platform: string | undefined, value: string) {
  const path = value.trim()
  return platform?.toLowerCase() === 'windows'
    ? /^[a-z]:[\\/]/i.test(path) || /^\\\\/.test(path)
    : path.startsWith('/')
}

export function defaultRemoteBrowsePath(platform: string | undefined) {
  return platform?.toLowerCase() === 'windows' ? 'C:\\' : '/'
}
