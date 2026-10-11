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
    <div className="uam-typography-settings">
      <section aria-label="Fonts and sizes" className="uam-settings-group">
        <h3 className="uam-settings-group__title">Fonts and sizes</h3>
        <div className="uam-settings-group__body">
          {rows.map((row) => {
            const fontKey = `${row.id}Font` as keyof TypographyPreferences
            const sizeKey = `${row.id}Size` as keyof TypographyPreferences
            const selected = preferences[fontKey] as string
            const family = (row.fonts as Record<string, { family: string }>)[selected].family
            const size = preferences[sizeKey] as number
            return (
              <div key={row.id} className="uam-typo-row">
                <div className="flex flex-wrap items-center gap-3">
                  <label htmlFor={`typography-${row.id}-font`} className="min-w-[120px] flex-1 text-sm font-medium">{row.label}</label>
                  <select id={`typography-${row.id}-font`} aria-label={`${row.label} font`} value={selected}
                    onChange={(event) => update({ [fontKey]: event.target.value })}
                    className="uam-field uam-field--select" style={{ width: 180 }}>
                    {Object.entries(row.fonts).map(([id, font]) => <option key={id} value={id}>{font.label}</option>)}
                  </select>
                  <label className="flex items-center gap-2 text-xs" style={{ color: 'var(--text-2)' }}>
                    Size
                    <input type="number" aria-label={`${row.label} font size`} min={row.min} max={row.max} step={1}
                      value={size} onChange={(event) => { if (event.target.value) update({ [sizeKey]: event.target.valueAsNumber }) }}
                      className="uam-field" style={{ width: 68 }} />
                  </label>
                </div>
                <div className="uam-typo-row__preview" aria-label={`${row.label} preview`} style={{ fontFamily: family, fontSize: size }}>
                  {row.preview}
                </div>
                <span className="uam-typo-row__meta">{(row.fonts as Record<string, { label: string }>)[selected]?.label} · {size}px</span>
              </div>
            )
          })}
        </div>
      </section>
      <div className="flex justify-end">
        <Button size="sm" onClick={reset}>Reset typography</Button>
      </div>
    </div>
  )
}
