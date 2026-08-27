import { describe, expect, it } from 'vitest'
import type { CppChat, CppMessage } from './types'
import { acpBindingFromCppChat, reconcileCppMessages } from './reconcile'
import { sanitizeCppAcpSession, sanitizeCppMessage, sanitizeCppProvider } from './sanitizers'

const message: CppMessage = {
  role: 'user',
  content: 'Use the attached file.',
  createdAt: '2026-01-01T00:00:00.000Z',
}

describe('reconcileCppMessages attachments', () => {
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
    expect(reconciled[0].attachments).toEqual([attachment])
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
  })
})

describe('backend state reconciliation', () => {
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
