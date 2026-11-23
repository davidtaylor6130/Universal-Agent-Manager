/** User allow/ask/deny rules for permission requests; the native matcher in command_rules.h is authoritative. */
export type PermissionRuleAction = 'allow' | 'ask' | 'deny'
export type PermissionRuleTool = 'any' | 'command' | 'readOnly' | 'edit' | 'fetch' | 'other'

export interface PermissionCommandRule {
  id: string
  action: PermissionRuleAction
  tool: PermissionRuleTool
  /** Command pattern; only used when tool is 'command'. */
  pattern: string
  /** Empty: every provider. */
  providerId: string
  /** Empty: every workspace; otherwise this folder and everything inside it. */
  workspace: string
  enabled: boolean
}

export interface PermissionRuleTestResult {
  outcome: 'allow' | 'ask' | 'deny' | 'none'
  ruleId: string
  reason: string
  readOnly: boolean
}

const ACTIONS: readonly PermissionRuleAction[] = ['allow', 'ask', 'deny']
const TOOLS: readonly PermissionRuleTool[] = ['any', 'command', 'readOnly', 'edit', 'fetch', 'other']

const text = (value: unknown, limit: number): string | undefined =>
  typeof value === 'string' && value.length <= limit && !/[\r\n\0]/.test(value) ? value.trim() : undefined

/** Drops malformed entries rather than repairing them into something broader. */
export function sanitizePermissionCommandRules(value: unknown): PermissionCommandRule[] {
  if (!Array.isArray(value)) return []
  const rules: PermissionCommandRule[] = []
  const ids = new Set<string>()
  for (const item of value.slice(0, 200)) {
    if (typeof item !== 'object' || item === null) continue
    const record = item as Record<string, unknown>
    const id = text(record.id, 128)
    const action = ACTIONS.find((candidate) => candidate === record.action)
    const tool = TOOLS.find((candidate) => candidate === record.tool)
    const pattern = text(record.pattern ?? '', 512)
    const providerId = text(record.providerId ?? '', 128)
    const workspace = text(record.workspace ?? '', 1024)
    if (!id || ids.has(id) || !action || !tool || pattern === undefined || providerId === undefined || workspace === undefined) continue
    ids.add(id)
    rules.push({ id, action, tool, pattern, providerId, workspace, enabled: typeof record.enabled === 'boolean' ? record.enabled : true })
  }
  return rules
}

/** Mirrors the native save checks so the editor can block an invalid rule before it is sent. */
export function permissionRuleError(rule: Pick<PermissionCommandRule, 'action' | 'tool' | 'pattern'> & { workspace?: string }): string {
  const pattern = rule.pattern.trim()
  const workspace = rule.workspace?.trim() ?? ''
  if (workspace && !/^(\/|\\\\|[A-Za-z]:[\\/])/.test(workspace)) return 'A workspace must be a full folder path.'
  if (rule.tool !== 'command' && pattern) return 'Only command rules take a pattern.'
  if (rule.tool === 'command') {
    if (!pattern) return 'A command rule needs a pattern.'
    if (/[\r\n]/.test(pattern)) return 'Rules must be a single line.'
    if (pattern.length > 512) return 'A command pattern can be at most 512 characters.'
    if (/(^|[^\\])(;|&|\||<|>|\(|\)|\$\(|`)/.test(pattern.replace(/'[^']*'/g, ''))) return 'A pattern is one command: no ;, &&, |, redirection or $( ).'
  }
  if (rule.action === 'allow' && (rule.tool === 'any' || rule.tool === 'fetch' || rule.tool === 'other')) return 'Allow rules can target commands, read-only work or file edits only.'
  if (rule.action === 'allow' && rule.tool === 'command') {
    const first = pattern.split(/\s+/)[0] ?? ''
    if (!first || first === '**' || /[*?[/\\]/.test(first)) return 'An allow pattern must start with a program name.'
  }
  return ''
}
