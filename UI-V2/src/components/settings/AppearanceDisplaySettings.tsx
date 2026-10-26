import { useState } from 'react'
import { MenuSelect, Switch } from '../ui'
import { applyAppearance, readAppearance, writeAppearance, type AppearancePreferences, type ChatWidth, type DiffPalette } from '../../utils/appearancePreferences'
import { MOTION_SPEEDS, applyMotionSpeed, readStoredMotion, writeStoredMotion, type MotionSpeed } from '../../utils/motionStorage'
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
  const [motionSpeed, setMotionSpeed] = useState<MotionSpeed>(readStoredMotion)
  const update = (patch: Partial<AppearancePreferences>) => {
    const next = { ...preferences, ...patch }
    setPreferences(next)
    writeAppearance(next)
    applyAppearance(next)
  }
  return (
    <SettingsSection title="Display">
      <SettingRow title="Message width"
        control={<div style={{ width: 170 }}>
          <MenuSelect label="Message width" value={preferences.chatWidth} options={WIDTH_OPTIONS}
            onChange={(value) => update({ chatWidth: value as ChatWidth })} />
        </div>} />
      <SettingRow title="Higher contrast text and borders"
        control={<Switch hideLabel label="Higher contrast text and borders" checked={preferences.highContrast}
          onChange={(event) => update({ highContrast: event.target.checked })} />} />
      <SettingRow title="Added and removed lines" description="Colours in diffs and reviews."
        control={<div style={{ width: 220 }}>
          <MenuSelect label="Added and removed line colours" value={preferences.diffPalette} options={DIFF_OPTIONS}
            onChange={(value) => update({ diffPalette: value as DiffPalette })} />
        </div>} />
      <SettingRow title="Model token limits" description="Show each model's reported context window and maximum output in the model picker."
        control={<Switch hideLabel label="Show model token limits" checked={preferences.modelTokenLimits}
          onChange={(event) => update({ modelTokenLimits: event.target.checked })} />} />
      <SettingRow title="Animation speed" description="Off removes transitions entirely."
        control={
          <MenuSelect
            label="Animation speed"
            value={motionSpeed}
            options={MOTION_SPEEDS.map((speed) => ({ value: speed.id, label: speed.label }))}
            onChange={(value) => {
              const next = value as MotionSpeed
              setMotionSpeed(next)
              writeStoredMotion(next)
              applyMotionSpeed(next)
            }}
          />
        } />
    </SettingsSection>
  )
}
