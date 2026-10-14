import { describe, expect, it } from 'vitest'
import { sanitizeAcpModel, sanitizeCppChat } from './sanitizers'
import { acpBindingFromCppChat } from './reconcile'

function metadataChat(contextWindowTokens: unknown, maxOutputTokens: unknown)
{
	const chat = sanitizeCppChat({
		id: 'metadata-fixture', title: 'Metadata', folderId: '', providerId: 'codex-cli',
		createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:00.000Z',
		acpSession: { availableModels: [{ id: 'fixture-model', contextWindowTokens, maxOutputTokens }] },
	})
	if (!chat) throw new Error('Private metadata chat fixture was rejected')
	return chat
}

describe('provider model metadata transport', () =>
{
	it('retains exact nonnegative token capacities including explicit zero', () =>
	{
		expect(sanitizeAcpModel({ id: 'fixture-model', contextWindowTokens: Number.MAX_SAFE_INTEGER, maxOutputTokens: 0 })).toMatchObject({
			contextWindowTokens: Number.MAX_SAFE_INTEGER, maxOutputTokens: 0,
		})
	})

	it('keeps missing and invalid capacities unknown without dropping the model', () =>
	{
		for (const invalid of [undefined, null, -1, 1.5, '200000', true, Number.MAX_SAFE_INTEGER + 1, Infinity, NaN])
		{
			expect(sanitizeAcpModel({ id: 'fixture-model', contextWindowTokens: invalid, maxOutputTokens: invalid })).toMatchObject({
				id: 'fixture-model', contextWindowTokens: null, maxOutputTokens: null,
			})
		}
	})

	it('publishes capacity-only binding refreshes and preserves unchanged bindings', () =>
	{
		const initial = acpBindingFromCppChat(metadataChat(200000, 8192), undefined)
		expect(acpBindingFromCppChat(metadataChat(200000, 8192), initial)).toBe(initial)
		const contextChanged = acpBindingFromCppChat(metadataChat(1000000, 8192), initial)
		expect(contextChanged).not.toBe(initial)
		expect(contextChanged.availableModels[0].contextWindowTokens).toBe(1000000)
		const outputChanged = acpBindingFromCppChat(metadataChat(1000000, 64000), contextChanged)
		expect(outputChanged).not.toBe(contextChanged)
		expect(outputChanged.availableModels[0].maxOutputTokens).toBe(64000)
		const cleared = acpBindingFromCppChat(metadataChat(null, null), outputChanged)
		expect(cleared).not.toBe(outputChanged)
		expect(cleared.availableModels[0]).toMatchObject({ contextWindowTokens: null, maxOutputTokens: null })
		expect(acpBindingFromCppChat(metadataChat(undefined, undefined), cleared)).toBe(cleared)
	})
})
