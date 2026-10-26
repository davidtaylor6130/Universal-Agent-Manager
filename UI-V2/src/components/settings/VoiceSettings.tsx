import { useEffect, useState } from 'react'
import { isCefContext, isCompanionContext, listSpeechVoices, type SpeechVoice } from '../../ipc/cefBridge'
import { useVoicePreferences, type VoicePreferences } from '../../hooks/useVoiceMode'

export function VoiceSettings() {
  const { preferences, save } = useVoicePreferences()
  const [voices, setVoices] = useState<SpeechVoice[]>([])
  const [error, setError] = useState('')
  const available = isCefContext() && !isCompanionContext()
  useEffect(() => {
    if (!available) return
    let cancelled = false
    void listSpeechVoices().then(response => {
      if (cancelled) return
      if (response.ok && Array.isArray(response.data?.voices)) setVoices(response.data.voices.filter(voice => voice && typeof voice.id === 'string' && typeof voice.name === 'string' && typeof voice.language === 'string'))
      else setError(response.error || 'Installed voices could not be loaded.')
    })
    return () => { cancelled = true }
  }, [available])
  const update = (patch: Partial<VoicePreferences>) => setError(save(patch) ? '' : 'Voice preferences could not be saved.')
  const languages = [...new Set(voices.map(voice => voice.language))].filter(Boolean).sort()
  return <div className="grid gap-3 text-sm" style={{ color: 'var(--text)' }}>
    <p>{available ? 'Voice mode uses the native system speech service. Start it in the composer. Input pauses during playback.' : 'The desktop app uses the native system speech service. Use your keyboard microphone for dictation here.'}</p>
    <fieldset disabled={!available} className="grid gap-3">
      <label className="flex items-center justify-between gap-3">Activation shortcut<select aria-label="Voice activation shortcut" className="uam-field" value={preferences.shortcut} onChange={event => update({ shortcut: event.target.value as VoicePreferences['shortcut'] })}><option value="v">Ctrl/Command + Shift + V</option><option value="m">Ctrl/Command + Shift + M</option><option value="off">Button only</option></select></label>
      <label className="flex items-center justify-between gap-3">Language<select aria-label="Voice language" className="uam-field" value={preferences.language} onChange={event => update({ language: event.target.value, voiceId: '' })}><option value="">System language</option>{preferences.language && !languages.includes(preferences.language) && <option value={preferences.language}>{preferences.language}</option>}{languages.map(language => <option key={language} value={language}>{language}</option>)}</select></label>
      <label className="flex items-center justify-between gap-3">Installed voice<select aria-label="Installed voice" className="uam-field" value={preferences.voiceId} onChange={event => update({ voiceId: event.target.value })}><option value="">System voice</option>{preferences.voiceId && !voices.some(voice => voice.id === preferences.voiceId) && <option value={preferences.voiceId} disabled>Saved voice unavailable</option>}{voices.filter(voice => !preferences.language || voice.language === preferences.language).map(voice => <option key={voice.id} value={voice.id}>{voice.name} ({voice.language})</option>)}</select></label>
      <label className="flex items-center justify-between gap-3">Speed<input aria-label="Speech speed" type="number" min="0.5" max="2" step="0.1" className="uam-field" value={preferences.rate} onChange={event => { const rate = Number(event.target.value); if (rate >= 0.5 && rate <= 2) update({ rate }) }}/></label>
      <label className="flex items-center justify-between gap-3">Volume<input aria-label="Speech volume" type="range" min="0" max="1" step="0.05" value={preferences.volume} onChange={event => update({ volume: Number(event.target.value) })}/></label>
      <label className="flex items-center gap-2"><input type="checkbox" checked={preferences.readReplies} onChange={event => update({ readReplies: event.target.checked })}/>Read new responses aloud in voice mode</label>
    </fieldset>
    <p>Commands: send message, stop response, next chat, previous chat, stop voice mode.</p>
    {error && <p role="alert" style={{ color: 'var(--red)' }}>{error}</p>}
  </div>
}
