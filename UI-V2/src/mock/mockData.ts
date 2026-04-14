import { Folder, Session } from '../types/session'
import { Message } from '../types/message'
import { Provider, ProviderFeature } from '../types/provider'

export const initialFolders: Folder[] = [
  {
    id: 'f1',
    name: 'Work Projects',
    parentId: null,
    directory: '/workspace/work-projects',
    isExpanded: true,
    createdAt: new Date('2026-03-10'),
  },
  {
    id: 'f2',
    name: 'Research',
    parentId: null,
    directory: '/workspace/research',
    isExpanded: false,
    createdAt: new Date('2026-03-15'),
  },
]

export const initialSessions: Session[] = [
  {
    id: 's1',
    name: 'API Design Review',
    viewMode: 'structured',
    folderId: 'f1',
    createdAt: new Date('2026-04-01'),
    updatedAt: new Date('2026-04-08'),
  },
  {
    id: 's2',
    name: 'Build System Debug',
    viewMode: 'cli',
    folderId: 'f1',
    createdAt: new Date('2026-04-05'),
    updatedAt: new Date('2026-04-09'),
  },
  {
    id: 's3',
    name: 'Refactor Auth Module',
    viewMode: 'coding-agent',
    folderId: 'f1',
    createdAt: new Date('2026-04-07'),
    updatedAt: new Date('2026-04-09'),
  },
  {
    id: 's4',
    name: 'ML Pipeline Notes',
    viewMode: 'structured',
    folderId: 'f2',
    createdAt: new Date('2026-03-28'),
    updatedAt: new Date('2026-04-02'),
  },
]

export const initialMessages: Record<string, Message[]> = {
  s1: [
    {
      id: 'm1',
      sessionId: 's1',
      role: 'user',
      content:
        'Review this REST endpoint design:\n\n```json\n{\n  "POST /api/sessions": {\n    "body": { "name": "string", "model": "string" },\n    "response": { "id": "uuid", "status": "created" }\n  }\n}\n```\n\nDoes it follow best practices?',
      createdAt: new Date('2026-04-08T10:02:00'),
    },
    {
      id: 'm2',
      sessionId: 's1',
      role: 'assistant',
      content:
        "The endpoint design is clean. A few recommendations:\n\n**1. API Versioning**\nPrefix with `/api/v1/sessions` to allow future evolution without breaking clients.\n\n**2. Response Envelope**\nWrap responses for consistency:\n```json\n{\n  \"data\": { \"id\": \"uuid\", \"status\": \"created\" },\n  \"meta\": { \"timestamp\": \"2026-04-08T10:02:00Z\" }\n}\n```\n\n**3. Idempotency Key**\nAccept an `X-Idempotency-Key` header on POST to prevent duplicate session creation on retry.\n\n**4. Status Codes**\nReturn `201 Created` not `200 OK` for resource creation. Include a `Location` header pointing to the new resource.\n\nOverall solid foundation — just add versioning and the envelope pattern before shipping.",
      createdAt: new Date('2026-04-08T10:02:08'),
    },
    {
      id: 'm3',
      sessionId: 's1',
      role: 'user',
      content:
        'Good points. Should I use UUID v4 or v7 for session IDs?',
      createdAt: new Date('2026-04-08T10:05:00'),
    },
    {
      id: 'm4',
      sessionId: 's1',
      role: 'assistant',
      content:
        "**UUID v7** for session IDs — here's why:\n\n- **Time-ordered**: v7 encodes a millisecond timestamp prefix, making IDs naturally sortable by creation time\n- **Database performance**: Monotonically increasing IDs avoid page splits in B-tree indexes (a major issue with random v4 UUIDs at scale)\n- **Debugging**: You can decode the creation timestamp directly from the ID\n\nUUID v4 is fine for small datasets but will cause index fragmentation as your sessions table grows.\n\n```ts\nimport { uuidv7 } from 'uuidv7'\nconst sessionId = uuidv7() // '018f8e2a-b1c3-7d00-8000-abc123def456'\n```\n\nSwitch to v7 — you won't regret it.",
      createdAt: new Date('2026-04-08T10:05:06'),
    },
  ],
  s4: [
    {
      id: 'm5',
      sessionId: 's4',
      role: 'user',
      content:
        'What preprocessing steps should I apply to the time series sensor data before feeding it into a transformer model?',
      createdAt: new Date('2026-04-02T14:20:00'),
    },
    {
      id: 'm6',
      sessionId: 's4',
      role: 'assistant',
      content:
        "For sensor time series going into a transformer, the key steps:\n\n**Normalization**\nZ-score normalize per channel (zero mean, unit variance). Fit on training split only to prevent data leakage.\n\n**Missing Values**\n- Short gaps (< 5 samples): forward-fill\n- Longer gaps: linear interpolation or flag with a mask token\n\n**Windowing**\nSliding windows with configurable stride:\n```python\ndef make_windows(arr, window=512, stride=128):\n    return np.lib.stride_tricks.sliding_window_view(arr, window)[::stride]\n```\n\n**Resampling**\nIf sensor rates differ, resample all channels to a common frequency before windowing.\n\n**Detrending**\nOnly if your model doesn't include positional bias correction — otherwise leave trends in.\n\nStart with z-score + forward-fill + windowing. Add the rest if performance is lacking.",
      createdAt: new Date('2026-04-02T14:20:10'),
    },
  ],
}

export const initialProviders: Provider[] = [
  {
    id: 'p-claude',
    name: 'Claude Sonnet 4.6',
    shortName: 'Claude',
    color: '#f97316',
    description: 'Anthropic — best for reasoning and code',
  },
  {
    id: 'p-gpt4o',
    name: 'GPT-4o',
    shortName: 'GPT-4o',
    color: '#3b82f6',
    description: 'OpenAI — strong general-purpose model',
  },
  {
    id: 'p-ollama',
    name: 'Ollama (local)',
    shortName: 'Local',
    color: '#4ade80',
    description: 'On-device — fully private, no network',
  },
]

export const defaultFeatures: ProviderFeature[] = [
  { id: 'feat-tools', label: 'Tools', enabled: true, icon: '⚙' },
  { id: 'feat-web', label: 'Web', enabled: false, icon: '⌁' },
  { id: 'feat-memory', label: 'Memory', enabled: true, icon: '◈' },
  { id: 'feat-files', label: 'Files', enabled: false, icon: '⊡' },
]

export type AgentStepStatus = 'done' | 'running' | 'pending' | 'error'

export interface AgentStep {
  id: string
  label: string
  status: AgentStepStatus
  output?: string
  durationMs?: number
}

export const mockAgentSteps: Record<string, AgentStep[]> = {
  s3: [
    {
      id: 'step-1',
      label: 'Analyze repository structure',
      status: 'done',
      output: 'Found 847 files across 12 modules. Auth module: src/auth/ (23 files).',
      durationMs: 1240,
    },
    {
      id: 'step-2',
      label: 'Read auth/middleware.ts',
      status: 'done',
      output: 'Identified deprecated `sessionStore.set()` pattern on lines 34, 67, 102.',
      durationMs: 380,
    },
    {
      id: 'step-3',
      label: 'Read auth/token-manager.ts',
      status: 'done',
      output: 'Token stored in plain localStorage — flagged for secure storage migration.',
      durationMs: 290,
    },
    {
      id: 'step-4',
      label: 'Refactor token storage to HttpOnly cookies',
      status: 'running',
      output: 'Rewriting tokenManager.set() … (14 / 23 files updated)',
    },
    {
      id: 'step-5',
      label: 'Update unit tests',
      status: 'pending',
    },
    {
      id: 'step-6',
      label: 'Run linter and type-check',
      status: 'pending',
    },
    {
      id: 'step-7',
      label: 'Generate change summary',
      status: 'pending',
    },
  ],
}
