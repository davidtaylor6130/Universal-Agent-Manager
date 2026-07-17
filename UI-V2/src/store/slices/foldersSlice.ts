import type { Folder } from '../../types/session'
import type { MemoryEntry, MemoryEntryDraft, MemoryScope, MemoryScanCandidate } from '../../types/memory'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../../types/markdownStore'
import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
import { pendingRequestIdsByKey } from '../push/pushBuffers'
import {
  clearPendingRequest,
  isLatestPendingRequest,
  rememberPendingRequest,
} from '../cpp/reconcile'
import type { CppFolder, ShellAction } from '../cpp/types'
import { DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID } from '../../utils/providerMetadata'
import type { ZustandSet, ZustandGet } from '../storeTypes'

let folderCounter = 10

function makeId(prefix: string, counter: number) {
  return `${prefix}-${counter}`
}

export function createFoldersSlice(set: ZustandSet, get: ZustandGet) {
  return {
    markdownStoreDirectory: '',
    memoryLibraryScope: null as MemoryScope | null,
    memoryLibraryEntries: [] as MemoryEntry[],
    memoryLibraryLoading: false,
    memoryLibraryError: '',
    isMemoryScanModalOpen: false,
    memoryScanCandidates: [] as MemoryScanCandidate[],
    selectedMemoryScanChatIds: [] as string[],
    memoryScanLoading: false,
    memoryScanRunning: false,
    memoryScanError: '',
    isMarkdownStoreOpen: false,
    markdownStoreEntries: [] as MarkdownStoreEntry[],
    markdownStoreLoading: false,
    markdownStoreError: '',
    markdownStoreAttachedBySessionId: {} as Record<string, MarkdownStoreEntry[]>,
    shellActions: [] as ShellAction[],
    shellActionNotification: '',

    addFolder: (name: string, _parentId: string | null, directory: string) => {
      if (isCefContext()) {
        return sendToCEF<CppFolder>({ action: 'createFolder', payload: { title: name, directory } }).then((resp) => {
          if (!resp.ok || !resp.data?.id) {
            if (!resp.ok) console.error('[CEF] createFolder failed:', resp.error)
            return false
          }

          set((state) => {
            if (state.folders.some((folder) => folder.id === resp.data!.id)) {
              return {}
            }

            const createdFolder: Folder = {
              id: resp.data!.id,
              name: resp.data!.title,
              parentId: null,
              directory: resp.data!.directory ?? '',
              isExpanded: !resp.data!.collapsed,
              createdAt: new Date(),
            }

            return {
              folders: [...state.folders, createdFolder],
            }
          })
          return true
        })
      }

      folderCounter++
      const folder: Folder = {
        id: makeId('f', folderCounter),
        name,
        parentId: null,
        directory,
        isExpanded: true,
        createdAt: new Date(),
      }
      set((state) => ({ folders: [...state.folders, folder] }))
      return Promise.resolve(true)
    },

    toggleFolder: (id: string) => {
      if (isCefContext()) {
        const currentFolder = get().folders.find((folder) => folder.id === id)
        if (!currentFolder) {
          return
        }

        const requestKey = `toggleFolder:${id}`
        const requestId = createRequestId('toggleFolder')
        rememberPendingRequest(requestKey, requestId)
        const previousExpanded = currentFolder.isExpanded
        set((state) => ({
          folders: state.folders.map((f) =>
            f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
          ),
        }))
        sendToCEF({ action: 'toggleFolder', payload: { folderId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => ({
            folders: state.folders.map((f) =>
              f.id === id ? { ...f, isExpanded: previousExpanded } : f
            ),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set((state) => ({
        folders: state.folders.map((f) =>
          f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
        ),
      }))
    },

    reorderFolders: async (folderIds: string[]): Promise<boolean> => {
      const previous = get().folders
      const byId = new Map(previous.map((folder) => [folder.id, folder]))
      const seen = new Set<string>()
      const reordered = folderIds.flatMap((id) => {
        const folder = byId.get(id)
        return folder && !seen.has(id) ? (seen.add(id), [folder]) : []
      })
      reordered.push(...previous.filter((folder) => !seen.has(folder.id)))
      set({ folders: reordered })
      if (!isCefContext()) return true

      const response = await sendToCEF({
        action: 'reorderFolders',
        payload: { folderIds: reordered.map((folder) => folder.id) },
        requestId: createRequestId('reorderFolders'),
      })
      if (!response.ok) set({ folders: previous })
      return response.ok
    },

    renameFolder: (id: string, name: string, directory: string) => {
      if (isCefContext()) {
        const previousFolder = get().folders.find((folder) => folder.id === id)
        if (!previousFolder) {
          return
        }

        const requestKey = `renameFolder:${id}`
        const requestId = createRequestId('renameFolder')
        rememberPendingRequest(requestKey, requestId)
        set((state) => ({
          folders: state.folders.map((folder) =>
            folder.id === id ? { ...folder, name, directory } : folder
          ),
        }))
        sendToCEF({ action: 'renameFolder', payload: { folderId: id, title: name, directory }, requestId }).then(
          (resp) => {
            if (resp.ok) {
              clearPendingRequest(requestKey, resp.requestId)
              return
            }

            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
              return
            }

            set((state) => ({
              folders: state.folders.map((folder) => (folder.id === id ? previousFolder : folder)),
            }))
            pendingRequestIdsByKey.delete(requestKey)
          }
        )
        return
      }

      set((state) => ({
        folders: state.folders.map((f) => (f.id === id ? { ...f, name, directory } : f)),
      }))
    },

    deleteFolder: (id: string) => {
      if (isCefContext()) {
        const deletedFolder = get().folders.find((folder) => folder.id === id)
        if (!deletedFolder) {
          return
        }

        const requestId = createRequestId('deleteFolder')
        sendToCEF({ action: 'deleteFolder', payload: { folderId: id }, requestId }).then((resp) => {
          if (!resp.ok) {
            console.error('[CEF] deleteFolder failed:', resp.error)
          }
        })
        return
      }

      set((state) => {
        const deletedSessionIds = new Set(
          state.sessions.filter((session) => session.folderId === id).map((session) => session.id)
        )
        const remainingFolders = state.folders.filter((f) => f.id !== id)
        const sessions = state.sessions.filter((session) => !deletedSessionIds.has(session.id))
        const messages = { ...state.messages }
        const cliBindingBySessionId = { ...state.cliBindingBySessionId }
        const acpBindingBySessionId = { ...state.acpBindingBySessionId }
        const cliTranscriptBySessionId = { ...state.cliTranscriptBySessionId }

        deletedSessionIds.forEach((sessionId) => {
          delete messages[sessionId]
          delete cliBindingBySessionId[sessionId]
          delete acpBindingBySessionId[sessionId]
          delete cliTranscriptBySessionId[sessionId]
        })

        return {
          folders: remainingFolders,
          sessions,
          messages,
          cliBindingBySessionId,
          acpBindingBySessionId,
          cliTranscriptBySessionId,
          activeSessionId:
            state.activeSessionId !== null && deletedSessionIds.has(state.activeSessionId)
              ? (sessions[0]?.id ?? null)
              : state.activeSessionId,
        }
      })
    },

    browseFolderDirectory: async (currentValue: string) => {
      if (!isCefContext()) {
        return null
      }

      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseFolderDirectory',
        payload: { currentValue },
      })

      const selectedPath = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selectedPath.length > 0 ? selectedPath : null
    },

    browseMarkdownStoreDirectory: async (currentValue: string) => {
      if (!isCefContext()) {
        return null
      }

      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseMarkdownStoreDirectory',
        payload: { currentValue },
      })

      const selectedPath = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selectedPath.length > 0 ? selectedPath : null
    },

    setMarkdownStoreDirectory: async (directory: string) => {
      const trimmed = directory.trim()
      if (isCefContext()) {
        const response = await sendToCEF<{ directory?: string }>({
          action: 'setMarkdownStoreDirectory',
          payload: { directory: trimmed },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to save Markdown Store directory.' })
          return false
        }
        set({
          markdownStoreDirectory: response.data?.directory ?? trimmed,
          markdownStoreError: '',
        })
        return true
      }

      set({ markdownStoreDirectory: trimmed, markdownStoreError: '' })
      return true
    },

    setShellActions: async (actions: ShellAction[]) => {
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'setShellActions', payload: { actions } })
        if (!response.ok) {
          set({ shellActionNotification: response.error ?? 'Failed to save shell actions.' })
          return false
        }
      }
      set({ shellActions: actions, shellActionNotification: 'Shell action settings saved. Choose Apply to update Finder or Explorer.' })
      return true
    },

    applyShellActions: async () => {
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'applyShellActions' })
        if (!response.ok) {
          set({ shellActionNotification: response.error ?? 'Failed to apply shell actions.' })
          return false
        }
      }
      set({ shellActionNotification: 'Shell actions applied successfully.' })
      return true
    },

    openMarkdownStore: async () => {
      set({ isMarkdownStoreOpen: true, markdownStoreLoading: true, markdownStoreError: '' })
      return get().refreshMarkdownStore()
    },

    closeMarkdownStore: () => set({
      isMarkdownStoreOpen: false,
      markdownStoreEntries: [],
      markdownStoreLoading: false,
      markdownStoreError: '',
    }),

    refreshMarkdownStore: async () => {
      if (isCefContext()) {
        const requestKey = 'listMarkdownStoreEntries'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ directory?: string; entries?: MarkdownStoreEntry[] }>({
          action: 'listMarkdownStoreEntries',
          payload: { requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)
        if (!response.ok) {
          set({
            markdownStoreLoading: false,
            markdownStoreError: response.error ?? 'Failed to load Markdown Store.',
          })
          return false
        }
        set({
          markdownStoreDirectory: response.data?.directory ?? get().markdownStoreDirectory,
          markdownStoreEntries: response.data?.entries ?? [],
          markdownStoreLoading: false,
          markdownStoreError: '',
        })
        return true
      }

      set({ markdownStoreLoading: false, markdownStoreError: '' })
      return true
    },

    createMarkdownStoreEntry: async (draft: MarkdownStoreDraft) => {
      if (isCefContext()) {
        const response = await sendToCEF<MarkdownStoreEntry>({
          action: 'createMarkdownStoreEntry',
          payload: draft,
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to publish Markdown Store entry.' })
          return false
        }
        return get().refreshMarkdownStore()
      }

      const synthetic: MarkdownStoreEntry = {
        id: `${draft.title || Date.now()}.uam`,
        title: draft.title,
        maker: draft.maker,
        review: draft.review,
        dateCreated: new Date().toISOString(),
        dateUpdated: new Date().toISOString(),
        preview: draft.body,
        body: draft.body,
        favorite: false,
        sourceProvider: '',
        sourcePath: '',
        commandName: draft.title.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'skill',
        filePath: `${get().markdownStoreDirectory}/${draft.title || Date.now()}.uam`,
      }
      set((state) => ({ markdownStoreEntries: [...state.markdownStoreEntries, synthetic] }))
      return true
    },

    updateMarkdownStoreEntry: async (entry: MarkdownStoreEntry, draft: MarkdownStoreDraft) => {
      if (isCefContext()) {
        const response = await sendToCEF<MarkdownStoreEntry>({
          action: 'updateMarkdownStoreEntry',
          payload: { filePath: entry.filePath, ...draft },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to save Markdown Store entry.' })
          return false
        }
        return get().refreshMarkdownStore()
      }
      set((state) => ({
        markdownStoreEntries: state.markdownStoreEntries.map((candidate) => candidate.filePath === entry.filePath
          ? { ...candidate, ...draft, preview: draft.body, dateUpdated: new Date().toISOString() }
          : candidate),
      }))
      return true
    },

    setMarkdownStoreFavorite: async (entry: MarkdownStoreEntry, favorite: boolean) => {
      set((state) => ({
        markdownStoreEntries: state.markdownStoreEntries.map((candidate) => candidate.filePath === entry.filePath ? { ...candidate, favorite } : candidate),
      }))
      if (isCefContext()) {
        const response = await sendToCEF<MarkdownStoreEntry>({
          action: 'setMarkdownStoreFavorite',
          payload: { filePath: entry.filePath, favorite },
        })
        if (!response.ok) {
          set((state) => ({
            markdownStoreEntries: state.markdownStoreEntries.map((candidate) => candidate.filePath === entry.filePath ? { ...candidate, favorite: entry.favorite ?? false } : candidate),
            markdownStoreError: response.error ?? 'Failed to update Markdown Store favorite.',
          }))
          return false
        }
        if (response.data) set((state) => ({
          markdownStoreEntries: state.markdownStoreEntries.map((candidate) => candidate.filePath === entry.filePath ? response.data! : candidate),
          markdownStoreError: '',
        }))
      }
      return true
    },

    browseMarkdownStoreImport: async (kind: 'file' | 'folder') => {
      if (!isCefContext()) return null
      const response = await sendToCEF<{ selectedPath?: string }>({ action: 'browseMarkdownStoreImport', payload: { kind } })
      const selected = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      if (!response.ok) set({ markdownStoreError: response.error ?? 'Failed to choose import source.' })
      return selected || null
    },

    previewMarkdownStoreImports: async (options: { includeProviders?: boolean; paths?: string[] }) => {
      if (!isCefContext()) return [] as MarkdownStoreImportCandidate[]
      const response = await sendToCEF<{ candidates?: MarkdownStoreImportCandidate[] }>({ action: 'previewMarkdownStoreImports', payload: options })
      if (!response.ok) {
        set({ markdownStoreError: response.error ?? 'Failed to preview Markdown Store imports.' })
        return []
      }
      set({ markdownStoreError: '' })
      return response.data?.candidates ?? []
    },

    importMarkdownStoreEntries: async (imports: Array<{ sourceProvider: string; sourcePath: string; conflictAction: MarkdownStoreConflictAction }>) => {
      if (!isCefContext()) return [] as MarkdownStoreImportResult[]
      const response = await sendToCEF<{ results?: MarkdownStoreImportResult[] }>({ action: 'importMarkdownStoreEntries', payload: { imports } })
      if (!response.ok) {
        set({ markdownStoreError: response.error ?? 'Failed to import Markdown Store entries.' })
        return []
      }
      await get().refreshMarkdownStore()
      return response.data?.results ?? []
    },

    revealMarkdownStoreEntry: async (entry: MarkdownStoreEntry) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'revealMarkdownStoreEntry',
          payload: { filePath: entry.filePath },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to reveal Markdown Store entry.' })
          return false
        }
      }
      return true
    },

    editMarkdownStoreEntry: async (entry: MarkdownStoreEntry) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'editMarkdownStoreEntry',
          payload: { filePath: entry.filePath },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to open Markdown Store entry for editing.' })
          return false
        }
      }
      return true
    },

    attachMarkdownStoreEntry: (sessionId: string, entry: MarkdownStoreEntry) => set((state) => {
      const current = state.markdownStoreAttachedBySessionId[sessionId] ?? []
      if (current.some((candidate) => candidate.filePath === entry.filePath)) {
        return state
      }
      return {
        markdownStoreAttachedBySessionId: {
          ...state.markdownStoreAttachedBySessionId,
          [sessionId]: [...current, entry],
        },
      }
    }),

    detachMarkdownStoreEntry: (sessionId: string, filePath: string) => set((state) => ({
      markdownStoreAttachedBySessionId: {
        ...state.markdownStoreAttachedBySessionId,
        [sessionId]: (state.markdownStoreAttachedBySessionId[sessionId] ?? []).filter((entry) => entry.filePath !== filePath),
      },
    })),

    openAllMemoryLibrary: async () => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = 'listMemoryEntries:all'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'all', requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({
        memoryLibraryScope: {
          scopeType: 'all',
          folderId: '',
          label: 'All memory',
          rootPath: 'Global and project memory roots',
          rootCount: 0,
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    openGlobalMemoryLibrary: async () => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = 'listMemoryEntries:global'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'global', requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load global memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({
        memoryLibraryScope: {
          scopeType: 'global',
          folderId: '',
          label: 'Global memory',
          rootPath: '/tmp/uam-memory',
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    openFolderMemoryLibrary: async (folderId: string) => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = `listMemoryEntries:folder:${folderId}`
        const requestId = createRequestId('listMemoryEntries')
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'folder', folderId, requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load project memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      const folder = get().folders.find((candidate) => candidate.id === folderId)
      if (!folder) {
        set({
          memoryLibraryLoading: false,
          memoryLibraryError: `Folder not found: ${folderId}`,
        })
        return false
      }

      set({
        memoryLibraryScope: {
          scopeType: 'folder',
          folderId,
          label: folder.name,
          rootPath: `${folder.directory}/.UAM`,
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    closeMemoryLibrary: () => set({
      memoryLibraryScope: null,
      memoryLibraryEntries: [],
      memoryLibraryLoading: false,
      memoryLibraryError: '',
    }),

    refreshMemoryLibrary: async () => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = `listMemoryEntries:refresh:${scope.scopeType}:${scope.folderId ?? ''}`
        const requestId = createRequestId('listMemoryEntries')
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId, requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to refresh memory library.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({ memoryLibraryLoading: false })
      return true
    },

    createMemoryEntry: async (draft: MemoryEntryDraft) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const payload: Record<string, string> = {
          scopeType: scope.scopeType,
          folderId: scope.folderId,
          category: draft.category,
          title: draft.title,
          memory: draft.memory,
          evidence: draft.evidence,
          confidence: draft.confidence,
          sourceChatId: draft.sourceChatId,
        }

        if (scope.scopeType === 'all') {
          payload.targetScopeType = draft.targetScopeType ?? 'global'
          payload.targetFolderId = draft.targetFolderId ?? ''
        }

        const response = await sendToCEF({
          action: 'createMemoryEntry',
          payload,
        })

        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to create memory entry.' })
          return false
        }

        return get().refreshMemoryLibrary()
      }

      const syntheticEntry: MemoryEntry = {
        id: `memory-${Date.now()}.md`,
        title: draft.title,
        category: draft.category,
        scope: (scope.scopeType === 'global' || (scope.scopeType === 'all' && draft.targetScopeType === 'global')) ? 'global' : 'local',
        confidence: draft.confidence,
        sourceChatId: draft.sourceChatId,
        lastObserved: new Date().toISOString(),
        occurrenceCount: 1,
        preview: draft.memory,
        filePath: `${scope.rootPath}/${draft.category}/${draft.title}.md`,
        scopeType: scope.scopeType === 'all' ? (draft.targetScopeType ?? 'global') : (scope.scopeType === 'global' ? 'global' : 'folder'),
        folderId: scope.scopeType === 'all' ? draft.targetFolderId : scope.folderId,
        scopeLabel: scope.scopeType === 'all' && draft.targetScopeType === 'folder'
          ? (get().folders.find((folder) => folder.id === draft.targetFolderId)?.name ?? 'Project memory')
          : scope.label,
        rootPath: scope.rootPath,
      }
      set((state) => ({
        memoryLibraryEntries: [...state.memoryLibraryEntries, syntheticEntry],
        memoryLibraryError: '',
      }))
      return true
    },

    deleteMemoryEntry: async (entryId: string) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'deleteMemoryEntry',
          payload: {
            scopeType: scope.scopeType,
            folderId: scope.folderId,
            entryId,
          },
        })

        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to delete memory entry.' })
          return false
        }

        return get().refreshMemoryLibrary()
      }

      set((state) => ({
        memoryLibraryEntries: state.memoryLibraryEntries.filter((entry) => entry.id !== entryId),
      }))
      return true
    },

    deleteMemoryEntries: async (entryIds: string[]) => {
      const scope = get().memoryLibraryScope
      const uniqueEntryIds = Array.from(new Set(entryIds.filter((entryId) => entryId.trim().length > 0)))
      if (!scope || uniqueEntryIds.length === 0) {
        return false
      }

      if (isCefContext()) {
        for (const entryId of uniqueEntryIds) {
          const response = await sendToCEF({
            action: 'deleteMemoryEntry',
            payload: {
              scopeType: scope.scopeType,
              folderId: scope.folderId,
              entryId,
            },
          })

          if (!response.ok) {
            set({ memoryLibraryError: response.error ?? 'Failed to delete memory entries.' })
            await get().refreshMemoryLibrary()
            return false
          }
        }

        return get().refreshMemoryLibrary()
      }

      set((state) => ({
        memoryLibraryEntries: state.memoryLibraryEntries.filter((entry) => !uniqueEntryIds.includes(entry.id)),
        memoryLibraryError: '',
      }))
      return true
    },

    openMemoryRoot: async () => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'openMemoryRoot',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId },
        })
        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to open memory root.' })
          return false
        }
      }

      return true
    },

    revealMemoryEntry: async (entryId: string) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'revealMemoryEntry',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId, entryId },
        })
        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to reveal memory file.' })
          return false
        }
      }

      return true
    },

    openMemoryScanModal: async () => {
      set({
        isMemoryScanModalOpen: true,
        memoryScanLoading: true,
        memoryScanError: '',
        memoryScanCandidates: [],
        selectedMemoryScanChatIds: [],
      })

      if (isCefContext()) {
        const response = await sendToCEF<{ candidates?: MemoryScanCandidate[] }>({
          action: 'listMemoryScanCandidates',
        })
        if (!response.ok) {
          set({
            memoryScanLoading: false,
            memoryScanError: response.error ?? 'Failed to load chats for memory scan.',
          })
          return false
        }

        const candidates = response.data?.candidates ?? []
        set({
          memoryScanCandidates: candidates,
          selectedMemoryScanChatIds: candidates.map((candidate) => candidate.chatId),
          memoryScanLoading: false,
          memoryScanError: '',
        })
        return true
      }

      const sessions = get().sessions
        .filter((session) => (session.memoryLevel ?? ((session.memoryEnabled ?? true) ? 'strict' : 'off')) !== 'off' && (get().messages[session.id]?.length ?? 0) > 0)
        .map((session) => ({
          chatId: session.id,
          title: session.name,
          folderId: session.folderId ?? '',
          folderTitle: session.folderId ? (get().folders.find((folder) => folder.id === session.folderId)?.name ?? '') : '',
          providerId: session.providerId ?? GEMINI_CLI_PROVIDER_ID,
          messageCount: get().messages[session.id]?.length ?? 0,
          memoryEnabled: session.memoryEnabled ?? true,
          memoryLevel: session.memoryLevel ?? ((session.memoryEnabled ?? true) ? 'strict' : 'off'),
          memoryLastProcessedAt: session.memoryLastProcessedAt ?? '',
          alreadyFullyProcessed: false,
        }))
      set({
        memoryScanCandidates: sessions,
        selectedMemoryScanChatIds: sessions.map((candidate) => candidate.chatId),
        memoryScanLoading: false,
        memoryScanError: '',
      })
      return true
    },

    closeMemoryScanModal: () => set({
      isMemoryScanModalOpen: false,
      memoryScanCandidates: [],
      selectedMemoryScanChatIds: [],
      memoryScanLoading: false,
      memoryScanRunning: false,
      memoryScanError: '',
    }),

    toggleMemoryScanChat: (chatId: string) => set((state) => ({
      selectedMemoryScanChatIds: state.selectedMemoryScanChatIds.includes(chatId)
        ? state.selectedMemoryScanChatIds.filter((id) => id !== chatId)
        : [...state.selectedMemoryScanChatIds, chatId],
    })),

    selectAllMemoryScanChats: () => set((state) => ({
      selectedMemoryScanChatIds: state.memoryScanCandidates.map((candidate) => candidate.chatId),
    })),

    selectNoMemoryScanChats: () => set({ selectedMemoryScanChatIds: [] }),

    startMemoryScan: async () => {
      const selectedChatIds = get().selectedMemoryScanChatIds
      if (selectedChatIds.length === 0) {
        set({ memoryScanError: 'Select at least one chat to scan.' })
        return false
      }

      set({ memoryScanRunning: true, memoryScanError: '' })

      if (isCefContext()) {
        const response = await sendToCEF<{ queuedCount?: number }>({
          action: 'scanCurrentChats',
          payload: { chatIds: selectedChatIds },
        })

        if (!response.ok) {
          set({
            memoryScanRunning: false,
            memoryScanError: response.error ?? 'Failed to queue memory scan.',
          })
          return false
        }

        set({
          isMemoryScanModalOpen: false,
          memoryScanRunning: false,
          memoryScanCandidates: [],
          selectedMemoryScanChatIds: [],
          memoryScanError: '',
        })
        return true
      }

      set({
        isMemoryScanModalOpen: false,
        memoryScanRunning: false,
        memoryScanCandidates: [],
        selectedMemoryScanChatIds: [],
        memoryScanError: '',
      })
      return true
    },
  }
}
