export interface UsageGaugeProps {
  /** Percentage used (0–100, clamped); null when unknown or unavailable. */
  usedPercent: number | null
  /** Rendered width in px; height follows the half-dial. 20px wide gives the same footprint as a 14px icon. */
  size?: number
  /** Hide from assistive technology when an enclosing control's accessible name already carries the value (for example inside a button, whose children are presentational). */
  decorative?: boolean
}

const C = 12, R = 9
const point = (p: number) => {
  const a = Math.PI * (1 - p / 100)
  return `${(C + R * Math.cos(a)).toFixed(2)} ${(C - R * Math.sin(a)).toFixed(2)}`
}
const arc = (from: number, to: number) => `M ${point(from)} A ${R} ${R} 0 0 1 ${point(to)}`
/** Equal thirds: green 0–33, orange 33–66, red 66–100. --meter-warning keeps 3:1 graphic contrast in light themes. */
export const USAGE_GAUGE_BANDS = [[0, 100 / 3, 'var(--success)'], [100 / 3, 200 / 3, 'var(--meter-warning)'], [200 / 3, 100, 'var(--error)']] as const
/** Needle rotation in degrees from straight up: 0% → -90, 50% → 0, 100% → 90. */
export const usageGaugeRotation = (usedPercent: number) => (Math.min(100, Math.max(0, usedPercent)) - 50) * 1.8

/** Half-dial usage meter: three semantic bands and a needle at the used percentage; unknown shows a neutral arc without a needle. */
export function UsageGauge({ usedPercent, size = 20, decorative = false }: UsageGaugeProps) {
  const known = usedPercent !== null && Number.isFinite(usedPercent)
  const used = known ? Math.round(Math.min(100, Math.max(0, usedPercent))) : null
  const meter = used === null || decorative ? { 'aria-hidden': true } : { role: 'meter', 'aria-valuemin': 0, 'aria-valuemax': 100, 'aria-valuenow': used, 'aria-valuetext': `${used}% used` }
  return <svg {...meter} data-usage-gauge viewBox="0 0 24 14" width={size} height={size * 14 / 24} fill="none" strokeWidth={2.5} className="uam-usage-gauge shrink-0">
    {used === null
      ? <path d={arc(0, 100)} stroke="var(--text-2)" />
      : <>
        {USAGE_GAUGE_BANDS.map(([from, to, tone]) => <path key={tone} data-band={tone} d={arc(from, to)} stroke={tone} />)}
        <g data-needle className="uam-usage-gauge__needle" style={{ transform: `rotate(${usageGaugeRotation(used)}deg)`, transformOrigin: `${C}px ${C}px`, transformBox: 'view-box', transition: 'transform var(--dur-base) var(--ease-out-soft)' }}>
          <line x1={C} y1={C} x2={C} y2={C - R + 1} stroke="var(--text)" strokeWidth={1.5} strokeLinecap="round" />
          <circle cx={C} cy={C} r={1.5} fill="var(--text)" stroke="none" />
        </g>
      </>}
  </svg>
}
