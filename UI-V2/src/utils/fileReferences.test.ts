import { describe, expect, it } from 'vitest'
import { inlineFileReference, parseFileReference, workspaceRelativeFilePath } from './fileReferences'

describe('Workspace file references', () => {
  it('retains macOS, Windows and relative paths with source ranges', () => {
    expect(parseFileReference('/workspace/src/main.cpp:42')).toEqual({ path: '/workspace/src/main.cpp', line: 42 })
    expect(parseFileReference('C:\\Workspace\\main.cpp:8-12')).toEqual({ path: 'C:\\Workspace\\main.cpp', line: 8, endLine: 12 })
    expect(parseFileReference('src/view.tsx#L15-L20')).toEqual({ path: 'src/view.tsx', line: 15, endLine: 20 })
    expect(parseFileReference('<src/café file.txt>')).toEqual({ path: 'src/café file.txt' })
  })
  it('decodes local file URLs while rejecting remote URLs and active schemes', () => {
    expect(parseFileReference('file:///work/caf%C3%A9%20file.txt#L2')).toEqual({ path: '/work/café file.txt', line: 2 })
    expect(parseFileReference('file:///C:/work/main.cpp:4')).toEqual({ path: 'C:/work/main.cpp', line: 4 })
    for (const value of ['https://site/file.ts', 'mailto:user@example.com', 'javascript:alert(1)', 'data:text/plain,x', 'file://other-host/private.txt', 'file:///work/%00.txt']) expect(parseFileReference(value)).toBeNull()
  })
  it('rejects invalid ranges, control bytes and unbounded references', () => {
    for (const value of ['file.ts:0', 'file.ts:8-2', 'file.ts:999999999999999999', 'file.ts#L1-L2147483648', 'file\n.ts', 'x'.repeat(16385)]) expect(parseFileReference(value)).toBeNull()
  })
  it('does not turn ordinary inline code into file buttons', () => {
    for (const value of ['object.method', 'call(file.ts)', 'const x = 1;', 'hello', 'npm/run']) expect(inlineFileReference(value)).toBeNull()
    expect(inlineFileReference('src/main.cpp:7')).toEqual({ path: 'src/main.cpp', line: 7 })
    expect(inlineFileReference('README.md')).toEqual({ path: 'README.md' })
  })
})

it('converts deleted paths without escaping the selected POSIX or Windows workspace', () => {
  expect(workspaceRelativeFilePath('/work/src/deleted.cpp', '/work')).toBe('src/deleted.cpp')
  expect(workspaceRelativeFilePath('C:\\WORK\\src\\deleted.cpp', 'c:\\work')).toBe('src/deleted.cpp')
  expect(workspaceRelativeFilePath('src/../deleted.cpp', '/work')).toBe('deleted.cpp')
  expect(workspaceRelativeFilePath('/work-other/deleted.cpp', '/work')).toBeNull()
  expect(workspaceRelativeFilePath('/WORK/deleted.cpp', '/work')).toBeNull()
  expect(workspaceRelativeFilePath('../deleted.cpp', '/work')).toBeNull()
  expect(workspaceRelativeFilePath('/work/../../secret.cpp', '/work')).toBeNull()
  expect(workspaceRelativeFilePath('D:/work/file.cpp', 'C:/work')).toBeNull()
})
