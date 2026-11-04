import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { Session } from '../../types/session'
import type { AcpBinding } from './types'
import { useAppStore } from '../useAppStore'

/**
 * Model discovery projection follows the requesting chat and native binding.
 * Cached models are fixture data; started/pending denotes discovery admission.
 * Deferred CEF callbacks exercise the real store action without native processes.
 */

type CefQuery = NonNullable<Window['cefQuery']>
type QueryParameters = Parameters<CefQuery>[0]
type DeferredQuery = {
	envelope: { action: string; payload?: Record<string, unknown>; requestId?: string }
	settled: boolean
	pending: () => void
	failure: () => void
	close: () => void
}

const chatId = 'private-model-discovery-owner'
const siblingId = 'private-model-discovery-sibling'
const originalBirth = '2026-10-22T01:00:00.000Z'
const replacementBirth = '2026-10-22T02:00:00.000Z'
const catalogUnavailable = 'Provider model catalog is unavailable.'
const initialStore = useAppStore.getState()
const previousCefQuery = Object.getOwnPropertyDescriptor(window, 'cefQuery')
let requests: DeferredQuery[] = []
let inFlight: Array<Promise<boolean>> = []

function session(birth = originalBirth, host = 'local', id = chatId): Session
{
	const createdAt = new Date(birth)
	expect(Number.isFinite(createdAt.getTime())).toBe(true)
	return {
		id, name: 'Private model discovery fixture', providerId: 'opencode-cli',
		viewMode: 'chat', folderId: null, executionHostId: host,
		workspaceDirectory: host === 'local' ? '/private/model-discovery-fixture' : 'C:/private/model-discovery-fixture',
		createdAt, updatedAt: new Date(birth), isPinned: false,
	}
}

function binding(nativeSessionId = 'original-private-native', modelId = 'cached-original-model'): AcpBinding
{
	return {
		sessionId: nativeSessionId, threadId: nativeSessionId, providerId: 'opencode-cli',
		protocolKind: 'acp', running: false, lifecycleState: 'stopped', processing: false,
		readySinceLastSelect: false, processingStartedAtMs: null,
		lastError: '', recentStderr: '', lastExitCode: null, diagnostics: [], toolCalls: [],
		planEntries: [], availableModes: [], currentModeId: '',
		availableModels: [{ id: modelId, name: 'Private cached model', description: '' }],
		currentModelId: modelId, modelsLoading: false, modelRefreshError: 'Existing cached model notice.',
		turnEvents: [], turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnSerial: 0,
		pendingPermission: null, pendingUserInput: null, agentInfo: null,
		queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
	}
}

beforeEach(() =>
{
	requests = []
	inFlight = []
	useAppStore.setState(initialStore, true)
	useAppStore.setState({
		sessions: [session(), session(originalBirth, 'local', siblingId)],
		activeSessionId: chatId,
		acpBindingBySessionId: {
			[chatId]: binding(),
			[siblingId]: binding('unrelated-native', 'cached-sibling-model'),
		},
		messages: { [chatId]: [], [siblingId]: [] },
		statusLine: 'Unrelated shared state must be conserved.',
	})
	vi.spyOn(console, 'error')
	const cefQuery: CefQuery = (parameters: QueryParameters) =>
	{
		const query: DeferredQuery = {
			envelope: JSON.parse(parameters.request), settled: false,
			pending: () =>
			{
				expect(query.settled).toBe(false)
				query.settled = true
				parameters.onSuccess(JSON.stringify({ started: true, pending: true }))
			},
			failure: () =>
			{
				expect(query.settled).toBe(false)
				query.settled = true
				parameters.onFailure(500, catalogUnavailable)
			},
			close: () =>
			{
				if (query.settled) return
				query.settled = true
				parameters.onSuccess(JSON.stringify({ started: false, pending: false }))
			},
		}
		requests.push(query)
	}
	Object.defineProperty(window, 'cefQuery', { configurable: true, writable: true, value: cefQuery })
})

afterEach(async () =>
{
	// Settle every held callback even if an earlier prerequisite assertion failed.
	for (const query of requests) query.close()
	await Promise.allSettled(inFlight)
	if (previousCefQuery) Object.defineProperty(window, 'cefQuery', previousCefQuery)
	else delete window.cefQuery
	useAppStore.setState(initialStore, true)
	vi.restoreAllMocks()
})

function beginDiscovery()
{
	const result = useAppStore.getState().discoverProviderModels(chatId)
	inFlight.push(result)
	expect(requests).toHaveLength(1)
	expect(requests[0].settled).toBe(false)
	expect(requests[0].envelope).toMatchObject({ action: 'discoverProviderModels', payload: { chatId } })
	expect(typeof requests[0].envelope.requestId).toBe('string')
	// Nonvacuous proof that the actual public action ran before the callback is released.
	expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe('')
	return result
}

function replaceOwner(birth: string, host = 'local')
{
	const replacement = session(birth, host)
	const replacementBinding = {
		...binding('replacement-private-native', 'cached-replacement-model'),
		modelRefreshError: 'Replacement-owned notice must remain unchanged.',
	}
	useAppStore.setState((state) => ({
		sessions: state.sessions.map((entry) => entry.id === chatId ? replacement : entry),
		acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: replacementBinding },
	}))
	return useAppStore.getState()
}

function expectNoReplay()
{
	expect(requests.map((query) => query.envelope.action)).toEqual(['discoverProviderModels'])
	expect(requests.every((query) => query.settled)).toBe(true)
}

describe('model discovery callback owner lifetime through the real public store', () =>
{
	it('keeps a same-ID different-birth replacement unchanged after the old failure', async () =>
	{
		const result = beginDiscovery()
		const replacement = replaceOwner(replacementBirth)
		expect(replacement.sessions.find((entry) => entry.id === chatId)?.createdAt.getTime()).not.toBe(new Date(originalBirth).getTime())
		requests[0].failure()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe(replacement.acpBindingBySessionId[chatId].modelRefreshError)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(replacement.acpBindingBySessionId[chatId].modelsLoading)
		expect(useAppStore.getState()).toBe(replacement)
		expect(useAppStore.getState().acpBindingBySessionId).toBe(replacement.acpBindingBySessionId)
		expectNoReplay()
	})

	it('keeps a same-ID different-birth replacement unchanged after old pending acceptance', async () =>
	{
		const result = beginDiscovery()
		const replacement = replaceOwner(replacementBirth)
		expect(replacement.acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		requests[0].pending()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe(replacement.acpBindingBySessionId[chatId].modelRefreshError)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(replacement.acpBindingBySessionId[chatId].modelsLoading)
		expect(useAppStore.getState()).toBe(replacement)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		expectNoReplay()
	})

	it('retains a real same-owner failure and its error without changing cached models', async () =>
	{
		useAppStore.setState((state) => ({ acpBindingBySessionId: {
			...state.acpBindingBySessionId, [chatId]: { ...state.acpBindingBySessionId[chatId], modelsLoading: true },
		} }))
		const result = beginDiscovery()
		const before = useAppStore.getState()
		requests[0].failure()
		await expect(result).resolves.toBe(false)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toMatchObject({ modelsLoading: false, modelRefreshError: catalogUnavailable, running: false, processing: false })
		expect(after.acpBindingBySessionId[chatId].availableModels).toBe(before.acpBindingBySessionId[chatId].availableModels)
		expect(after.acpBindingBySessionId[chatId].currentModelId).toBe('cached-original-model')
		expect(after.sessions).toBe(before.sessions)
		expect(after.messages).toBe(before.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(before.acpBindingBySessionId[siblingId])
		expectNoReplay()
	})

	it('marks only the same-owner selector loading for real pending acceptance', async () =>
	{
		const result = beginDiscovery()
		const before = useAppStore.getState()
		requests[0].pending()
		await expect(result).resolves.toBe(true)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toMatchObject({ modelsLoading: true, modelRefreshError: '', running: false, processing: false })
		expect(after.acpBindingBySessionId[chatId].availableModels).toBe(before.acpBindingBySessionId[chatId].availableModels)
		expect(after.acpBindingBySessionId[chatId].currentModelId).toBe('cached-original-model')
		expect(after.sessions).toBe(before.sessions)
		expect(after.messages).toBe(before.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(before.acpBindingBySessionId[siblingId])
		expectNoReplay()
	})

	it('does not recreate a deleted owner binding when its old failure arrives', async () =>
	{
		const result = beginDiscovery()
		useAppStore.setState((state) => ({
			sessions: state.sessions.filter((entry) => entry.id !== chatId), activeSessionId: siblingId,
			acpBindingBySessionId: { [siblingId]: state.acpBindingBySessionId[siblingId] },
			messages: { [siblingId]: state.messages[siblingId] },
		}))
		const deleted = useAppStore.getState()
		requests[0].failure()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expect(useAppStore.getState()).toBe(deleted)
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expectNoReplay()
	})

	it('keeps a changed execution host and native binding unchanged after old pending acceptance', async () =>
	{
		const result = beginDiscovery()
		const replacement = replaceOwner(originalBirth, 'private-ssh-fixture-not-contacted')
		expect(replacement.sessions.find((entry) => entry.id === chatId)?.executionHostId).not.toBe('local')
		requests[0].pending()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe(replacement.acpBindingBySessionId[chatId].modelRefreshError)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(replacement.acpBindingBySessionId[chatId].modelsLoading)
		expect(useAppStore.getState()).toBe(replacement)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].sessionId).toBe('replacement-private-native')
		expectNoReplay()
	})
})


/** Identityless and binding-only discovery retain their exact prepared selector. */
function omitOwnerBinding()
{
	useAppStore.setState((state) =>
	{
		const bindings = { ...state.acpBindingBySessionId }
		delete bindings[chatId]
		return { acpBindingBySessionId: bindings }
	})
	expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
}

function expectIdentitylessClearedBinding()
{
	const cleared = useAppStore.getState().acpBindingBySessionId[chatId]
	expect(cleared).toBeDefined()
	expect(cleared.modelRefreshError).toBe('')
	// This is the partial object created by the actual discovery action, not a
	// fabricated native/session identity or a synthetic provider acknowledgement.
	expect(cleared.sessionId).toBeUndefined()
	expect(cleared.threadId).toBeUndefined()
	return cleared
}

function deleteOwnerWhileDiscoveryWaits()
{
	useAppStore.setState((state) =>
	{
		const bindings = { ...state.acpBindingBySessionId }
		const messages = { ...state.messages }
		delete bindings[chatId]
		delete messages[chatId]
		return {
			sessions: state.sessions.filter((entry) => entry.id !== chatId),
			activeSessionId: siblingId, acpBindingBySessionId: bindings, messages,
		}
	})
	return useAppStore.getState()
}

function replaceIdentitylessBinding()
{
	const cleared = expectIdentitylessClearedBinding()
	const replacement = {
		...cleared, modelRefreshError: 'A new identityless owner notice.',
		modelsLoading: false, recentStderr: 'Replacement-owned observation.',
	}
	useAppStore.setState((state) => ({
		acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: replacement },
	}))
	expect(replacement).not.toBe(cleared)
	expect(replacement.sessionId).toBeUndefined()
	expect(replacement.threadId).toBeUndefined()
	return useAppStore.getState()
}

function removeOwnerSessionOnly()
{
	useAppStore.setState((state) => ({
		sessions: state.sessions.filter((entry) => entry.id !== chatId), activeSessionId: siblingId,
	}))
	expect(useAppStore.getState().sessions.some((entry) => entry.id === chatId)).toBe(false)
	expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeDefined()
}

describe('additive model discovery partial-binding and public compatibility controls', () =>
{
	it('projects an error for a retained valid session whose ACP binding was initially absent', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		expectIdentitylessClearedBinding()
		const before = useAppStore.getState()
		requests[0].failure()
		await expect(result).resolves.toBe(false)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toEqual({
			...before.acpBindingBySessionId[chatId], modelsLoading: false, modelRefreshError: catalogUnavailable,
		})
		expect(after.sessions).toBe(before.sessions)
		expect(after.messages).toBe(before.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(before.acpBindingBySessionId[siblingId])
		expect(after.acpBindingBySessionId[chatId].sessionId).toBeUndefined()
		expect(after.acpBindingBySessionId[chatId].threadId).toBeUndefined()
		expectNoReplay()
	})

	it('projects pending discovery for a retained valid session whose ACP binding was initially absent', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		expectIdentitylessClearedBinding()
		const before = useAppStore.getState()
		requests[0].pending()
		await expect(result).resolves.toBe(true)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toEqual({ ...before.acpBindingBySessionId[chatId], modelsLoading: true })
		expect(after.sessions).toBe(before.sessions)
		expect(after.messages).toBe(before.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(before.acpBindingBySessionId[siblingId])
		expect(after.acpBindingBySessionId[chatId].sessionId).toBeUndefined()
		expect(after.acpBindingBySessionId[chatId].threadId).toBeUndefined()
		expectNoReplay()
	})

	it('does not recreate an initially absent ACP binding after its valid session is deleted and old failure arrives', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		expectIdentitylessClearedBinding()
		const deleted = deleteOwnerWhileDiscoveryWaits()
		requests[0].failure()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expect(useAppStore.getState()).toBe(deleted)
		expectNoReplay()
	})

	it('does not recreate an initially absent ACP binding after its valid session is deleted and old pending reply arrives', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		expectIdentitylessClearedBinding()
		const deleted = deleteOwnerWhileDiscoveryWaits()
		requests[0].pending()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expect(useAppStore.getState()).toBe(deleted)
		expectNoReplay()
	})

	it('preserves a same-session replacement identityless partial binding after the old error', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		const replacement = replaceIdentitylessBinding()
		requests[0].failure()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe('A new identityless owner notice.')
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		expect(useAppStore.getState()).toBe(replacement)
		expectNoReplay()
	})

	it('preserves a same-session replacement identityless partial binding after the old pending reply', async () =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		const replacement = replaceIdentitylessBinding()
		requests[0].pending()
		await result
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe('A new identityless owner notice.')
		expect(useAppStore.getState()).toBe(replacement)
		expectNoReplay()
	})

	it('accepts pending model projection after a harmless same-owner known-native binding clone', async () =>
	{
		const result = beginDiscovery()
		useAppStore.setState((state) => ({ acpBindingBySessionId: {
			...state.acpBindingBySessionId,
			[chatId]: { ...state.acpBindingBySessionId[chatId], recentStderr: 'New same-owner background observation.' },
		} }))
		const before = useAppStore.getState()
		expect(before.acpBindingBySessionId[chatId].sessionId).toBe('original-private-native')
		expect(before.acpBindingBySessionId[chatId].threadId).toBe('original-private-native')
		requests[0].pending()
		await expect(result).resolves.toBe(true)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toEqual({ ...before.acpBindingBySessionId[chatId], modelsLoading: true })
		expect(after.acpBindingBySessionId[chatId].availableModels).toBe(before.acpBindingBySessionId[chatId].availableModels)
		expect(after.sessions).toBe(before.sessions)
		expect(after.messages).toBe(before.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(before.acpBindingBySessionId[siblingId])
		expectNoReplay()
	})

	it('retains binding-only public request compatibility but does not recreate its deleted binding after an old error', async () =>
	{
		removeOwnerSessionOnly()
		const result = beginDiscovery()
		const deleted = deleteOwnerWhileDiscoveryWaits()
		requests[0].failure()
		await expect(result).resolves.toBe(false)
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expect(useAppStore.getState()).toBe(deleted)
		expectNoReplay()
	})

	it('retains binding-only public pending compatibility without projecting into a replaced binding', async () =>
	{
		removeOwnerSessionOnly()
		const result = beginDiscovery()
		const replacement = {
			...useAppStore.getState().acpBindingBySessionId[chatId],
			modelRefreshError: 'New binding-only owner notice.', modelsLoading: false,
		}
		useAppStore.setState((state) => ({
			acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: replacement },
		}))
		const before = useAppStore.getState()
		requests[0].pending()
		// The raw transport boolean remains true; no Session-dependent early refusal.
		await expect(result).resolves.toBe(true)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		expect(useAppStore.getState().acpBindingBySessionId[chatId].modelRefreshError).toBe('New binding-only owner notice.')
		expect(useAppStore.getState()).toBe(before)
		expectNoReplay()
	})
})


// Keep the same valid Session while only its prepared partial binding disappears.
describe('model discovery identityless binding disappearance with retained Session', () =>
{
	it.each(['failure', 'pending'] as const)('does not recreate the partial binding after %s when the valid Session remains', async (reply) =>
	{
		omitOwnerBinding()
		const result = beginDiscovery()
		expectIdentitylessClearedBinding()
		const sameSessions = useAppStore.getState().sessions
		const sameSession = sameSessions.find((entry) => entry.id === chatId)
		expect(sameSession?.createdAt.getTime()).toBe(new Date(originalBirth).getTime())
		omitOwnerBinding()
		const before = useAppStore.getState()
		expect(before.sessions).toBe(sameSessions)
		expect(before.sessions.find((entry) => entry.id === chatId)).toBe(sameSession)
		requests[0][reply]()
		await expect(result).resolves.toBe(reply === 'pending')
		expect(useAppStore.getState().acpBindingBySessionId[chatId]).toBeUndefined()
		expect(useAppStore.getState()).toBe(before)
		expectNoReplay()
	})
})


/** Each isolated tuple change retires the previous selector projection. */
function seedIsolatedTupleOwner()
{
	const folderDirectory = '/private/isolated-model-discovery-folder'
	useAppStore.setState((state) => ({
		sessions: state.sessions.map((entry) => entry.id === chatId
			? { ...entry, folderId: 'isolated-model-folder-A', workspaceWorktreeDirectory: '/private/isolated-model-discovery-worktree' }
			: entry),
		// Both valid folders deliberately resolve the SAME directory: changing folderId alone
		// must exercise its own fence, not accidentally pass because the directory also changed.
		folders: ['isolated-model-folder-A', 'isolated-model-folder-B'].map((id) => ({
			id, name: id, parentId: null, directory: folderDirectory,
			isExpanded: false, createdAt: new Date(originalBirth),
		})),
	}))
}

function isolatedOwnerTuple(state: ReturnType<typeof useAppStore.getState>)
{
	const owner = state.sessions.find((entry) => entry.id === chatId)!
	const native = state.acpBindingBySessionId[chatId]
	return {
		birth: owner.createdAt.getTime(), provider: owner.providerId,
		host: owner.executionHostId, workspace: owner.workspaceDirectory,
		worktree: owner.workspaceWorktreeDirectory, folderId: owner.folderId,
		folderDirectory: state.folders.find((entry) => entry.id === owner.folderId)?.directory,
		nativeSessionId: native.sessionId, nativeThreadId: native.threadId,
		nativeProviderId: native.providerId, nativeProtocol: native.protocolKind,
	}
}

const isolatedOwnershipDimensions = [
	{ name: 'session provider', target: 'session', key: 'providerId', value: 'codex-cli', tuple: 'provider' },
	{ name: 'execution host', target: 'session', key: 'executionHostId', value: 'isolated-ssh-not-contacted', tuple: 'host' },
	{ name: 'workspace', target: 'session', key: 'workspaceDirectory', value: '/private/changed-model-workspace', tuple: 'workspace' },
	{ name: 'worktree', target: 'session', key: 'workspaceWorktreeDirectory', value: '/private/changed-model-worktree', tuple: 'worktree' },
	{ name: 'folder ID only', target: 'session', key: 'folderId', value: 'isolated-model-folder-B', tuple: 'folderId' },
	{ name: 'folder directory only', target: 'folder', key: 'directory', value: '/private/changed-model-folder', tuple: 'folderDirectory' },
	{ name: 'native session ID only', target: 'native', key: 'sessionId', value: 'changed-private-native-session', tuple: 'nativeSessionId' },
	{ name: 'native thread ID only', target: 'native', key: 'threadId', value: 'changed-private-native-thread', tuple: 'nativeThreadId' },
	{ name: 'native provider only', target: 'native', key: 'providerId', value: 'codex-cli', tuple: 'nativeProviderId' },
	{ name: 'native protocol only', target: 'native', key: 'protocolKind', value: 'codex-app-server', tuple: 'nativeProtocol' },
] as const

const isolatedOwnershipReplies = isolatedOwnershipDimensions.flatMap((dimension) =>
	(['failure', 'pending'] as const).map((reply) => ({ ...dimension, reply })))

describe('isolated model-discovery selector ownership dimensions', () =>
{
	it.each(isolatedOwnershipReplies)('preserves the selector after only $name changes before $reply', async (dimension) =>
	{
		seedIsolatedTupleOwner()
		const result = beginDiscovery()
		const captured = useAppStore.getState()
		const capturedTuple = isolatedOwnerTuple(captured)
		expect(capturedTuple.birth).toBe(new Date(originalBirth).getTime())
		expect(capturedTuple.nativeSessionId).toBe('original-private-native')
		expect(capturedTuple.nativeThreadId).toBe('original-private-native')
		expect(capturedTuple.folderDirectory).toBe('/private/isolated-model-discovery-folder')
		expect(captured.acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		if (dimension.target === 'session')
		{
			useAppStore.setState((state) => ({ sessions: state.sessions.map((entry) => entry.id === chatId
				? { ...entry, [dimension.key]: dimension.value } : entry) }))
		}
		else if (dimension.target === 'folder')
		{
			useAppStore.setState((state) => ({ folders: state.folders.map((entry) => entry.id === 'isolated-model-folder-A'
				? { ...entry, directory: dimension.value } : entry) }))
		}
		else
		{
			useAppStore.setState((state) => ({ acpBindingBySessionId: {
				...state.acpBindingBySessionId,
				[chatId]: { ...state.acpBindingBySessionId[chatId], [dimension.key]: dimension.value },
			} }))
		}
		const beforeReply = useAppStore.getState()
		const changedTuple = isolatedOwnerTuple(beforeReply)
		const changedKeys = (Object.keys(capturedTuple) as Array<keyof typeof capturedTuple>)
			.filter((key) => changedTuple[key] !== capturedTuple[key])
		expect(changedKeys).toEqual([dimension.tuple])
		expect(beforeReply.messages).toBe(captured.messages)
		expect(beforeReply.acpBindingBySessionId[siblingId]).toBe(captured.acpBindingBySessionId[siblingId])
		if (dimension.target !== 'native') expect(beforeReply.acpBindingBySessionId).toBe(captured.acpBindingBySessionId)
		if (dimension.target !== 'session') expect(beforeReply.sessions).toBe(captured.sessions)
		if (dimension.target !== 'folder') expect(beforeReply.folders).toBe(captured.folders)
		requests[0][dimension.reply]()
		await expect(result).resolves.toBe(dimension.reply === 'pending')
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId].modelRefreshError).toBe(beforeReply.acpBindingBySessionId[chatId].modelRefreshError)
		expect(after.acpBindingBySessionId[chatId].modelsLoading).toBe(false)
		expect(after.acpBindingBySessionId[chatId]).toBe(beforeReply.acpBindingBySessionId[chatId])
		expect(after).toBe(beforeReply)
		expectNoReplay()
	})

	it('projects a same-owner failure after a harmless known-native binding clone', async () =>
	{
		const result = beginDiscovery()
		const prepared = useAppStore.getState()
		const cloned = { ...prepared.acpBindingBySessionId[chatId], recentStderr: 'Later same-owner observation.', modelsLoading: true }
		useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: cloned } }))
		const beforeReply = useAppStore.getState()
		expect(cloned).not.toBe(prepared.acpBindingBySessionId[chatId])
		expect(isolatedOwnerTuple(beforeReply)).toEqual(isolatedOwnerTuple(prepared))
		requests[0].failure()
		await expect(result).resolves.toBe(false)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toEqual({ ...cloned, modelsLoading: false, modelRefreshError: catalogUnavailable })
		expect(after.acpBindingBySessionId[chatId].availableModels).toBe(cloned.availableModels)
		expect(after.sessions).toBe(beforeReply.sessions)
		expect(after.messages).toBe(beforeReply.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(beforeReply.acpBindingBySessionId[siblingId])
		expectNoReplay()
	})

	it('projects a same-prepared-binding public failure without a Session', async () =>
	{
		removeOwnerSessionOnly()
		const result = beginDiscovery()
		const beforeReply = useAppStore.getState()
		expect(beforeReply.sessions.some((entry) => entry.id === chatId)).toBe(false)
		expect(beforeReply.acpBindingBySessionId[chatId].sessionId).toBe('original-private-native')
		requests[0].failure()
		await expect(result).resolves.toBe(false)
		const after = useAppStore.getState()
		expect(after.acpBindingBySessionId[chatId]).toEqual({
			...beforeReply.acpBindingBySessionId[chatId], modelsLoading: false, modelRefreshError: catalogUnavailable,
		})
		expect(after.acpBindingBySessionId[chatId].availableModels).toBe(beforeReply.acpBindingBySessionId[chatId].availableModels)
		expect(after.sessions).toBe(beforeReply.sessions)
		expect(after.messages).toBe(beforeReply.messages)
		expect(after.acpBindingBySessionId[siblingId]).toBe(beforeReply.acpBindingBySessionId[siblingId])
		expectNoReplay()
	})
})
