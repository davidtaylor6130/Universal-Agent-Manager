import { act, type ReactNode } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { SubagentRouteStage, freezeOutgoing, stepSpring, springSettled, subagentStageDirection, subagentStageKey } from './SubagentRouteStage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
let frames: FrameRequestCallback[]
let requested: number
let clones: ReturnType<typeof vi.spyOn>
// jsdom does not resolve var(--dur-*); the stage's duration list stands in for the resolved shared tokens.
let tokens = '0.24s, 0.17s'
beforeEach(() => {
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host); frames = []; requested = 0; tokens = '0.24s, 0.17s'
  vi.spyOn(window, 'requestAnimationFrame').mockImplementation(callback => { frames.push(callback); return ++requested })
  vi.spyOn(window, 'cancelAnimationFrame').mockImplementation(() => {})
  window.matchMedia = vi.fn().mockReturnValue({ matches: false }) as never
  const computed = window.getComputedStyle.bind(window)
  vi.spyOn(window, 'getComputedStyle').mockImplementation((element, pseudo) => {
    const style = computed(element, pseudo)
    return (element as HTMLElement).style?.transitionProperty === 'none'
      ? new Proxy(style, { get: (target, key) => key === 'transitionDuration' ? tokens : Reflect.get(target, key) }) : style
  })
  clones = vi.spyOn(Node.prototype, 'cloneNode')
})
afterEach(() => { act(() => root.unmount()); host.remove(); document.documentElement.removeAttribute('data-motion'); vi.restoreAllMocks() })

const view = (key: string, label: string, extra: ReactNode = null) => <SubagentRouteStage routeKey={key}>
  <div id={`view-${label}`} data-testid={`view-${label}`}>
    <textarea defaultValue={`${label} private draft`} autoFocus />
    <button onClick={() => {}}>{label} action</button>
    <a href="#x">{label} link</a>
    <div className="scroller" style={{ overflow: 'auto' }}><p>{label} text</p></div>
    {extra}
  </div>
</SubagentRouteStage>
const ghosts = () => [...host.querySelectorAll<HTMLElement>('[data-subagent-stage-copy]')]
const live = (label: string) => host.querySelector<HTMLElement>(`#view-${label}`)!.closest<HTMLElement>('[style*="z-index: 1"]')!
let clock = 0
const flush = (ms: number) => { clock = Math.max(clock, performance.now()) + ms; const queued = frames.splice(0); act(() => queued.forEach(frame => frame(clock))) }
const settle = () => { for (let i = 0; i < 240 && frames.length; i++) flush(16) }

describe('subagent route stage', () => {
  it('pushes only into a child of the shown parent and pops only back to that parent', () => {
    const child = subagentStageKey('p', 'c')
    expect(subagentStageDirection('p', child)).toBe(1)
    expect(subagentStageDirection(child, subagentStageKey('p', 'd'))).toBe(1)
    expect(subagentStageDirection(child, 'p')).toBe(-1)
    expect(subagentStageDirection('p', 'q')).toBe(0)
    expect(subagentStageDirection(child, 'q')).toBe(0)
    expect(subagentStageDirection('q', child)).toBe(0)
  })

  it('settles the default-token spring within a second without visible overshoot; Back is quicker', () => {
    const run = (response: number) => {
      let state = { p: 1, v: 0 }, elapsed = 0, lowest = 1
      while (!springSettled(state) && elapsed < 2) { state = stepSpring(state, 1 / 60, response); elapsed += 1 / 60; lowest = Math.min(lowest, state.p) }
      return { elapsed, lowest }
    }
    const push = run(0.24 * 1.75), pop = run(0.17 * 1.75)
    expect(push.elapsed).toBeLessThan(1)
    expect(push.lowest).toBeGreaterThan(-0.01)
    expect(pop.elapsed).toBeLessThan(push.elapsed)
  })

  it('moves a static, inert, id-free copy whose controls are geometry placeholders without labels, values or links', () => {
    act(() => root.render(view('p', 'parent')))
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    expect(ghosts()).toHaveLength(1)
    const ghost = ghosts()[0]
    expect(ghost.getAttribute('aria-hidden')).toBe('true')
    expect(ghost.hasAttribute('inert')).toBe(true)
    expect(ghost.querySelector('button, textarea, input, select, a, summary, [id], [data-testid], [href], [autofocus], [tabindex]')).toBeNull()
    expect([...ghost.querySelectorAll('*')].some(element => element.getAttributeNames().some(name => name.startsWith('on')))).toBe(false)
    expect(ghost.textContent).toContain('parent text')
    expect(ghost.textContent).not.toContain('private draft')
    expect(ghost.textContent).not.toContain('parent action')
    expect(ghost.querySelectorAll('.uam-subagent-stage__placeholder')).toHaveLength(3)
    expect(host.querySelectorAll('#view-parent')).toHaveLength(0) // the parent React tree is gone, not hidden
    expect(live('child').style.transform).toMatch(/translate3d/)
    expect(document.activeElement).not.toBe(ghost.querySelector('*'))
  })

  it('restores copied scroll offsets on the moving copy', () => {
    act(() => root.render(view('p', 'parent')))
    const scroller = host.querySelector<HTMLElement>('.scroller')!
    Object.defineProperty(scroller, 'scrollTop', { value: 140, configurable: true, writable: true })
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    expect(ghosts()[0].querySelector<HTMLElement>('.scroller')!.scrollTop).toBe(140)
  })

  it('reverses mid-flight with one copy, keeps velocity direction, and lands without leftover transforms or frames', () => {
    act(() => root.render(view('p', 'parent')))
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    flush(60)
    act(() => root.render(view('p', 'parent')))
    expect(ghosts()).toHaveLength(1)
    expect(ghosts()[0].textContent).toContain('child text')
    settle()
    expect(ghosts()).toHaveLength(0)
    expect(frames).toHaveLength(0)
    expect(live('parent').style.transform).toBe('')
  })

  it.each(['off', 'reduced'])('allocates no copy and schedules no frame when motion is %s, including unrelated changes', mode => {
    if (mode === 'off') { document.documentElement.setAttribute('data-motion', 'off'); tokens = '0s, 0s' }
    else window.matchMedia = vi.fn().mockReturnValue({ matches: true }) as never
    act(() => root.render(view('p', 'parent')))
    clones.mockClear()
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    act(() => root.render(view('p', 'parent')))
    act(() => root.render(view('q', 'other')))
    expect(clones).not.toHaveBeenCalled()
    expect(ghosts()).toHaveLength(0)
    expect(requested).toBe(0)
  })

  it('does not copy or animate unrelated chat switches', () => {
    act(() => root.render(view('p', 'parent')))
    clones.mockClear()
    act(() => root.render(view('q', 'other')))
    expect(clones).not.toHaveBeenCalled()
    expect(requested).toBe(0)
  })

  it('lands within one frame when motion is turned off or an overlay opens mid-flight', () => {
    act(() => root.render(view('p', 'parent')))
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    flush(16)
    document.documentElement.setAttribute('data-motion', 'off')
    flush(16)
    expect(ghosts()).toHaveLength(0)
    expect(frames).toHaveLength(0)
    expect(live('child').style.transform).toBe('')
    document.documentElement.removeAttribute('data-motion')
    act(() => root.render(view('p', 'parent')))
    flush(16)
    act(() => root.render(view('p', 'parent', <div role="dialog">Open overlay</div>)))
    flush(16)
    expect(ghosts()).toHaveLength(0)
    expect(frames).toHaveLength(0)
    expect(host.querySelector('[role="dialog"]')!.textContent).toBe('Open overlay') // the user's overlay is untouched
  })

  it('changes instantly, with no blank copy, when the outgoing view holds a canvas or an open overlay', () => {
    act(() => root.render(view('p', 'parent', <canvas />)))
    clones.mockClear()
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    expect(clones).not.toHaveBeenCalled()
    expect(requested).toBe(0)
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child', <div className="uam-overlay fixed">Menu</div>)))
    act(() => root.render(view('p', 'parent')))
    expect(clones).not.toHaveBeenCalled()
    expect(host.querySelector('#view-parent')).not.toBeNull()
  })

  it('removes its copy and stops frames when the stage unmounts mid-flight', () => {
    act(() => root.render(view('p', 'parent')))
    act(() => root.render(view(subagentStageKey('p', 'c'), 'child')))
    const ghost = ghosts()[0]
    act(() => root.render(<div />))
    expect(ghost.isConnected).toBe(false)
    const queued = frames.splice(0)
    act(() => queued.forEach(frame => frame(performance.now() + 16)))
    expect(frames).toHaveLength(0)
  })

  it('freezeOutgoing never copies iframe, media or canvas content', () => {
    for (const tag of ['canvas', 'iframe', 'video', 'audio', 'embed']) {
      const source = document.createElement('div'); source.appendChild(document.createElement(tag))
      expect(freezeOutgoing(source)).toBeNull()
    }
  })
})
