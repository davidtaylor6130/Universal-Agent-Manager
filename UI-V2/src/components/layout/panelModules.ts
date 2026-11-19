// Panel code is split out of the shell. Hover or focus on a panel's trigger starts the same import that
// React.lazy uses, so opening it rarely waits on the chunk. import() is cached, so repeats cost nothing.
// ponytail: no dwell or cancel; a pass across the rail loads at most these few local chunks once.
export const loadSettingsPanel = () => import('../settings/SettingsModal')
export const loadUsagePanel = () => import('../usage/UsagePage')
export const loadNewChatPanel = () => import('../sidebar/NewChatModal')

/** Trigger props that warm a panel's code without opening it. A failed warm-up is left to the real open. */
export function warmPanelOn(load: () => Promise<unknown>) {
  const warm = () => { load().catch(() => {}) }
  return { onPointerEnter: warm, onFocus: warm }
}
