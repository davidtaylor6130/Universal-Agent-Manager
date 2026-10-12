// Dev-only demo of the illustrated chat status divider. Open /status-demo.html on the Vite dev server.
import { StrictMode, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../index.css'
import '../components/chat/conversation.css'
import { ChatStatusStrip } from '../components/chat/StatusHelpers'
import { AgentsScene, CompactingScene, SleepingScene } from '../components/chat/StatusScenes'
import { ContextUsageStatus } from '../components/chat/StatusHelpers'

const THEMES = ['dark', 'light', 'focus', 'midnight', 'paper', 'dusk', 'aurora', 'contrast']

function Demo() {
  const [theme, setTheme] = useState('dark')
  const [compacting, setCompacting] = useState(true)
  const [waiting, setWaiting] = useState(true)
  const [timedOut, setTimedOut] = useState(true)
  const [working, setWorking] = useState(2)
  const [idle, setIdle] = useState(1)
  const [scale, setScale] = useState(1)
  document.documentElement.setAttribute('data-theme', theme)
  const button = (label: string, on: boolean, toggle: () => void) => <button type="button" className={`uam-btn uam-btn--sm ${on ? 'uam-btn--primary' : 'uam-btn--secondary'}`} onClick={toggle}>{label}: {on ? 'on' : 'off'}</button>
  return (
    <div style={{ minHeight: '100vh', background: 'var(--bg)', color: 'var(--text)', fontFamily: 'var(--font-sans)', padding: 32 }}>
      <div style={{ maxWidth: 760, margin: '0 auto', display: 'grid', gap: 20 }}>
        <h1 style={{ fontSize: 18, fontWeight: 600 }}>Chat status divider demo</h1>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8, alignItems: 'center' }}>
          <select className="uam-field" value={theme} onChange={(event) => setTheme(event.target.value)} style={{ width: 140 }}>{THEMES.map((value) => <option key={value}>{value}</option>)}</select>
          {button('Compacting', compacting, () => setCompacting((value) => !value))}
          {button('Waiting on agents', waiting, () => setWaiting((value) => !value))}
          {button('Timed out', timedOut, () => setTimedOut((value) => !value))}
          <label style={{ fontSize: 12, color: 'var(--text-2)' }}>Working <input type="number" min={0} max={4} value={working} onChange={(event) => setWorking(+event.target.value)} style={{ width: 44 }} className="uam-field" /></label>
          <label style={{ fontSize: 12, color: 'var(--text-2)' }}>Asleep <input type="number" min={0} max={4} value={idle} onChange={(event) => setIdle(+event.target.value)} style={{ width: 44 }} className="uam-field" /></label>
          <label style={{ fontSize: 12, color: 'var(--text-2)' }}>Zoom <input type="range" min={1} max={4} step={0.5} value={scale} onChange={(event) => setScale(+event.target.value)} /> {scale}×</label>
        </div>
        <p style={{ fontSize: 12, color: 'var(--text-3)' }}>Toggle a state off and on to see the spring entrance and exit. Turn on “Reduce motion” in macOS Accessibility settings to see the still versions.</p>

        <div style={{ zoom: scale, display: 'grid', gap: 12, padding: '24px 0', borderTop: '1px solid var(--border)' }}>
          <details className="context-compaction" open={false}><summary>Context compacted · 164k → 48k tokens</summary><div>Earlier messages were summarised.</div></details>
          <div style={{ fontSize: 13, color: 'var(--text-2)', padding: '0 4px' }}>Done. I grouped the provider rows by connection status and moved the advanced options behind a disclosure.</div>
          <ChatStatusStrip open={timedOut} tone="warning" scene={<SleepingScene />} title="Stopped after timeout" />
          <ChatStatusStrip open={compacting} tone="info" scene={<CompactingScene />} title="Compacting context" detail={<ContextUsageStatus usage={{ usedTokens: 152000, windowTokens: 200000, updatedAt: Date.now() }} />} />
          <ChatStatusStrip open={waiting} tone="agents" scene={<AgentsScene working={working} idle={idle} />} title={working > 1 ? `Waiting on ${working} subagents` : 'Waiting on subagents'} />
          <div style={{ height: 44, borderRadius: 12, border: '1px solid var(--border)', background: 'var(--surface)', display: 'flex', alignItems: 'center', padding: '0 14px', fontSize: 13, color: 'var(--text-3)' }}>Message Claude · / for commands</div>
        </div>
      </div>
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<StrictMode><Demo /></StrictMode>)
