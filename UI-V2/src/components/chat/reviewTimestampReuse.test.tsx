import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from '../views/ChatView'
import { PersistedConversationHistory } from './PersistedConversationMessage'
import { reviewAssistantLabel, turnTimestampLabel } from './conversationHistoryModel'
import { useAppStore } from '../../store/useAppStore'
import { acpBindingFromCppChat } from '../../store/cpp/reconcile'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => { vi.unstubAllEnvs(); vi.restoreAllMocks() })

// Keep correctness oracles separate from the spy on production formatter calls.
const realTimeFormatter = Date.prototype.toLocaleTimeString
const localTime = (date: Date) => realTimeFormatter.call(date, [], { hour: '2-digit', minute: '2-digit' })
const realDateTimeFormatter = Date.prototype.toLocaleString
const localDateTime = (date: Date) => realDateTimeFormatter.call(date, [], { dateStyle: 'medium', timeStyle: 'short' })
const messageAt = (createdAt: Date): Message => ({ id: 'same-message-id', sessionId: 'timestamp-chat',
  role: 'assistant', content: 'Answer', modelId: ' recorded-model ', createdAt })

describe('review-label timestamp reuse', () => {
  it('refreshes turn timestamps without changing their date/time format or invalid-date contract', () => {
    const date = new Date('2026-01-01T09:10:00Z')
    const format = vi.spyOn(Date.prototype, 'toLocaleString')
    expect(turnTimestampLabel(date)).toBe(localDateTime(date))
    expect(turnTimestampLabel(date)).toBe(localDateTime(date))
    expect(format).toHaveBeenCalledTimes(1)
    date.setTime(Date.parse('2026-02-02T13:45:00Z'))
    expect(turnTimestampLabel(date)).toBe(localDateTime(date))
    expect(format).toHaveBeenCalledTimes(2)
    date.setTime(NaN)
    expect(turnTimestampLabel(date)).toBe(localDateTime(date))
    date.setTime(Date.parse('2026-03-03T17:20:00Z'))
    expect(turnTimestampLabel(date)).toBe(localDateTime(date))
    expect(format).toHaveBeenCalledTimes(4)
    expect(format).toHaveBeenLastCalledWith([], { dateStyle: 'medium', timeStyle: 'short' })
  })

  it('refreshes replaced, mutated and invalid timestamps without retaining provider or model labels', () => {
    const date = new Date('2026-01-01T09:10:00Z')
    const replacement = new Date('2026-02-02T13:45:00Z')
    const restored = new Date('2026-03-03T17:20:00Z')
    const firstTime = localTime(date)
    const nextTime = localTime(replacement)
    const restoredTime = localTime(restored)
    const format = vi.spyOn(Date.prototype, 'toLocaleTimeString')
    const message = messageAt(date)
    expect(reviewAssistantLabel('Codex', message)).toBe(`Codex · recorded-model · ${firstTime}`)
    expect(reviewAssistantLabel('Claude', { ...message, modelId: ' replacement-model ' })).toBe(`Claude · replacement-model · ${firstTime}`)
    expect(format).toHaveBeenCalledTimes(1)
    expect(reviewAssistantLabel('Codex', { ...message, createdAt: replacement })).toBe(`Codex · recorded-model · ${nextTime}`)
    expect(format).toHaveBeenCalledTimes(2)
    date.setTime(replacement.getTime())
    expect(reviewAssistantLabel('Codex', message)).toBe(`Codex · recorded-model · ${nextTime}`)
    expect(format).toHaveBeenCalledTimes(3)
    date.setTime(NaN)
    expect(reviewAssistantLabel('Codex', message)).toBe('Codex · recorded-model')
    expect(reviewAssistantLabel('Codex')).toBe('Codex')
    expect(format).toHaveBeenCalledTimes(3)
    date.setTime(restored.getTime())
    // The invalid-to-valid transition must use the current Date again.
    expect(reviewAssistantLabel('Codex', message)).toBe(`Codex · recorded-model · ${restoredTime}`)
    expect(format).toHaveBeenCalledTimes(4)
    expect(format).toHaveBeenLastCalledWith([], { hour: '2-digit', minute: '2-digit' })
  })

  it('does not reuse another chat’s time when a message ID is repeated', () => {
    const first = messageAt(new Date('2026-01-01T09:10:00Z'))
    const second = { ...first, sessionId: 'another-chat', createdAt: new Date('2026-01-01T14:35:00Z') }
    const firstTime = localTime(first.createdAt)
    const secondTime = localTime(second.createdAt)
    expect(firstTime).not.toBe(secondTime)
    expect(reviewAssistantLabel('Codex', first)).toBe(`Codex · recorded-model · ${firstTime}`)
    expect(reviewAssistantLabel('Codex', second)).toBe(`Codex · recorded-model · ${secondTime}`)
  })

  it.each(['main', 'passive'] as const)('reuses all 50 review times through 20 stream updates in %s history', (route) => {
    // This changes only the isolated test process, never the machine or app timezone.
    vi.stubEnv('TZ', 'UTC')
    const previous = useAppStore.getState()
    const session: Session = { id: 'timestamp-stream-chat', name: 'Timestamp fixture', viewMode: 'chat', folderId: null,
      providerId: 'gemini-cli', createdAt: new Date(0), updatedAt: new Date(0) }
    let messages: Message[] = Array.from({ length: 50 }, (_, index) => ({
      id: `timestamp-${index}`, sessionId: session.id, role: index % 2 ? 'assistant' : 'user',
      content: `Message ${index}`, providerId: 'gemini-cli', modelId: 'recorded-model',
      createdAt: new Date(Date.parse('2026-01-01T09:00:00Z') + index * 60_000),
      ...(index === 49 ? { isStreaming: true } : {}),
    }))
    const labels = messages.map(message => `Gemini · recorded-model · ${localTime(message.createdAt)}`)
    const replacement = new Date('2026-02-02T13:45:00Z')
    const replacementLabel = `Codex · replacement-model · ${localTime(replacement)}`
    const format = vi.spyOn(Date.prototype, 'toLocaleTimeString')
    const turnFormat = vi.spyOn(Date.prototype, 'toLocaleString')
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    const render = () => root.render(route === 'main' ? <ChatView session={session} /> :
      <PersistedConversationHistory messages={messages} start={0} end={messages.length} fallbackProviderId="gemini-cli" sessionId={session.id} />)
    const article = (index: number) => host.querySelector(`[data-message-index="${index}"] article.conversation-turn--assistant`)
    const assertTurnTime = (index: number, identity?: string) => {
      const row = host.querySelector(`.space-y-1[data-message-index="${index}"]`)
      const time = row?.querySelector('time.conversation-turn-start')
      const date = messages[index].createdAt
      expect(row?.getAttribute('title')).toBe(localDateTime(date))
      expect(time?.getAttribute('datetime')).toBe(date.toISOString())
      expect(time?.textContent).toBe(identity ?? localDateTime(date))
      expect(time?.classList.contains('sr-only')).toBe(identity === undefined)
    }
    try {
      useAppStore.setState({ sessions: [session], activeSessionId: session.id, folders: [], messages: { [session.id]: messages },
        historyStartIndexBySessionId: {}, chatHistoryErrorBySessionId: {}, cliBindingBySessionId: {},
        refreshUamAgents: vi.fn(async () => true), loadSessionMessages: vi.fn(async () => {}),
        acpBindingBySessionId: route === 'main' ? { [session.id]: acpBindingFromCppChat({
          id: session.id, title: session.name, folderId: '', providerId: 'gemini-cli',
          createdAt: session.createdAt.toISOString(), updatedAt: session.updatedAt.toISOString(), acpSession: {
            sessionId: 'timestamp-native', running: true, processing: true, lifecycleState: 'processing', protocolKind: 'gemini-acp',
            turnUserMessageIndex: 48, turnAssistantMessageIndex: 49, turnSerial: 1,
            turnEvents: [{ type: 'assistant_text', text: messages[49].content }],
          },
        }, undefined) } : {},
      })
      act(render)
      expect(format).toHaveBeenCalledTimes(50)
      assertTurnTime(0, 'Gemini · recorded-model')
      assertTurnTime(4)
      const initialTurnFormats = turnFormat.mock.calls.length
      for (let delta = 0; delta < 20; delta += 1) {
        const content = `Stream delta ${delta}`
        messages = messages.map((message, index) => index === 49 ? { ...message, content } : message)
        act(() => {
          useAppStore.setState(state => ({ messages: { [session.id]: messages },
            ...(route === 'main' ? { acpBindingBySessionId: { [session.id]: { ...state.acpBindingBySessionId[session.id],
              turnEvents: [{ type: 'assistant_text' as const, text: content }] } } } : {}),
          }))
          if (route === 'passive') render()
        })
        expect(article(49)?.textContent).toContain(content)
        expect(format).toHaveBeenCalledTimes(50)
        expect(turnFormat).toHaveBeenCalledTimes(initialTurnFormats)
        assertTurnTime(4)
        for (let index = 1; index < 50; index += 2) expect(article(index)?.getAttribute('aria-label')).toBe(labels[index])
      }
      const publish = () => act(() => {
        useAppStore.setState(state => ({ messages: { [session.id]: messages },
          ...(route === 'main' ? { acpBindingBySessionId: { [session.id]: { ...state.acpBindingBySessionId[session.id],
            turnEvents: [{ type: 'assistant_text' as const, text: messages[49].content }] } } } : {}),
        }))
        render()
      })
      messages = messages.map((message, index) => index === 1 ? { ...message,
        providerId: 'codex-cli', modelId: ' replacement-model ' } : message)
      publish()
      expect(article(1)?.getAttribute('aria-label')).toBe(`Codex · replacement-model · ${localTime(messages[1].createdAt)}`)
      expect(format).toHaveBeenCalledTimes(50)
      messages = messages.map((message, index) => index === 1 ? { ...message, createdAt: replacement } : message)
      publish()
      expect(article(1)?.getAttribute('aria-label')).toBe(replacementLabel)
      expect(article(3)?.getAttribute('aria-label')).toBe(labels[3])
      expect(format).toHaveBeenCalledTimes(51)

      const changeSameDate = (timestamp: number) => {
        replacement.setTime(timestamp)
        messages = messages.map((message, index) => index === 1 ? { ...message } : message)
        publish()
      }
      changeSameDate(Date.parse('2026-03-03T17:20:00Z'))
      expect(article(1)?.getAttribute('aria-label')).toBe(`Codex · replacement-model · ${localTime(replacement)}`)
      expect(format).toHaveBeenCalledTimes(52)
      changeSameDate(NaN)
      expect(article(1)?.getAttribute('aria-label')).toBe('Codex · replacement-model')
      expect(format).toHaveBeenCalledTimes(52)
      changeSameDate(Date.parse('2026-04-04T11:25:00Z'))
      expect(article(1)?.getAttribute('aria-label')).toBe(`Codex · replacement-model · ${localTime(replacement)}`)
      expect(format).toHaveBeenCalledTimes(53)

      const beforeZoneChange = article(1)?.getAttribute('aria-label')
      const originalOffset = replacement.getTimezoneOffset()
      vi.stubEnv('TZ', 'Pacific/Honolulu')
      expect(replacement.getTimezoneOffset()).not.toBe(originalOffset)
      // A real incoming delta must reach memoized ChatView after the environment changes.
      messages = messages.map((message, index) => index === 49 ? { ...message, content: 'Stream after timezone change' } : message)
      publish()
      expect(article(49)?.textContent).toContain('Stream after timezone change')
      expect(article(1)?.getAttribute('aria-label')).not.toBe(beforeZoneChange)
      for (let index = 1; index < 50; index += 2) {
        const identity = index === 1 ? 'Codex · replacement-model' : 'Gemini · recorded-model'
        expect(article(index)?.getAttribute('aria-label')).toBe(`${identity} · ${localTime(messages[index].createdAt)}`)
      }
      assertTurnTime(0, 'Gemini · recorded-model')
      assertTurnTime(4)
      expect(format).toHaveBeenCalledTimes(103)
      const zonedTurnFormats = turnFormat.mock.calls.length
      publish()
      expect(format).toHaveBeenCalledTimes(103)
      expect(turnFormat).toHaveBeenCalledTimes(zonedTurnFormats)

      // Turn dates belong to user rows, independently of the assistant review labels.
      const turnDate = messages[4].createdAt
      turnDate.setTime(Date.parse('2026-05-05T18:30:00Z'))
      messages = messages.map((message, index) => index === 4 ? { ...message } : message)
      publish()
      assertTurnTime(4)
      expect(turnFormat).toHaveBeenCalledTimes(zonedTurnFormats + 2) // Row title and memoized time text.
      turnDate.setTime(NaN)
      messages = messages.map((message, index) => index === 4 ? { ...message } : message)
      publish()
      const invalidRow = host.querySelector('.space-y-1[data-message-index="4"]')
      expect(invalidRow?.hasAttribute('title')).toBe(false)
      expect(invalidRow?.querySelector('time.conversation-turn-start')).toBeNull()
      turnDate.setTime(Date.parse('2026-06-06T19:40:00Z'))
      messages = messages.map((message, index) => index === 4 ? { ...message } : message)
      publish()
      assertTurnTime(4)
      messages = messages.map((message, index) => index === 4 ? { ...message, providerId: 'claude-cli', modelId: 'turn-model' } : message)
      publish()
      assertTurnTime(4, 'Claude · turn-model')
    } finally {
      act(() => root.unmount()); host.remove()
      useAppStore.setState(previous, true)
      vi.unstubAllGlobals()
    }
  })
})
