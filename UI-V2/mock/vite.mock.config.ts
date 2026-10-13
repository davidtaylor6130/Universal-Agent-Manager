import { mergeConfig } from 'vite'
import base from '../vite.config'

// Review-only dev server for /mock.html. node_modules may be a symlink outside the worktree, so relax file serving.
export default mergeConfig(base, { server: { port: 5199, strictPort: true, open: '/mock.html', fs: { strict: false } } })
