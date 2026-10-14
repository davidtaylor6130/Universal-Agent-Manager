import { useEffect, useMemo, useRef, useState, type KeyboardEvent, type ReactNode } from 'react'
import { DynamicIcon, iconNames } from 'lucide-react/dynamic'
import { SUGGESTED_LUCIDE_ICONS, readRecentLucideIcons } from '../../utils/lucideIconLibrary'

// ponytail: this module is loaded lazily (CustomIcon / picker), so the ~2000-name list and
// per-icon chunk loaders never reach the main bundle. Tags are not shipped by lucide-react; search is by name.
const KNOWN_ICONS = new Set<string>(iconNames)
const COLUMNS = 8
const RESULT_LIMIT = 96
const Placeholder = () => <span aria-hidden className="block" style={{ width: 16, height: 16 }} />

/** Renders a Lucide icon at 16px in currentColor; unknown names render the caller's fallback. */
export function LucideGlyph({ name, fallback }: { name: string; fallback: ReactNode }) {
  if (!KNOWN_ICONS.has(name)) return <>{fallback}</>
  return <DynamicIcon name={name as never} size={16} aria-hidden fallback={Placeholder} />
}

function IconCells({ label, names, selected, onSelect }: { label: string; names: readonly string[]; selected: string | null; onSelect: (name: string) => void }) {
  const [active, setActive] = useState(() => Math.max(0, names.indexOf(selected ?? '')))
  const cells = useRef<Array<HTMLButtonElement | null>>([])
  useEffect(() => setActive(0), [names])
  const onKeyDown = (event: KeyboardEvent<HTMLButtonElement>, index: number) => {
    const moves: Record<string, number> = { ArrowRight: index + 1, ArrowLeft: index - 1, ArrowDown: index + COLUMNS, ArrowUp: index - COLUMNS, Home: 0, End: names.length - 1 }
    const next = moves[event.key]
    if (next === undefined) return
    event.preventDefault()
    const target = Math.min(names.length - 1, Math.max(0, next))
    setActive(target)
    cells.current[target]?.focus()
  }
  return (
    <div role="group" aria-label={label} className="grid gap-1" style={{ gridTemplateColumns: `repeat(${COLUMNS}, 32px)` }}>
      {names.map((name, index) => (
        <button
          key={name}
          ref={(element) => { cells.current[index] = element }}
          type="button"
          aria-label={name}
          aria-pressed={name === selected}
          title={name}
          tabIndex={index === active ? 0 : -1}
          onFocus={() => setActive(index)}
          onClick={() => onSelect(name)}
          onKeyDown={(event) => onKeyDown(event, index)}
          className="uam-icon-cell"
          style={{
            color: name === selected ? 'var(--accent)' : 'var(--text-2)',
            background: name === selected ? 'var(--accent-dim)' : undefined,
            border: `1px solid ${name === selected ? 'var(--accent)' : 'transparent'}`,
          }}
        >
          <LucideGlyph name={name} fallback={null} />
        </button>
      ))}
    </div>
  )
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="flex flex-col gap-1.5">
      <h3 className="font-medium uppercase" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)', letterSpacing: '.08em' }}>{title}</h3>
      {children}
    </section>
  )
}

/** Searchable Lucide icon grid. Arrows move within a grid, Enter/Space select, search Enter picks the first match. */
export function LucideIconLibrary({ selected, onSelect }: { selected: string | null; onSelect: (name: string) => void }) {
  const [query, setQuery] = useState('')
  const gridsRef = useRef<HTMLDivElement>(null)
  const normalized = query.trim().toLowerCase().replace(/\s+/g, '-')
  const recent = useMemo(() => readRecentLucideIcons().filter((name) => KNOWN_ICONS.has(name)), [])
  const suggested = useMemo(() => SUGGESTED_LUCIDE_ICONS.filter((name) => KNOWN_ICONS.has(name)), [])
  const matches = useMemo(() => {
    if (!normalized) return []
    const all = iconNames.filter((name) => name.includes(normalized))
    return [...all.filter((name) => name.startsWith(normalized)), ...all.filter((name) => !name.startsWith(normalized))]
  }, [normalized])
  const results = matches.slice(0, RESULT_LIMIT)
  const focusGrid = () => gridsRef.current?.querySelector<HTMLButtonElement>('button[tabindex="0"]')?.focus()

  return (
    <div className="flex flex-col gap-3">
      <input
        type="search"
        aria-label="Search icons"
        placeholder="Search icons, e.g. folder"
        value={query}
        onChange={(event) => setQuery(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === 'ArrowDown') { event.preventDefault(); focusGrid() }
          if (event.key === 'Enter' && results[0]) { event.preventDefault(); onSelect(results[0]) }
        }}
        className="w-full rounded-lg px-3 py-2 text-sm outline-none"
        style={{ background: 'var(--bg)', color: 'var(--text)', border: '1px solid var(--border)' }}
      />
      <div ref={gridsRef} className="flex max-h-[232px] flex-col gap-3 overflow-y-auto pr-1">
        {normalized ? (
          results.length > 0 ? (
            <>
              <IconCells label={`Results for ${query.trim()}`} names={results} selected={selected} onSelect={onSelect} />
              {matches.length > results.length && <p className="" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)' }}>Showing {results.length} of {matches.length}. Refine the search to see more.</p>}
            </>
          ) : (
            <p role="status" className="py-4 text-center text-xs" style={{ color: 'var(--text-3)' }}>No icons match “{query.trim()}”.</p>
          )
        ) : (
          <>
            {recent.length > 0 && <Section title="Recently used"><IconCells label="Recently used icons" names={recent} selected={selected} onSelect={onSelect} /></Section>}
            <Section title="Suggested"><IconCells label="Suggested icons" names={suggested} selected={selected} onSelect={onSelect} /></Section>
          </>
        )}
      </div>
    </div>
  )
}
