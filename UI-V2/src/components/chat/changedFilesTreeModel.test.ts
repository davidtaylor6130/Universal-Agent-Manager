import { describe, expect, it } from 'vitest'
import { buildChangedFilesTree, defaultExpandedFolderKeys, type ChangedFileNode, type ChangedFolderNode } from './changedFilesTreeModel'

const file = (path: string, additions = 1, deletions = 0, binary = false) => ({ path, additions, deletions, binary })

describe('buildChangedFilesTree', () => {
  it('collapses single-child folder chains and aggregates folder counts', () => {
    const tree = buildChangedFilesTree([
      file('UI-V2/src/components/a.tsx', 3, 1),
      file('UI-V2/src/components/b.tsx', 2, 2),
      file('README.md', 1, 0),
    ])

    expect(tree.map((node) => node.name)).toEqual(['UI-V2/src/components', 'README.md'])
    const folder = tree[0] as ChangedFolderNode<ReturnType<typeof file>>
    expect(folder.kind).toBe('folder')
    expect(folder.path).toBe('UI-V2/src/components')
    expect(folder.additions).toBe(5)
    expect(folder.deletions).toBe(3)
    expect(folder.children.map((node) => node.name)).toEqual(['a.tsx', 'b.tsx'])
    expect(folder.children.every((node) => node.depth === 1)).toBe(true)
  })

  it('keeps a folder split when it has files beside a subfolder, and skips binary counts', () => {
    const tree = buildChangedFilesTree([
      file('src/index.ts', 4, 0),
      file('src/lib/util.ts', 1, 1),
      file('src/img.png', 9, 9, true),
    ])

    const src = tree[0] as ChangedFolderNode<ReturnType<typeof file>>
    expect(src.name).toBe('src')
    expect(src.children.map((node) => node.name)).toEqual(['lib', 'img.png', 'index.ts'])
    expect(src.additions).toBe(5)
    expect(src.deletions).toBe(1)
    const image = src.children[1] as ChangedFileNode<ReturnType<typeof file>>
    expect(image.file.binary).toBe(true)
  })

  it('opens every folder for small reviews and only the top two levels for large ones', () => {
    const small = [file('a/s.ts'), file('a/p/q/x.ts'), file('a/p/q/r/y.ts')]
    const large = [...small, ...Array.from({ length: 7 }, (_, index) => file(`a/p/q/r/z${index}.ts`))]

    expect([...defaultExpandedFolderKeys(buildChangedFilesTree(small), small.length)].sort()).toEqual(['a', 'a/p/q', 'a/p/q/r'])
    expect([...defaultExpandedFolderKeys(buildChangedFilesTree(large), large.length)].sort()).toEqual(['a', 'a/p/q'])
  })
})
