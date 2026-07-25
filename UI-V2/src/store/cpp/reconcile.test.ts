import { describe, expect, it } from 'vitest'
import type { CppChat, CppMessage } from './types'
import { acpBindingFromCppChat, reconcileCppMessages } from './reconcile'
import { sanitizeCppMessage, sanitizeCppProvider } from './sanitizers'

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
})
