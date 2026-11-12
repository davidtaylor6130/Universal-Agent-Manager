import type { SvnRepositoryLayout } from '../store/cpp/types'

export type SvnLayoutFolders = Pick<SvnRepositoryLayout, 'trunk' | 'branches' | 'tags'>

export const STANDARD_SVN_LAYOUT: SvnLayoutFolders = { trunk: 'trunk', branches: 'branches', tags: 'tags' }

const withoutTrailingSlash = (value: string) => value.trim().replace(/(.)\/+$/, '$1')

/** The layout saved for this repository, matched by its root URL or UUID; the standard layout otherwise. */
export function svnLayoutFor(layouts: readonly SvnRepositoryLayout[] | undefined, repositoryRoot: string, repositoryUuid: string): SvnLayoutFolders {
  const root = withoutTrailingSlash(repositoryRoot)
  const found = (layouts ?? []).find((layout) => {
    const key = withoutTrailingSlash(layout.repository)
    return key !== '' && (key.toLowerCase() === root.toLowerCase() || key.toLowerCase() === repositoryUuid.trim().toLowerCase())
  })
  return found ?? STANDARD_SVN_LAYOUT
}

export type SvnLayoutLabel = { kind: 'trunk' | 'branch' | 'tag' | 'path'; name: string }

/**
 * Names a repository path from the repository's layout: the main line, a branch or a tag. Layout folders may sit under a
 * project folder (`/shop/trunk`) and may be more than one folder deep (`lines/dev`). Display only, never used to act.
 */
export function svnLayoutLabel(location: string, layout: SvnLayoutFolders = STANDARD_SVN_LAYOUT): SvnLayoutLabel {
  const segments = location.replace(/^\^/, '').split('/').filter(Boolean)
  const folders = { trunk: layout.trunk.split('/').filter(Boolean), branches: layout.branches.split('/').filter(Boolean), tags: layout.tags.split('/').filter(Boolean) }
  const startsAt = (index: number, folder: string[]) => folder.length > 0 && folder.every((part, offset) => segments[index + offset] === part)
  for (let index = 0; index < segments.length; index += 1) {
    if (startsAt(index, folders.trunk)) return { kind: 'trunk', name: layout.trunk }
    if (startsAt(index, folders.branches) && segments[index + folders.branches.length]) return { kind: 'branch', name: segments[index + folders.branches.length] }
    if (startsAt(index, folders.tags) && segments[index + folders.tags.length]) return { kind: 'tag', name: segments[index + folders.tags.length] }
  }
  return { kind: 'path', name: segments.length ? `/${segments.join('/')}` : 'Repository root' }
}

export function svnLayoutText(location: string, layout?: SvnLayoutFolders): string {
  const label = svnLayoutLabel(location, layout)
  return label.kind === 'branch' ? `Branch ${label.name}` : label.kind === 'tag' ? `Tag ${label.name}` : label.name
}
