import { Button, MenuSelect } from '../ui'
import { MONO_FONTS, SANS_FONTS, useTypographyStore, type TypographyPreferences } from '../../store/typographyStore'
import { NumberField } from './NumberField'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

export function TypographySettings() {
  const preferences = useTypographyStore((state) => state.preferences)
  const update = useTypographyStore((state) => state.update)
  const reset = useTypographyStore((state) => state.reset)
  const rows = [
    { id: 'interface', label: 'Chats, settings and workspace lists', fonts: SANS_FONTS, min: 12, max: 20 },
    { id: 'prompt', label: 'Message input', fonts: SANS_FONTS, min: 12, max: 20 },
    { id: 'code', label: 'Code blocks and diffs in messages', fonts: MONO_FONTS, min: 10, max: 18 },
    { id: 'terminal', label: 'CLI fallback terminal', fonts: MONO_FONTS, min: 8, max: 20 },
  ] as const
  return (
    <div className="grid gap-6">
      <SettingsSection title="Fonts and sizes"
        actions={<Button size="sm" onClick={reset}>Reset typography</Button>}>
        {rows.map((row) => {
          const fontKey = `${row.id}Font` as keyof TypographyPreferences
          const sizeKey = `${row.id}Size` as keyof TypographyPreferences
          const selected = preferences[fontKey] as string
          const size = preferences[sizeKey] as number
          return (
            <SettingRow key={row.id} title={row.label}
              control={<div className="flex items-center gap-2">
                <div style={{ width: 170 }}>
                  <MenuSelect label={`${row.label} font`} value={selected}
                    options={Object.entries(row.fonts).map(([id, font]) => ({ value: id, label: font.label }))}
                    onChange={(value) => update({ [fontKey]: value })} />
                </div>
                <div style={{ width: 96 }}>
                  <NumberField label={`${row.label} font size`} value={size} min={row.min} max={row.max} unit="px"
                    onCommit={(value) => update({ [sizeKey]: value })} />
                </div>
              </div>} />
          )
        })}
      </SettingsSection>
    </div>
  )
}
