import { expect, it } from 'vitest'
import type { Folder, Session } from '../types/session'
import { composerWorkspaceName } from './workspacePresentation'

const session: Session = { id: 'chat', name: 'Chat', folderId: 'project', viewMode: 'chat', createdAt: new Date(0), updatedAt: new Date(0) }
const folder: Folder = { id: 'project', name: 'Original project', directory: '/projects/source', parentId: null, isExpanded: true, createdAt: new Date(0) }

it('names a worktree after its source workspace without changing its working directory', () => {
  const worktree = { ...session, workspaceSourceDirectory: '/projects/source', workspaceDirectory: '/tmp/chat-random' }
  expect(composerWorkspaceName(worktree, [folder], worktree.workspaceDirectory)).toBe('Original project')
  expect(worktree.workspaceDirectory).toBe('/tmp/chat-random')
})

it('matches Windows source paths only on the same host', () => {
  const worktree = { ...session, executionHostId: 'windows', workspaceSourceDirectory: 'C:\\Projects\\UAM\\' }
  const wrongHost = { ...folder, name: 'Wrong machine', directory: 'C:/Projects/UAM' }
  const rightHost = { ...wrongHost, id: 'remote-project', executionHostId: 'windows', name: 'Windows project', directory: 'c:/projects/uam' }
  expect(composerWorkspaceName(worktree, [wrongHost, rightHost], 'C:\\Temp\\chat-random')).toBe('Windows project')
  expect(composerWorkspaceName(worktree, [wrongHost], 'C:\\Temp\\chat-random')).toBe('UAM')
})

it('preserves POSIX path case and falls back to the source name when the folder is absent', () => {
  expect(composerWorkspaceName({ ...session, workspaceSourceDirectory: '/Projects/Source/' }, [folder], '/tmp/chat-random')).toBe('Source')
  expect(composerWorkspaceName(session, [folder], '/projects/source')).toBe('Original project')
  expect(composerWorkspaceName(session, [], '')).toBe('No workspace')
})
