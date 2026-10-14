export interface ChangedPathLike {
  path: string
  additions: number
  deletions: number
  binary: boolean
}

export interface ChangedFolderNode<T extends ChangedPathLike> {
  kind: 'folder'
  key: string
  /** Display name; single-child folder chains are joined, e.g. `UI-V2/src/components`. */
  name: string
  path: string
  depth: number
  additions: number
  deletions: number
  children: ChangedTreeNode<T>[]
}

export interface ChangedFileNode<T extends ChangedPathLike> {
  kind: 'file'
  key: string
  name: string
  path: string
  depth: number
  file: T
}

export type ChangedTreeNode<T extends ChangedPathLike> = ChangedFolderNode<T> | ChangedFileNode<T>

interface MutableFolder<T extends ChangedPathLike> {
  name: string
  path: string
  folders: Map<string, MutableFolder<T>>
  files: T[]
}

/** Builds a directory tree from changed file paths, collapsing single-child folder chains. */
export function buildChangedFilesTree<T extends ChangedPathLike>(files: T[]): ChangedTreeNode<T>[] {
  const root: MutableFolder<T> = { name: '', path: '', folders: new Map(), files: [] }
  for (const file of files) {
    const segments = file.path.split('/').filter(Boolean)
    let folder = root
    for (const segment of segments.slice(0, -1)) {
      const path = folder.path ? `${folder.path}/${segment}` : segment
      let next = folder.folders.get(segment)
      if (!next) {
        next = { name: segment, path, folders: new Map(), files: [] }
        folder.folders.set(segment, next)
      }
      folder = next
    }
    folder.files.push(file)
  }
  return toNodes(root, 0)
}

function toNodes<T extends ChangedPathLike>(folder: MutableFolder<T>, depth: number): ChangedTreeNode<T>[] {
  const folders: ChangedFolderNode<T>[] = [...folder.folders.values()].map((child) => {
    let current = child
    let name = child.name
    // ponytail: only pure single-child chains collapse; a folder with a file beside it stays split.
    while (current.files.length === 0 && current.folders.size === 1) {
      current = [...current.folders.values()][0]
      name = `${name}/${current.name}`
    }
    const children = toNodes(current, depth + 1)
    return {
      kind: 'folder' as const,
      key: current.path,
      name,
      path: current.path,
      depth,
      additions: sum(children, 'additions'),
      deletions: sum(children, 'deletions'),
      children,
    }
  })
  const fileNodes: ChangedFileNode<T>[] = folder.files.map((file) => ({
    kind: 'file' as const,
    key: file.path,
    name: file.path.split('/').pop() ?? file.path,
    path: file.path,
    depth,
    file,
  }))
  return [...folders.sort((a, b) => a.name.localeCompare(b.name)), ...fileNodes.sort((a, b) => a.name.localeCompare(b.name))]
}

function sum<T extends ChangedPathLike>(nodes: ChangedTreeNode<T>[], field: 'additions' | 'deletions'): number {
  return nodes.reduce((total, node) => {
    if (node.kind === 'folder') return total + node[field]
    return total + (node.file.binary ? 0 : node.file[field])
  }, 0)
}

/**
 * Folder keys expanded by default. With more than `collapseAbove` files, folders
 * deeper than the first two levels start collapsed; smaller reviews open fully.
 */
export function defaultExpandedFolderKeys<T extends ChangedPathLike>(
  nodes: ChangedTreeNode<T>[],
  fileCount: number,
  collapseAbove = 8,
): Set<string> {
  const keys = new Set<string>()
  const visit = (list: ChangedTreeNode<T>[]) => {
    for (const node of list) {
      if (node.kind !== 'folder') continue
      if (fileCount <= collapseAbove || node.depth < 2) keys.add(node.key)
      visit(node.children)
    }
  }
  visit(nodes)
  return keys
}

export function changedLineTotals(files: ChangedPathLike[]): { additions: number; deletions: number } {
  return files.reduce((total, file) => (file.binary
    ? total
    : { additions: total.additions + file.additions, deletions: total.deletions + file.deletions }), { additions: 0, deletions: 0 })
}
