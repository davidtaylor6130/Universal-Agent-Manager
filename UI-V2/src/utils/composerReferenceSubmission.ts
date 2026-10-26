import type { ContextReference } from '../types/contextReference'
import { readChatComposerDraft, writeChatComposerDraft } from './composerDraftStorage'
import { contextReferenceOwner } from './contextReferenceOwner'
import { sanitizeContextReferences } from './contextReferences'

export interface ComposerReferenceSubmission {
  chatId: string
  birth: number
  ownerKey: string
  references: readonly ContextReference[]
}

/** Freeze selected reference bytes before a goal/mode operation can await or a draft can change. */
export function captureComposerReferenceSubmission(chatId: string): ComposerReferenceSubmission | null {
  const owner = contextReferenceOwner(chatId)
  return owner ? { chatId, birth: owner.birth, ownerKey: owner.deliveryKey, references: sanitizeContextReferences(readChatComposerDraft(chatId).references) } : null
}

export function composerReferenceSubmissionIsCurrent(submission: ComposerReferenceSubmission): boolean {
  return contextReferenceOwner(submission.chatId)?.deliveryKey === submission.ownerKey
}

/** Admission consumes only unchanged submitted selections for the same chat birth; it is not a write/delivery fact. */
export function acknowledgeComposerReferences(submission: ComposerReferenceSubmission): void {
  if (contextReferenceOwner(submission.chatId)?.birth !== submission.birth || !submission.references.length) return
  const submitted = new Map(submission.references.map((reference) => [reference.id, JSON.stringify(reference)]))
  const current = readChatComposerDraft(submission.chatId)
  writeChatComposerDraft(submission.chatId, { ...current, references: (current.references ?? []).filter((reference) => submitted.get(reference.id) !== JSON.stringify(reference)) })
}
