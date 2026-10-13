import { useState } from 'react'
import { MenuSelect, Switch } from '../ui'
import { applyAppearance, readAppearance, writeAppearance, type AppearancePreferences, type ChatWidth, type DiffPalette } from '../../utils/appearancePreferences'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

const WIDTH_OPTIONS: Array<{ value: ChatWidth; label: string }> = [
  { value: 'comfortable', label: 'Comfortable' },
  { value: 'wide', label: 'Wide' },
  { value: 'full', label: 'Full width' },
]

const DIFF_OPTIONS: Array<{ value: DiffPalette; label: string }> = [
  { value: 'default', label: 'Green and red' },
  { value: 'colour-blind', label: 'Blue and orange (colour-blind friendly)' },
]

export function AppearanceDisplaySettings() {
  const [preferences, setPreferences] = useState<AppearancePreferences>(() => readAppearance())
  const update = (patch: Partial<AppearancePreferences>) => {
    const next = { ...preferences, ...patch }
    setPreferences(next)
    writeAppearance(next)
    applyAppearance(next)
  }
  return (
    <>
      <SettingsSection title="Chat width" description="How wide the conversation column is on screen.">
        <SettingRow title="Message width" description="Comfortable keeps lines short and easy to read. Full width uses the whole chat area."
          control={<div style={{ width: 170 }}>
            <MenuSelect label="Message width" value={preferences.chatWidth} options={WIDTH_OPTIONS}
              onChange={(value) => update({ chatWidth: value as ChatWidth })} />
          </div>} />
      </SettingsSection>
      <SettingsSection title="Contrast and diff colours" description="Make text, borders and code changes easier to tell apart.">
        <SettingRow title="Higher contrast text and borders" description="Stronger text and dividing lines across the whole app."
          control={<Switch hideLabel label="Higher contrast text and borders" checked={preferences.highContrast}
            onChange={(event) => update({ highContrast: event.target.checked })} />} />
        <SettingRow title="Added and removed lines" description="Colours used for added and removed lines in diffs and reviews."
          control={<div style={{ width: 220 }}>
            <MenuSelect label="Added and removed line colours" value={preferences.diffPalette} options={DIFF_OPTIONS}
              onChange={(value) => update({ diffPalette: value as DiffPalette })} />
          </div>} />
      </SettingsSection>
    </>
  )
}
