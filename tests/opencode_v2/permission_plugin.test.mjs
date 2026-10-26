// Runs the exact C++-embedded plugin against the public V2 Promise API shape.
// No OpenCode executable, credentials, account directories or network is used.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const source = process.argv[2].endsWith('.cpp')
  ? readFileSync(process.argv[2], 'utf8').match(/R"UAMV2\(([\s\S]*?)\)UAMV2"/)[1]
  : execFileSync(process.argv[2], ['--plugin-source'], { encoding: 'utf8' });
const { default: plugin } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));
const directory = mkdtempSync(join(tmpdir(), 'uam-v2-policy-fixture-'));
const policyPath = join(directory, 'policy.json'), proofPath = join(directory, 'proof.json');
let hook;
const base = { nonce: 'fixture-nonce', active: true, sessionID: 'ses_owned', directory: '/workspace', readOnly: false };
function policy(patch = {}) { writeFileSync(policyPath, JSON.stringify({ ...base, ...patch })); }
async function evaluate(patch = {}, native = 'allow') {
  const event = { sessionID: 'ses_owned', action: 'read', resources: ['/workspace/file'], effect: native, ...patch };
  await hook(event);
  return event;
}
try {
  policy();
  await plugin.setup({ options: { policyPath, proofPath, nonce: 'fixture-nonce' }, location: { directory: '/workspace' }, permission: { hook: async (name, callback) => { assert.equal(name, 'evaluate'); hook = callback; } } });
  assert.deepEqual(JSON.parse(readFileSync(proofPath, 'utf8')), { nonce: 'fixture-nonce', directory: '/workspace' });
  assert.equal((await evaluate({}, 'allow')).effect, 'ask'); // saved project or session allow
  assert.equal((await evaluate({}, 'ask')).effect, 'ask');
  assert.equal((await evaluate({}, 'deny')).effect, 'deny');
  const late = await evaluate(); late.effect = 'allow'; assert.equal(late.effect, 'ask');
  late.effect = 'deny'; late.effect = 'allow'; assert.equal(late.effect, 'deny');
  assert.throws(() => Object.defineProperty(late, 'effect', { value: 'allow' }));
  policy({ readOnly: true });
  assert.equal((await evaluate({ action: 'edit' })).effect, 'deny');
  assert.equal((await evaluate({ action: 'task' })).effect, 'deny');
  assert.equal((await evaluate({ action: 'subagent' })).effect, 'deny');
  assert.equal((await evaluate({ action: 'shell' })).effect, 'ask'); // existing captured-command classifier decides
  assert.equal((await evaluate({ sessionID: 'ses_foreign' })).effect, 'deny');
  policy({ directory: '/foreign' }); assert.equal((await evaluate()).effect, 'deny');
  policy({ nonce: 'foreign' }); assert.equal((await evaluate()).effect, 'deny');
  policy({ active: false }); assert.equal((await evaluate()).effect, 'deny');
  writeFileSync(policyPath, '{malformed'); assert.equal((await evaluate()).effect, 'deny');
  console.log('PASS 15 process-local permission boundaries and load proof');
} finally { rmSync(directory, { recursive: true, force: true }); }
