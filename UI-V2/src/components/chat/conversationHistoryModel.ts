import type { Message } from '../../types/message'
import { stoppedResponseLabel } from './MessageBlocks'

/** Pure history model shared by the main transcript and read-only child conversations. */
export const ALWAYS_OPEN_WORK_SECTION = { expanded: true, toggle: () => undefined }

export type WorkSection = { id: string; firstIndex: number; lastAssistantIndex: number; workedSeconds: number; hasAssistant: boolean; hasTrace: boolean }

function messageHasTrace(message: Message) {
  return Boolean(message.toolCalls?.length || message.thoughts?.trim() || message.blocks?.some((block) => block.type === 'tool_call' || (block.type === 'thought' && block.text.trim())))
}

export function findWorkSectionStartIndex(messages: Message[], index: number) {
  let start = Math.min(index, messages.length - 1)
  while (start > 0 && !(messages[start].role === 'user' && !messages[start].continuesTurn)) start -= 1
  return Math.max(0, start)
}

export function buildVisibleWorkSections(messages: Message[], firstVisibleIndex: number) {
  const sections = new Map<number, WorkSection>()
  if (messages.length === 0) return sections

  const startIndex = findWorkSectionStartIndex(messages, firstVisibleIndex)
  let section: WorkSection = { id: '', firstIndex: startIndex, lastAssistantIndex: -1, workedSeconds: 0, hasAssistant: false, hasTrace: false }
  for (let index = startIndex; index < messages.length; index += 1) {
    const message = messages[index]
    if (index === 0 || (message.role === 'user' && !message.continuesTurn))
      section = { id: message.id, firstIndex: index, lastAssistantIndex: -1, workedSeconds: 0, hasAssistant: false, hasTrace: false }
    section.workedSeconds = Math.max(section.workedSeconds, (message.processingTimeMs ?? 0) / 1000)
    section.hasAssistant ||= message.role === 'assistant'
    section.hasTrace ||= message.role === 'assistant' && messageHasTrace(message)
    if (message.role === 'assistant') section.lastAssistantIndex = index
    sections.set(index, section)
  }
  return sections
}

export function hasSubstantiveOutput(message: Message) {
  return Boolean(message.content.trim() || message.thoughts?.trim() || message.planSummary?.trim() ||
    message.planEntries?.length || message.toolCalls?.length || message.blocks?.length || message.attachments?.length)
}

/** Attribute each turn from its recorded metadata, independently of current provider/model settings. */
export function reviewAssistantLabel(provider: string, message?: Message) {
  const model = message?.modelId?.trim() || ''
  const date = message?.createdAt
  const time = date && Number.isFinite(date.getTime())
    ? date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : ''
  return [provider, model, time].filter(Boolean).join(' · ')
}

// ponytail: WeakMap keeps composer/stream renders from re-formatting settled message dates.
const turnTimeLabels = new WeakMap<Date, string>()
export function turnTimestampLabel(date: Date) {
  let label = turnTimeLabels.get(date)
  if (label === undefined) turnTimeLabels.set(date, label = date.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }))
  return label
}

/** Day divider wording: Today, Yesterday, or the calendar date. */
export function dayDividerLabel(date: Date, now = new Date()) {
  const dayOf = (value: Date) => new Date(value.getFullYear(), value.getMonth(), value.getDate()).getTime()
  const daysAgo = Math.round((dayOf(now) - dayOf(date)) / 86_400_000)
  if (daysAgo === 0) return 'Today'
  if (daysAgo === 1) return 'Yesterday'
  return date.toLocaleDateString([], { dateStyle: 'medium' })
}

export function previousAssistantMessage(messages: Message[], beforeIndex: number) {
  for (let index = beforeIndex - 1; index >= 0; index -= 1) if (messages[index].role === 'assistant') return messages[index]
  return undefined
}

/** Delivered/stopped flags for a user row, from the same-turn response's provider evidence. */
export function responseFlags(messages: Message[], index: number) {
  const message = messages[index]
  const followingResponse = message.role === 'user' && messages[index + 1]?.role === 'assistant' ? messages[index + 1] : undefined
  const hasDeliveredResponse = Boolean(message.interrupted && message.acpPromptNotSent && followingResponse?.providerId?.trim() &&
    followingResponse.providerId.trim() === message.providerId?.trim() && hasSubstantiveOutput(followingResponse))
  const hasStoppedResponse = Boolean(hasDeliveredResponse && stoppedResponseLabel(followingResponse?.stopReason, followingResponse?.interrupted))
  return { followingResponse, hasDeliveredResponse, hasStoppedResponse }
}
