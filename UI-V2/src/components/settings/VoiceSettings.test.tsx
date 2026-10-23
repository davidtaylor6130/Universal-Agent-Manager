import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { VoiceSettings } from './VoiceSettings'
import { useVoicePreferences } from '../../hooks/useVoiceMode'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
it('enumerates actual native profiles, persists output preferences, and reports storage failure', async () => {
  const requests: string[] = []
  const stored = new Map<string, string>()
  const setItem = vi.fn((key: string, value: string) => { stored.set(key, value) })
  vi.stubGlobal('localStorage', { getItem: (key: string) => stored.get(key), setItem })
  useVoicePreferences.setState({ preferences: { language: '', voiceId: '', rate: 1, volume: 1, shortcut: 'v', readReplies: true } })
  window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request).action); onSuccess(JSON.stringify({ voices: [{ id: 'native-id', name: 'Installed English', language: 'en-GB' }] })) }
  const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
  await act(async () => root.render(<VoiceSettings/>))
  expect(requests).toEqual(['listSpeechVoices'])
  const profile = host.querySelector<HTMLSelectElement>('[aria-label="Installed voice"]')!
  expect(profile.textContent).toContain('Installed English (en-GB)')
  act(() => { profile.value = 'native-id'; profile.dispatchEvent(new Event('change', { bubbles: true })) })
  expect(JSON.parse(stored.get('uam-voice-preferences')!).voiceId).toBe('native-id')
  setItem.mockImplementation(() => { throw new Error('Storage denied') })
  const shortcut = host.querySelector<HTMLSelectElement>('[aria-label="Voice activation shortcut"]')!
  act(() => { shortcut.value = 'off'; shortcut.dispatchEvent(new Event('change', { bubbles: true })) })
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('could not be saved')
  expect(useVoicePreferences.getState().preferences.shortcut).toBe('v')
  act(() => root.unmount()); host.remove(); delete window.cefQuery; vi.unstubAllGlobals()
})
