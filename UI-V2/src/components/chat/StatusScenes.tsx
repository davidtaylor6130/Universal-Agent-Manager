// Tiny illustrated scenes for the chat status divider. Pure SVG drawn in currentColor so every theme tints them;
// all motion lives in conversation.css and stops under prefers-reduced-motion.

/** Compacting: boxes ride a conveyor, get pressed flat, and roll on. */
export function CompactingScene() {
  return (
    <svg className="status-scene status-scene--compact" viewBox="0 0 48 20" width="48" height="20" aria-hidden>
      <rect className="status-scene__press" x="20" y="1" width="8" height="5" rx="1" />
      <line className="status-scene__piston" x1="24" y1="0" x2="24" y2="2" />
      <g className="status-scene__boxes">
        <rect className="status-scene__box status-scene__box--1" x="0" y="9" width="6" height="6" rx="1" />
        <rect className="status-scene__box status-scene__box--2" x="0" y="9" width="6" height="6" rx="1" />
        <rect className="status-scene__box status-scene__box--3" x="0" y="9" width="6" height="6" rx="1" />
      </g>
      <line className="status-scene__belt" x1="2" y1="16" x2="46" y2="16" />
      <g className="status-scene__rollers">
        {[7, 19, 31, 43].map((cx) => <circle key={cx} className="status-scene__roller" cx={cx} cy="18.4" r="1.9" />)}
      </g>
    </svg>
  )
}

/** One little agent at a desk: typing while it works, dozing once it is idle. */
function AgentAtDesk({ asleep, index }: { asleep: boolean; index: number }) {
  return (
    <g className={asleep ? 'status-agent status-agent--asleep' : 'status-agent'} transform={`translate(${index * 22} 0)`} style={{ '--agent-delay': `${index * 180}ms` } as React.CSSProperties}>
      <g className="status-agent__body">
        <line className="status-agent__antenna" x1="7" y1="2" x2="7" y2="4" />
        <circle className="status-agent__antenna-tip" cx="7" cy="1.6" r="0.9" />
        <rect className="status-agent__head" x="3.5" y="4" width="7" height="5.5" rx="2" />
        <circle className="status-agent__eye" cx="5.8" cy="6.8" r="0.7" />
        <circle className="status-agent__eye" cx="8.2" cy="6.8" r="0.7" />
        <path className="status-agent__closed" d="M5 6.9h1.6M7.4 6.9H9" />
        <rect className="status-agent__torso" x="4.5" y="10" width="5" height="4" rx="1.2" />
      </g>
      <g className="status-agent__laptop">
        <rect x="11" y="9" width="7" height="4.5" rx="0.8" />
        <rect className="status-agent__screen" x="12" y="10" width="5" height="2.5" rx="0.4" />
      </g>
      <line className="status-agent__desk" x1="1" y1="14.5" x2="20" y2="14.5" />
      <line className="status-agent__leg" x1="3" y1="14.5" x2="3" y2="19" />
      <line className="status-agent__leg" x1="18" y1="14.5" x2="18" y2="19" />
      <text className="status-agent__z" x="12" y="5">z</text>
    </g>
  )
}

/** Waiting on subagents: one desk per child (up to four), working or asleep. */
export function AgentsScene({ working, idle }: { working: number; idle: number }) {
  const desks = [...Array.from({ length: working }, () => false), ...Array.from({ length: idle }, () => true)].slice(0, 4)
  const shown = desks.length > 0 ? desks : [false]
  const width = shown.length * 22
  return (
    <svg className="status-scene status-scene--agents" viewBox={`0 0 ${width} 20`} width={width} height="20" aria-hidden>
      {shown.map((asleep, index) => <AgentAtDesk key={index} asleep={asleep} index={index} />)}
    </svg>
  )
}

/** Stopped after timeout: the AI has gone to bed. The blanket rises and falls; little z's drift up. */
export function SleepingScene() {
  return (
    <svg className="status-scene status-scene--sleep" viewBox="0 0 40 20" width="40" height="20" aria-hidden>
      <rect className="status-bed__headboard" x="2" y="6" width="3" height="10" rx="1" />
      <rect className="status-bed__pillow" x="5" y="9" width="8" height="4" rx="2" />
      <g className="status-bed__sleeper">
        <rect className="status-agent__head" x="6" y="6.5" width="6.5" height="5" rx="2" />
        <path className="status-agent__closed status-agent__closed--on" d="M7.4 9h1.4M9.8 9h1.4" />
        <line className="status-agent__antenna" x1="9.3" y1="4.5" x2="9.3" y2="6.5" />
        <circle className="status-agent__antenna-tip" cx="9.3" cy="4" r="0.9" />
      </g>
      <path className="status-bed__blanket" d="M12 11.5 Q24 8.5 36 11.5 L36 16 L12 16 Z" />
      <line className="status-bed__frame" x1="2" y1="16" x2="38" y2="16" />
      <line className="status-scene__leg" x1="4" y1="16" x2="4" y2="19" />
      <line className="status-scene__leg" x1="36" y1="16" x2="36" y2="19" />
      <text className="status-bed__z status-bed__z--1" x="16" y="7">z</text>
      <text className="status-bed__z status-bed__z--2" x="20" y="5">z</text>
    </svg>
  )
}
