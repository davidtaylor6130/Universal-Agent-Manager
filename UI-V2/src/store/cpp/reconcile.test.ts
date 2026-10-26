import { describe, expect, it } from 'vitest'
import type { CppChat, CppMessage } from './types'
import { reconcileHeldOutboxHistory, messageBlocksEquivalent, sessionFromCppChat, acpBindingFromCppChat, cliBindingFromCppChat, reconcileCppMessages, normalizeCliLifecycleState, cliLifecycleIsProcessing } from './reconcile'
import { sanitizeCppAppState, sanitizeCppChat, sanitizeCppAcpSession, sanitizeCppGoal, sanitizeCppMessage, sanitizeCppProvider, sanitizeCppSettings } from './sanitizers'

describe('Computer Use settings sanitization', () => {
  it('canonicalizes backend identity kinds during persisted settings roundtrip', () => {
    expect(sanitizeCppSettings({
      computerUseAllowlistEnabled: true,
      computerUseAllowedApplications: [
        { identityKind: 'bundleid', identity: 'com.apple.TextEdit' },
        { identityKind: 'executablepath', identity: '/Applications/Preview.app' },
      ],
    }).computerUseAllowedApplications).toEqual([
      { identityKind: 'bundleId', identity: 'com.apple.TextEdit' },
      { identityKind: 'executablePath', identity: '/Applications/Preview.app' },
    ])
  })
})

describe('computerUseGloballyEnabled sanitization', () => {
  it('keeps only a real boolean and otherwise leaves the global consent absent', () => {
    expect(sanitizeCppSettings({ computerUseGloballyEnabled: true }).computerUseGloballyEnabled).toBe(true)
    expect(sanitizeCppSettings({ computerUseGloballyEnabled: 'yes' }).computerUseGloballyEnabled).toBeUndefined()
    expect(sanitizeCppSettings({}).computerUseGloballyEnabled).toBeUndefined()
  })
  it('keeps unknown cleanup distinct from an explicit cleared state', () => {
    expect(sanitizeCppSettings({ computerUseRevocationPending: true }).computerUseRevocationPending).toBe(true)
    expect(sanitizeCppSettings({ computerUseRevocationPending: false }).computerUseRevocationPending).toBe(false)
    expect(sanitizeCppSettings({ computerUseRevocationPending: 'false' }).computerUseRevocationPending).toBeUndefined()
    expect(sanitizeCppSettings({}).computerUseRevocationPending).toBeUndefined()
  })
})

describe('hostScopedSettings capability sanitization', () => {
  it('keeps only a real boolean and otherwise leaves the capability absent', () => {
    expect(sanitizeCppSettings({ hostScopedSettings: true }).hostScopedSettings).toBe(true)
    expect(sanitizeCppSettings({ hostScopedSettings: 'yes' }).hostScopedSettings).toBeUndefined()
    expect(sanitizeCppSettings({}).hostScopedSettings).toBeUndefined()
  })
})

const message: CppMessage = {
  role: 'user',
  content: 'Use the attached file.',
  createdAt: '2026-01-01T00:00:00.000Z',
}

describe('reconcileCppMessages attachments', () => {
  it('adopts a persisted unsent discard without changing its native position or delivery flag', () => {
    const original = sanitizeCppMessage({ ...message, acpPromptNotSent: true })!
    const before = reconcileCppMessages('chat', undefined, [original], true, 7)
    const discarded = sanitizeCppMessage({ ...original, acpPromptDiscarded: true })!
    const after = reconcileCppMessages('chat', before, [discarded], true, 7)
    expect(after[0]).not.toBe(before[0])
    expect(after[0]).toMatchObject({ acpPromptDiscarded: true, acpPromptNotSent: true, nativeMessageIndex: 7, content: message.content })
    expect(sanitizeCppMessage({ ...message, acpPromptDiscarded: 'true' })?.acpPromptDiscarded).toBe(false)
  })
	it('preserves persisted interruption and steering provenance', () => {
		const sanitized = sanitizeCppMessage({ ...message, interrupted: true, prioritySteer: true, continuesTurn: true })
		const reconciled = reconcileCppMessages('chat-1', undefined, [sanitized!])

		expect(reconciled[0].interrupted).toBe(true)
		expect(reconciled[0].prioritySteer).toBe(true)
		expect(reconciled[0].continuesTurn).toBe(true)
		const ordinary = reconcileCppMessages('chat-1', reconciled, [message])
		expect(ordinary[0].continuesTurn).toBe(false)
		expect(ordinary[0]).not.toBe(reconciled[0])
	})

  it('keeps an ordinary attachment-only update', () => {
    const existing = reconcileCppMessages('chat-1', undefined, [message])
    const attachment = {
      id: 'file-1',
      name: 'diagram.png',
      type: 'image/png',
      size: 42,
      path: '/tmp/diagram.png',
    }

    const reconciled = reconcileCppMessages('chat-1', existing, [{
      ...message,
      attachments: [attachment],
    }])

    expect(reconciled).not.toBe(existing)
    expect(reconciled[0].attachments).toEqual([{ ...attachment, mimeType: 'image/png' }])
  })

  it('keeps a markdown-store attachment-only update', () => {
    const existing = reconcileCppMessages('chat-1', undefined, [message])
    const filePath = '/tmp/skill-builder.uam'

    const reconciled = reconcileCppMessages('chat-1', existing, [{
      ...message,
      markdownStoreFiles: [filePath],
    }])

    expect(reconciled).not.toBe(existing)
    expect(reconciled[0].attachments).toEqual([{
      id: filePath,
      name: 'skill-builder.uam',
      type: 'markdown-store',
      size: 0,
      path: filePath,
    }])
  })

  it('keeps markdown-store files after sanitizing a backend message', () => {
    const filePath = '/tmp/skill-builder.uam'
    const sanitized = sanitizeCppMessage({
      ...message,
      markdownStoreFiles: [filePath],
    })

    expect(reconcileCppMessages('chat-1', undefined, [sanitized!])[0].attachments).toEqual([{
      id: filePath,
      name: 'skill-builder.uam',
      type: 'markdown-store',
      size: 0,
      path: filePath,
    }])
  })
})

describe('reconcileCppMessages deferred tool content', () => {
	it('keeps stable identity for timestamp-less remote messages', () => {
		const timestampLess = { ...message, createdAt: '' }
		const first = reconcileCppMessages('chat-1', undefined, [timestampLess])
		const second = reconcileCppMessages('chat-1', first, [timestampLess])
		expect(second).toBe(first)
		expect(second[0].id).toBe(first[0].id)
	})

  it('replaces a tool call when only contentDeferred changes', () => {
    const toolMessage: CppMessage = {
      role: 'assistant',
      content: 'Done.',
      createdAt: '2026-01-01T00:00:00.000Z',
      toolCalls: [{
        id: 'tool-1',
        title: 'Read file',
        kind: 'read',
        status: 'completed',
        content: '',
        contentDeferred: false,
      }],
    }
    const existing = reconcileCppMessages('chat-1', undefined, [toolMessage])

    const reconciled = reconcileCppMessages('chat-1', existing, [{
      ...toolMessage,
      toolCalls: [{ ...toolMessage.toolCalls![0], contentDeferred: true }],
    }], true)

    expect(reconciled).not.toBe(existing)
    expect(reconciled[0].toolCalls?.[0].contentDeferred).toBe(true)
    const revised = sanitizeCppMessage({
      ...toolMessage,
      toolCalls: [{ ...toolMessage.toolCalls![0], contentDeferred: true, contentDigest: 'revised' }],
    })!
    const refreshed = reconcileCppMessages('chat-1', reconciled, [revised], true)
    expect(refreshed).not.toBe(reconciled)
    expect(refreshed[0].toolCalls?.[0].contentDigest).toBe('revised')

  })
})

describe('unsent prompt native authority', () => {
  it('keeps valid real queue references and invalidates binding reuse when their raw index changes', () => {
    const queued = { id: 'held', revision: 1, text: 'Original', existingUserMessageIndex: 23 }
    const session = sanitizeCppAcpSession({ queuedPrompts: [queued] })!
    expect(session.queuedPrompts?.[0].existingUserMessageIndex).toBe(23)
    for (const invalid of [-1, 1.5, '23', null, Number.MAX_SAFE_INTEGER + 1]) {
      expect(sanitizeCppAcpSession({ queuedPrompts: [{ ...queued, existingUserMessageIndex: invalid }] })?.queuedPrompts?.[0].existingUserMessageIndex).toBeUndefined()
    }
    expect(sanitizeCppAcpSession({ queuedPrompts: [{ ...queued, existingUserMessageIndex: 0 }] })?.queuedPrompts?.[0].existingUserMessageIndex).toBe(0)
    const chat = { id: 'chat', providerId: 'codex-cli', acpSession: session } as CppChat
    const first = acpBindingFromCppChat(chat, undefined)
    expect(acpBindingFromCppChat(chat, first)).toBe(first)
    const changed = acpBindingFromCppChat({ ...chat, acpSession: { ...session,
      queuedPrompts: session.queuedPrompts?.map((prompt) => ({ ...prompt, existingUserMessageIndex: 24 })) } }, first)
    expect(changed).not.toBe(first)
    expect(changed.queuedPrompts?.[0].existingUserMessageIndex).toBe(24)
  })

  it('sanitizes absolute native indices and invalidates binding reuse when retry authority changes', () => {
    const acp = sanitizeCppAcpSession({ unsentMessageIndices: [0, 7, -1, 1.5, '8', null], recoverableUnsentMessageIndices: [7, '7', -1] })!
    expect(acp.unsentMessageIndices).toEqual([0, 7])
    expect(acp.recoverableUnsentMessageIndices).toEqual([7])
    const chat = { id: 'chat', providerId: 'opencode-cli', acpSession: acp } as CppChat
    const first = acpBindingFromCppChat(chat, undefined)
    expect(acpBindingFromCppChat(chat, first)).toBe(first)
    const blocked = acpBindingFromCppChat({ ...chat, acpSession: { ...acp, recoverableUnsentMessageIndices: [] } }, first)
    expect(blocked).not.toBe(first)
    expect(blocked.unsentMessageIndices).toEqual([0, 7])
    expect(blocked.recoverableUnsentMessageIndices).toEqual([])
    const delivered = acpBindingFromCppChat({ ...chat, acpSession: { ...acp, unsentMessageIndices: [0] } }, first)
    expect(delivered).not.toBe(first)
    expect(delivered.unsentMessageIndices).toEqual([0])
  })
})

describe('backend state reconciliation', () => {
	it('preserves the blocker category used by the goal banner', () => {
		expect(sanitizeCppGoal({
			id: 'goal-1', objective: 'Finish safely', status: 'blocked',
			lastBlocker: 'Approval required', lastBlockerKind: 'permission',
			createdAt: '', updatedAt: '',
		})?.lastBlockerKind).toBe('permission')
	})

  it('keeps provider package metadata after sanitizing', () => {
    expect(sanitizeCppProvider({
      id: 'custom',
      name: 'Custom',
      shortName: 'Custom',
      npmPackageName: '@example/custom-cli',
    })?.npmPackageName).toBe('@example/custom-cli')
  })

  it('updates a pending permission when only its safety assessment changes', () => {
    const chat: CppChat = {
      id: 'chat-1',
      title: 'Chat',
      folderId: '',
      providerId: 'codex-cli',
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-01-01T00:00:00.000Z',
      acpSession: {
        pendingPermission: {
          requestId: 'permission-1',
          toolCallId: 'tool-1',
          title: 'Run command',
          kind: 'execute',
          status: 'pending',
          content: 'npm test',
          safetyRisk: 'allowed',
          safetyTier: 'low',
          safetyRequiresApproval: false,
          options: [],
        },
      },
    }
    const previous = acpBindingFromCppChat(chat, undefined)
    const next = acpBindingFromCppChat({
      ...chat,
      acpSession: {
        ...chat.acpSession,
        pendingPermission: {
          ...chat.acpSession!.pendingPermission!,
          safetyRisk: 'warn_high',
          safetyTier: 'high',
          safetyRequiresApproval: true,
        },
      },
    }, previous)

    expect(next).not.toBe(previous)
    expect(next.pendingPermission?.safetyRisk).toBe('warn_high')
  })

  it('keeps the native idle shutdown deadline through sanitization and reconciliation', () => {
    const chat: CppChat = {
      id: 'chat-1', title: 'Chat', folderId: '', providerId: 'codex-cli',
      createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:00.000Z',
      acpSession: sanitizeCppAcpSession({ running: true, lifecycleState: 'ready',
        idleShutdownAtMs: 1_000_000, idleShutdownTimeoutSeconds: 600 }),
    }
    const previous = acpBindingFromCppChat(chat, undefined)
    expect(previous.idleShutdownAtMs).toBe(1_000_000)
    const next = acpBindingFromCppChat({ ...chat, acpSession: sanitizeCppAcpSession({
      running: true, lifecycleState: 'processing',
    }) }, previous)
    expect(next).not.toBe(previous)
    expect(next.idleShutdownAtMs).toBeUndefined()
  })

  it('keeps current context separate from consumed tokens and rejects malformed snapshots', () => {
    const contextUsage = { updatedAt: 1000, usedTokens: 1234, windowTokens: 200000 }
    expect(sanitizeCppAcpSession({ providerUsage: { contextUsage } })?.providerUsage).toMatchObject({ contextUsage, tokenUsage: null, rateLimits: null })
    expect(sanitizeCppAcpSession({ providerUsage: { contextUsage: { ...contextUsage, usedTokens: 0 } } })?.providerUsage?.contextUsage?.usedTokens).toBe(0)
    for (const invalid of [{ usedTokens: -1 }, { usedTokens: null }, { usedTokens: 1.5 }, { usedTokens: 200001 }, { windowTokens: 0 }, { windowTokens: Number.MAX_SAFE_INTEGER + 1 }]) expect(sanitizeCppAcpSession({ providerUsage: { contextUsage: { ...contextUsage, ...invalid } } })?.providerUsage).toBeNull()
  })

  it('keeps cumulative usage when the last-turn breakdown is unknown', () => {
    const tokenUsage = { updatedAt: 1000, total: { totalTokens: 123 }, last: null }
    const parsed = sanitizeCppAcpSession({ providerUsage: { tokenUsage } })?.providerUsage?.tokenUsage
    expect(parsed?.total.totalTokens).toBe(123)
    expect(parsed?.last).toBeNull()
    for (const last of [undefined, { totalTokens: -1 }, { totalTokens: Number.MAX_SAFE_INTEGER + 1 }]) {
      expect(sanitizeCppAcpSession({ providerUsage: { tokenUsage: { ...tokenUsage, last } } })?.providerUsage).toBeNull()
    }
  })

  it('sanitizes and reconciles provider usage updates', () => {
    const usage = {
      tokenUsage: {
        updatedAt: 1000,
        total: { totalTokens: 12345 },
        last: { totalTokens: 1234 },
        modelContextWindow: 200000,
      },
      rateLimits: {
        updatedAt: 1000,
        limitId: 'codex',
        limitName: 'Codex',
        primary: { usedPercent: 42, resetsAt: 1786118400, windowDurationMinutes: 300 },
        secondary: null,
      },
    }
    const chat: CppChat = {
      id: 'chat-usage',
      title: 'Usage',
      folderId: '',
      providerId: 'codex-cli',
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-01-01T00:00:00.000Z',
      acpSession: sanitizeCppAcpSession({ providerUsage: usage }),
    }

    const previous = acpBindingFromCppChat(chat, undefined)
    expect(previous.providerUsage?.tokenUsage?.total.totalTokens).toBe(12345)
    expect(previous.providerUsage?.rateLimits?.primary?.usedPercent).toBe(42)

    const next = acpBindingFromCppChat({
      ...chat,
      acpSession: sanitizeCppAcpSession({
        providerUsage: {
          ...usage,
          rateLimits: { ...usage.rateLimits, primary: { ...usage.rateLimits.primary, usedPercent: 43 } },
        },
      }),
    }, previous)
    expect(next).not.toBe(previous)
    expect(next.providerUsage?.rateLimits?.primary?.usedPercent).toBe(43)

    expect(sanitizeCppAcpSession({
      providerUsage: {
        tokenUsage: { updatedAt: 1000, total: { totalTokens: -1 }, last: { totalTokens: 0 } },
        rateLimits: { updatedAt: 1000, primary: { usedPercent: 101 } },
      },
    })?.providerUsage).toBeNull()
  })
})


describe('historical model provenance', () => {
  it('preserves model snapshots and accepts a model-only backend update', () => {
    const oldMessage = sanitizeCppMessage({...message,role:'assistant',modelId:'model-first'})!
    const first = reconcileCppMessages('chat-1',undefined,[oldMessage])
    expect(first[0].modelId).toBe('model-first')
    const next = reconcileCppMessages('chat-1',first,[{...oldMessage,modelId:'model-second'}])
    expect(next[0].modelId).toBe('model-second')
    expect(first[0].modelId).toBe('model-first')
    expect(sanitizeCppMessage(message)?.modelId).toBeUndefined()
  })
})

describe('Chat attention revision sanitization', () => {
  it('preserves opaque revisions and explicit clearing while rejecting malformed values', () => {
    const chat = { id: 'attention-chat', createdAt: '', updatedAt: '' }
    expect(sanitizeCppChat({ ...chat, attentionRevision: 'completion-one' })?.attentionRevision).toBe('completion-one')
    expect(sanitizeCppChat({ ...chat, attentionRevision: '' })?.attentionRevision).toBe('')
    for (const attentionRevision of [undefined, null, 12, { id: 'completion' }]) {
      expect(sanitizeCppChat({ ...chat, attentionRevision })?.attentionRevision).toBeUndefined()
    }
  })
})

describe('compaction reload and live event parity', () => {
  it('keeps provider summaries as compaction blocks through save reload reconciliation', () => {
    const summary = { type: 'context_compaction', text: 'Keep the selected workspace.', requestId: 'compact-1' }
    const persisted = sanitizeCppMessage({ ...message, role: 'assistant', content: '', blocks: [summary] })!
    const reconciled = reconcileCppMessages('chat-1', undefined, [persisted])
    expect(reconciled[0].content).toBe('')
    expect(reconciled[0].blocks).toEqual([summary])
    const live = sanitizeCppAcpSession({ turnEvents: [summary] })
    expect(live?.turnEvents).toEqual(reconciled[0].blocks)
    const reloaded = reconcileCppMessages('chat-1', reconciled, [persisted])
    expect(reloaded[0].blocks).toEqual([summary])
  })
})


describe('CLI context preparation', () => {
  it('keeps a pending native context launch processing before its PTY starts', () => {
    const state = normalizeCliLifecycleState('starting', false)
    expect(state).toBe('starting')
    expect(cliLifecycleIsProcessing(state)).toBe(true)
  })
})

it('preserves state event recency through bridge sanitization and streaming snapshots', () => {
  const chat = sanitizeCppChat({
    id: 'recency', title: 'Recency', createdAt: '2026-01-01T00:00:00Z',
    updatedAt: '2026-01-01T00:05:00Z', interactionAt: '2026-01-01T00:01:00Z',
  })!
  const first = sessionFromCppChat(chat, undefined, [])
  expect(first.interactionAt?.toISOString()).toBe('2026-01-01T00:01:00.000Z')
  const streamed = sessionFromCppChat({ ...chat, updatedAt: '2026-01-01T00:06:00Z' }, first, [])
  expect(streamed.interactionAt?.getTime()).toBe(first.interactionAt?.getTime())
  const completed = sessionFromCppChat({ ...chat, interactionAt: '2026-01-01T00:07:00Z' }, streamed, [])
  expect(completed.interactionAt?.getTime()).toBeGreaterThan(streamed.interactionAt!.getTime())
  const legacy = sessionFromCppChat({ ...chat, interactionAt: undefined }, streamed, [])
  expect(legacy.interactionAt?.getTime()).toBe(streamed.interactionAt?.getTime())
})

describe('native inactivity deadlines', () => {
  it('preserves grace and shutdown timing for both views and clears stale timing', () => {
    const timing = { idleCountdownStartsAtMs: 60_000, idleShutdownAtMs: 660_000, idleShutdownTimeoutSeconds: 600 }
    const chat = sanitizeCppChat({ id: 'timing', name: 'Timing', createdAt: '', updatedAt: '',
      cliTerminal: { running: true, lifecycleState: 'idle', ...timing },
      acpSession: { running: true, lifecycleState: 'ready', ...timing } })!
    const cli = cliBindingFromCppChat(chat, undefined)!
    const acp = acpBindingFromCppChat(chat, undefined)
    expect(cli.idleCountdownStartsAtMs).toBe(60_000)
    expect(cli.idleShutdownAtMs).toBe(acp.idleShutdownAtMs)
    expect(acp.idleShutdownTimeoutSeconds).toBe(600)
    expect(cliBindingFromCppChat({ ...chat, cliTerminal: { running: false, lastError: '' } }, cli)?.idleShutdownAtMs).toBeUndefined()
    expect(acpBindingFromCppChat({ ...chat, acpSession: { running: false } }, acp).idleShutdownAtMs).toBeUndefined()
  })
})

describe('question selection mode reconciliation', () => {
  it('updates the binding when an otherwise identical question becomes multi-select', () => {
    const question = { id: 'scope', header: 'Scope', question: 'Which checks?', isOther: true, isSecret: false, options: [] }
    const chat: CppChat = {
      id: 'chat-1', title: 'Chat', folderId: '', providerId: 'claude-cli', createdAt: '', updatedAt: '',
      acpSession: { pendingUserInput: { requestId: 'request-1', itemId: 'tool-1', status: 'pending', questions: [question] } },
    }
    const previous = acpBindingFromCppChat(chat, undefined)
    const next = acpBindingFromCppChat({ ...chat, acpSession: {
      pendingUserInput: { ...chat.acpSession!.pendingUserInput!, questions: [{ ...question, isMultiple: true }] },
    } }, previous)
    expect(next).not.toBe(previous)
    expect(next.pendingUserInput?.questions[0].isMultiple).toBe(true)
  })
})

it('preserves empty imported interaction timestamps across reconciliation and activation', () => {
  const chat = sanitizeCppChat({ id: 'imported', name: 'External', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:05:00Z', interactionAt: '' })!
  const imported = sessionFromCppChat(chat, undefined, [])
  expect(imported.interactionAt).toBeNull()
  const refreshed = sessionFromCppChat({ ...chat, updatedAt: '2026-01-01T00:06:00Z' }, imported, [])
  expect(refreshed.interactionAt).toBeNull()
  const used = sessionFromCppChat({ ...chat, interactionAt: '2026-01-01T00:07:00Z' }, refreshed, [])
  expect(used.interactionAt?.toISOString()).toBe('2026-01-01T00:07:00.000Z')
})


describe('remote recovery snapshot reconciliation', () => {
  const chat = sanitizeCppChat({
    id: 'remote-recovery', title: 'Remote', createdAt: '2026-01-01T00:00:00Z',
    updatedAt: '2026-01-01T00:05:00Z', remoteRecoveryEnabled: false, remoteRecoveryState: '',
  })!

  it('applies recovery enablement without a timestamp change', () => {
    const previous = sessionFromCppChat(chat, undefined, [])
    const next = sessionFromCppChat({ ...chat, remoteRecoveryEnabled: true }, previous, [])
    expect(next.remoteRecoveryEnabled).toBe(true)
    expect(next).not.toBe(previous)
  })

  it.each(['waiting', 'recovering', 'failed', ''])('applies recovery state %j independently', (remoteRecoveryState) => {
    const previousChat = { ...chat, remoteRecoveryEnabled: true, remoteRecoveryState: 'reconnecting' }
    const previous = sessionFromCppChat(previousChat, undefined, [])
    const next = sessionFromCppChat({ ...previousChat, remoteRecoveryState }, previous, [])
    expect(next.remoteRecoveryState).toBe(remoteRecoveryState)
    expect(next).not.toBe(previous)
    expect(sessionFromCppChat({ ...previousChat, remoteRecoveryState }, next, [])).toBe(next)
  })
})

describe('native compaction lifecycle', () => {
  it('propagates start and completion without changing turn identity and rejects stale stopped state', () => {
    const chat = { id: 'compacting', acpSession: sanitizeCppAcpSession({ running: true, processing: true, lifecycleState: 'processing', compacting: true }) } as CppChat
    const active = acpBindingFromCppChat(chat, undefined)
    expect(active.compacting).toBe(true)
    const completed = acpBindingFromCppChat({ ...chat, acpSession: { ...chat.acpSession, compacting: false } }, active)
    expect(completed).not.toBe(active)
    expect(completed.compacting).toBe(false)
    expect(acpBindingFromCppChat({ ...chat, acpSession: { ...chat.acpSession, running: false, processing: false, lifecycleState: 'stopped' } }, active).compacting).toBe(false)
    expect(sanitizeCppAcpSession({ compacting: 'true' })?.compacting).toBe(false)
  })
})

 it('propagates native child activity through sanitization and immutable binding reconciliation', () => {
  const chat = { id: 'child-parent', acpSession: sanitizeCppAcpSession({ lifecycleState: 'ready', running: true, processing: false, backgroundTaskCount: 1 }) } as CppChat
  const working = acpBindingFromCppChat(chat, undefined)
  expect(working.processing).toBe(false)
  expect(working.backgroundTaskCount).toBe(1)
  const settled = acpBindingFromCppChat({ ...chat, acpSession: sanitizeCppAcpSession({ lifecycleState: 'ready', running: true, processing: false, backgroundTaskCount: 0 }) }, working)
  expect(settled).not.toBe(working)
  expect(settled.backgroundTaskCount).toBe(0)
  for (const value of [-1, 1.5, '1', Infinity]) expect(sanitizeCppAcpSession({ backgroundTaskCount: value })?.backgroundTaskCount).toBe(0)
 })

describe('reported compaction counts', () => {
  it('preserves zero and rejects malformed counts without inventing a reduction', () => {
    const valid = sanitizeCppMessage({ role: 'assistant', content: '', blocks: [{ type: 'context_compaction', tokensBefore: 1234, tokensAfter: 0 }] })
    expect(valid?.blocks?.[0]).toMatchObject({ tokensBefore: 1234, tokensAfter: 0 })
    for (const invalid of [-1, 1.5, '1200', Number.MAX_SAFE_INTEGER + 1, Infinity]) {
      const result = sanitizeCppMessage({ role: 'assistant', content: '', blocks: [{ type: 'context_compaction', tokensBefore: invalid, tokensAfter: invalid }] })
      expect(result?.blocks?.[0]).toMatchObject({ tokensBefore: undefined, tokensAfter: undefined })
    }
    expect(messageBlocksEquivalent([{ type: 'context_compaction', text: '', tokensBefore: 1234 }],
      [{ type: 'context_compaction', text: '', tokensBefore: 1235 }])).toBe(false)
  })
})

describe('remote host startup applied state sanitization', () => {
  const host = { id: 'h1', sshAlias: 'h1', label: 'H1', runnerStatus: 'ready' }

  it('keeps applied state absent when the backend does not report it', () => {
    const [entry] = sanitizeCppSettings({ executionHosts: [host] }).executionHosts!.slice(1)
    expect(entry).not.toHaveProperty('runnerStartupApplied')
    expect(entry).not.toHaveProperty('runnerStartupError')
  })

  it('accepts known applied states and sanitizes the error to one bounded line', () => {
    const [entry] = sanitizeCppSettings({
      executionHosts: [{ ...host, runnerStartupApplied: 'failed', runnerStartupError: `bad\nline\u0007${'x'.repeat(400)}` }],
    }).executionHosts!.slice(1)
    expect(entry.runnerStartupApplied).toBe('failed')
    expect(entry.runnerStartupError).not.toMatch(/[\u0000-\u001f\u007f]/)
    expect(entry.runnerStartupError!.length).toBe(300)
  })

  it('drops unknown applied states', () => {
    const [entry] = sanitizeCppSettings({ executionHosts: [{ ...host, runnerStartupApplied: 'exploded' }] }).executionHosts!.slice(1)
    expect(entry).not.toHaveProperty('runnerStartupApplied')
  })
})

describe('chat custom icon data', () => {
  it('reconciles icon-only changes, removal and owned image changes without discarding unchanged sessions', () => {
    const chat = sanitizeCppChat({ id: 'icon-chat', title: 'Icon chat', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' })!
    const first = sessionFromCppChat({ ...chat, customIcon: { type: 'text', value: '📌' } }, undefined, [])
    expect(first.customIcon).toEqual({ type: 'text', value: '📌' })
    expect(sessionFromCppChat({ ...chat, customIcon: { type: 'text', value: '📌' } }, first, [])).toBe(first)
    const changed = sessionFromCppChat({ ...chat, customIcon: { type: 'text', value: '🚀' } }, first, [])
    expect(changed).not.toBe(first); expect(changed.customIcon?.value).toBe('🚀')
    const removed = sessionFromCppChat(chat, changed, [])
    expect(removed).not.toBe(changed); expect(removed.customIcon).toBeUndefined()
    const png = { type: 'png' as const, value: '12345678-1234-1234-1234-123456789abc.png', dataUrl: 'data:image/png;base64,YQ==' }
    const image = sessionFromCppChat({ ...chat, customIcon: png }, removed, [])
    expect(sessionFromCppChat({ ...chat, customIcon: { ...png, dataUrl: undefined } }, image, [])).not.toBe(image)
  })
})


describe('held outbox history admission', () => {
  const chat: CppChat = { id: 'held', title: 'Held', createdAt: '', updatedAt: '', messageCount: 0, messagesDigest: 'native-empty',
    acpSession: { queueHeld: true, queuedPrompts: [{ id: 'actual-queue-id', revision: 1, text: 'Unsent' }] } }
  const existing = reconcileCppMessages('held', undefined, [{ role: 'user', content: 'Unsent', createdAt: '', acpPromptNotSent: true }], true)
  it('requires an explicit native count, digest and stable held identity before retiring history', () => {
    expect(reconcileHeldOutboxHistory(chat, existing)).toEqual([])
    for (const incomplete of [
      { ...chat, messageCount: undefined }, { ...chat, messagesDigest: '' },
      { ...chat, acpSession: { ...chat.acpSession, queueHeld: false } },
      { ...chat, acpSession: { ...chat.acpSession, queuedPrompts: [{ id: 'actual-queue-id', revision: 0, text: 'Unsent' }] } },
    ]) expect(reconcileHeldOutboxHistory(incomplete, existing)).toBe(existing)
    expect(sanitizeCppChat({ id: 'no-count' })?.messageCount).toBeUndefined()
    const skillChanged = { ...chat, acpSession: { ...chat.acpSession, queuedPrompts: [{ id: 'actual-queue-id', revision: 1, text: 'Unsent', markdownStoreFiles: ['/skills/different.md'] }] } }
    expect(reconcileHeldOutboxHistory(skillChanged, existing)).toBe(existing)
  })
  it('requires prior native-known exact held references before retiring a disappeared queue tail', () => {
    const prior = acpBindingFromCppChat({ ...chat, acpSession: { ...chat.acpSession, unsentMessageIndices: [0],
      queuedPrompts: [{ ...chat.acpSession!.queuedPrompts![0], existingUserMessageIndex: 0 }] } }, undefined)
    const oldSummary = { messageCount: 1, messagesDigest: 'before-remove' }
    const removed = { ...chat, acpSession: { queueHeld: true, queuedPrompts: [] } }
    expect(reconcileHeldOutboxHistory(removed, existing, 0, prior, oldSummary)).toEqual([])
    for (const changed of [
      { ...prior, unsentMessageIndices: [] }, { ...prior, queueHeld: false },
      { ...prior, queuedPrompts: prior.queuedPrompts?.map((prompt) => ({ ...prompt, existingUserMessageIndex: 1 })) },
      { ...prior, queuedPrompts: prior.queuedPrompts?.map((prompt) => ({ ...prompt, markdownStoreFiles: ['/different-skill.md'] })) },
    ]) expect(reconcileHeldOutboxHistory(removed, existing, 0, changed, oldSummary)).toBe(existing)
    const later = [...existing, { ...existing[0], nativeMessageIndex: undefined, content: 'Newer optimistic input' }]
    expect(reconcileHeldOutboxHistory(removed, later, 0, prior, oldSummary)).toEqual([later[1]])
    expect(reconcileHeldOutboxHistory(removed, later, 0, prior, oldSummary)?.[0]).toBe(later[1])
    const actualLater = [...existing, { ...existing[0], nativeMessageIndex: 1, content: 'Later native input' }]
    expect(reconcileHeldOutboxHistory(removed, actualLater, 0, prior, oldSummary)).toBe(actualLater)
    expect(reconcileHeldOutboxHistory(removed, existing, 0, prior, { ...oldSummary, messagesDigest: removed.messagesDigest })).toBe(existing)
    const delivered = existing.map((message) => ({ ...message, acpPromptNotSent: false }))
    expect(reconcileHeldOutboxHistory(removed, delivered, 0, prior, oldSummary)).toBe(delivered)
  })
  it('preserves partial output, opaque optimistic attempts and changed attachment intent', () => {
    for (const suffix of [
      [{ ...existing[0], nativeMessageIndex: undefined }],
      [{ ...existing[0], attachments: [{ id: 'different', name: 'x', path: '/x', size: 1, type: 'text/plain' }] }],
      [...existing, { ...existing[0], role: 'assistant' as const, content: 'Actual output', nativeMessageIndex: 1 }],
      [...existing, { ...existing[0], content: 'Newer attempt', nativeMessageIndex: 1 }],
    ]) expect(reconcileHeldOutboxHistory(chat, suffix)).toBe(suffix)
  })
})
