import { Button } from '../ui'
import { MONO_FONTS, SANS_FONTS, useTypographyStore, type TypographyPreferences } from '../../store/typographyStore'

export function TypographySettings() {
  const preferences = useTypographyStore((state) => state.preferences)
  const update = useTypographyStore((state) => state.update)
  const reset = useTypographyStore((state) => state.reset)
  const rows = [
    { id: 'interface', label: 'Interface', fonts: SANS_FONTS, min: 12, max: 20, preview: 'Chats · Settings · Workspaces' },
    { id: 'prompt', label: 'Message input', fonts: SANS_FONTS, min: 12, max: 20, preview: 'What would you like to work on?' },
    { id: 'code', label: 'Code', fonts: MONO_FONTS, min: 10, max: 18, preview: 'const message = "Hello, David";' },
    { id: 'terminal', label: 'Terminal', fonts: MONO_FONTS, min: 8, max: 20, preview: '$ npm run build' },
  ] as const
  return (
    <div className="space-y-4">
      {rows.map((row) => {
        const fontKey = `${row.id}Font` as keyof TypographyPreferences
        const sizeKey = `${row.id}Size` as keyof TypographyPreferences
        const selected = preferences[fontKey] as string
        const family = Object.entries(row.fonts).find(([id]) => id === selected)![1].family
        return (
          <div key={row.id} className="border-b pb-4" style={{ borderColor: 'var(--border)' }}>
            <div className="flex flex-wrap items-center gap-3">
              <label htmlFor={`typography-${row.id}-font`} className="min-w-[120px] flex-1 text-sm font-medium">{row.label}</label>
              <select id={`typography-${row.id}-font`} aria-label={`${row.label} font`} value={selected}
                onChange={(event) => update({ [fontKey]: event.target.value })}
                className="uam-field uam-field--select" style={{ width: 180, background: '#000', color: 'var(--text)' }}>
                {Object.entries(row.fonts).map(([id, font]) => <option key={id} value={id}>{font.label}</option>)}
              </select>
              <label className="flex items-center gap-2 text-sm">
                Size
                <input type="number" aria-label={`${row.label} font size`} min={row.min} max={row.max} step={1}
                  value={preferences[sizeKey]} onChange={(event) => { if (event.target.value) update({ [sizeKey]: event.target.valueAsNumber }) }}
                  className="uam-field" style={{ width: 68, background: '#000', color: 'var(--text)' }} />
              </label>
            </div>
            <div className="mt-3 overflow-x-auto" style={{ fontFamily: family, fontSize: preferences[sizeKey] as number, color: 'var(--text)', whiteSpace: 'pre' }}>
              {row.preview}
            </div>
          </div>
        )
      })}
      <Button onClick={reset}>Reset typography</Button>
    </div>
  )
}
