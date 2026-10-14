import { Button, MenuSelect } from '../ui'
import { MONO_FONTS, SANS_FONTS, useTypographyStore, type TypographyPreferences } from '../../store/typographyStore'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

export function TypographySettings() {
  const preferences = useTypographyStore((state) => state.preferences)
  const update = useTypographyStore((state) => state.update)
  const reset = useTypographyStore((state) => state.reset)
  const rows = [
    { id: 'interface', label: 'Interface', detail: 'Chats, settings and workspace lists.', fonts: SANS_FONTS, min: 12, max: 20, preview: 'Chats · Settings · Workspaces' },
    { id: 'prompt', label: 'Message input', detail: 'The text you type into a chat.', fonts: SANS_FONTS, min: 12, max: 20, preview: 'What would you like to work on?' },
    { id: 'code', label: 'Code', detail: 'Code blocks and diffs in messages.', fonts: MONO_FONTS, min: 10, max: 18, preview: 'const message = "Hello, David";' },
    { id: 'terminal', label: 'Terminal', detail: 'The CLI fallback terminal.', fonts: MONO_FONTS, min: 8, max: 20, preview: '$ npm run build' },
  ] as const
  return (
    <div className="grid gap-6">
      <SettingsSection title="Fonts and sizes"
        actions={<Button size="sm" onClick={reset}>Reset typography</Button>}>
        {rows.map((row) => {
          const fontKey = `${row.id}Font` as keyof TypographyPreferences
          const sizeKey = `${row.id}Size` as keyof TypographyPreferences
          const selected = preferences[fontKey] as string
          const family = (row.fonts as Record<string, { family: string }>)[selected].family
          const size = preferences[sizeKey] as number
          return (
            <SettingRow key={row.id} title={row.label} description={row.detail}
              control={<div className="flex items-center gap-2">
                <div style={{ width: 170 }}>
                  <MenuSelect label={`${row.label} font`} value={selected}
                    options={Object.entries(row.fonts).map(([id, font]) => ({ value: id, label: font.label }))}
                    onChange={(value) => update({ [fontKey]: value })} />
                </div>
                <input type="number" aria-label={`${row.label} font size`} title="Size in pixels" min={row.min} max={row.max} step={1}
                  value={size} onChange={(event) => { if (event.target.value) update({ [sizeKey]: event.target.valueAsNumber }) }}
                  className="uam-field" style={{ width: 64 }} />
              </div>}>
              <div className="uam-typo-row__preview" aria-label={`${row.label} preview`} style={{ fontFamily: family, fontSize: size }}>
                {row.preview}
              </div>
            </SettingRow>
          )
        })}
      </SettingsSection>
    </div>
  )
}
