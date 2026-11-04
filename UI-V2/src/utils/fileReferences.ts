export interface FileReference {
  path: string
  line?: number
  endLine?: number
}

/** Recognizes file references; the owning native workspace still authorizes every read. */
export function parseFileReference(value: string): FileReference | null {
  let path = value.trim().replace(/^<(.*)>$/, '$1')
  if (!path || path.length > 16384 || /[\x00-\x1f\x7f]/.test(path)) return null
  if (/^file:\/\//i.test(path)) {
    try {
      const url = new URL(path)
      if (url.hostname && url.hostname !== 'localhost') return null
      path = decodeURIComponent(url.pathname) + url.hash
      if (/^\/[A-Za-z]:\//.test(path)) path = path.slice(1)
    } catch { return null }
  } else if (/^[a-z][a-z\d+.-]*:/i.test(path) && !/^[A-Za-z]:[\\/]/.test(path)) return null
  const range = /(?:#L(\d+)(?:-L?(\d+))?|:(\d+)(?:-(\d+))?)$/.exec(path)
  let line: number | undefined
  let endLine: number | undefined
  if (range) {
    line = Number(range[1] ?? range[3])
    endLine = range[2] || range[4] ? Number(range[2] ?? range[4]) : undefined
    if (!Number.isSafeInteger(line) || line < 1 || line > 2_147_483_647 || (endLine !== undefined && (!Number.isSafeInteger(endLine) || endLine < line || endLine > 2_147_483_647))) return null
    path = path.slice(0, range.index)
  }
  if (!path || /[\x00-\x1f\x7f]/.test(path) || (!/[\\/]/.test(path) && !/\.[a-z\d_-]+$/i.test(path))) return null
  return { path, ...(line !== undefined ? { line } : {}), ...(endLine !== undefined ? { endLine } : {}) }
}

/** Inline code requires a recognizable source filename, so ordinary code remains plain text. */
export function inlineFileReference(value: string): FileReference | null {
  if (/[(){};<>]/.test(value)) return null
  const reference = parseFileReference(value)
  return reference && /\.(?:[cm]?[jt]sx?|[ch](?:pp|xx)?|cs|py|rs|go|java|swift|kt|json|ya?ml|toml|md|txt|css|scss|html|xml|sh|ps1|bat|sql|vue|svelte)$/i.test(reference.path) ? reference : null
}

/** Converts missing/deleted absolute references for repository APIs; native code still authorizes the path. */
export function workspaceRelativeFilePath(path: string, workspace: string): string | null {
  const windows = /^[A-Za-z]:[\\/]/.test(workspace) || workspace.startsWith('\\\\')
  const normalize = (value: string) => windows ? value.replace(/\\/g, '/') : value
  const root = normalize(workspace).replace(/\/+$/, '')
  let relative = normalize(path)
  if (!relative || relative.length > 16384 || /[\x00-\x1f\x7f]/.test(relative)) return null
  const absolute = relative.startsWith('/') || /^[A-Za-z]:/.test(relative)
  if (absolute) {
    const prefix = root + '/'
    if (!root || !(windows ? relative.toLowerCase().startsWith(prefix.toLowerCase()) : relative.startsWith(prefix))) return null
    relative = relative.slice(prefix.length)
  }
  const parts: string[] = []
  for (const part of relative.split('/')) {
    if (!part || part === '.') continue
    if (part === '..') { if (!parts.length) return null; parts.pop() }
    else parts.push(part)
  }
  return parts.length ? parts.join('/') : null
}
