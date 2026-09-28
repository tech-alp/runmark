#!/bin/sh
set -eu
plugin_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec node - "$plugin_root" <<'NODE'
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const plugin = process.argv[2];
let failures = 0;
function check(name, test) {
  try { test(); console.log(`PASS ${name}`); }
  catch (error) { failures++; console.error(`FAIL ${name}: ${error.message}`); }
}

// Fake herdr and rmk on PATH. herdr answers `pane list` and records every
// other call; rmk prints a line in the two Runmark directories, fails elsewhere.
// realpath: on macOS the temp dir is a symlink, and the fake rmk compares $PWD.
const bin = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'runmark-herdr-')));
const dir = name => path.join(bin, name);
for (const name of ['a', 'b', 'elsewhere', 'plain']) fs.mkdirSync(dir(name));
const log = dir('calls.log');
const panes = JSON.stringify({ result: { panes: [
  { workspace_id: 'w1', pane_id: 'w1:p1', cwd: dir('a') },
  { workspace_id: 'w1', pane_id: 'w1:p2', cwd: dir('elsewhere') },
  { workspace_id: 'w2', pane_id: 'w2:p1', cwd: dir('b') },
  { workspace_id: 'w3', pane_id: 'w3:p1', cwd: dir('plain') },
] } });
fs.writeFileSync(dir('herdr'), `#!/bin/sh
if [ "$1" = pane ]; then printf '%s' '${panes}'; exit 0; fi
printf '%s\\n' "$*" >> '${log}'
`, { mode: 0o755 });
fs.writeFileSync(dir('rmk'), `#!/bin/sh
case "$PWD" in '${dir('a')}') echo '3/7 · 1 open' ;; '${dir('b')}') echo '0/2' ;; *) exit 1 ;; esac
`, { mode: 0o755 });

function invoke(mode, env = {}) {
  fs.rmSync(log, { force: true });
  const result = spawnSync('sh', [path.join(plugin, 'runmark-herdr.sh'), mode], {
    env: { PATH: `${bin}:${process.env.PATH}`, HOME: process.env.HOME, ...env }, encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  return fs.existsSync(log) ? fs.readFileSync(log, 'utf8').trim().split('\n') : [];
}
const report = (workspace, line) => `workspace report-metadata ${workspace} --source runmark --token runmark=${line}`;
const context = { HERDR_PLUGIN_CONTEXT_JSON: JSON.stringify({ workspace_id: 'w1', workspace_cwd: dir('a') }) };

check('startup publishes every Runmark workspace, by its first pane', () => {
  assert.deepEqual(invoke('startup'), [report('w1', '3/7 · 1 open'), report('w2', '0/2')]);
});
check('an event publishes the workspace it names, not the focused one', () => {
  assert.deepEqual(invoke('event', { ...context,
    HERDR_PLUGIN_EVENT_JSON: JSON.stringify({ data: { workspace_id: 'w2', pane_id: 'w2:p1' } }) }),
  [report('w2', '0/2')]);
});
check('an event without a workspace falls back to the context', () => {
  assert.deepEqual(invoke('event', context), [report('w1', '3/7 · 1 open')]);
});
check('a workspace outside Runmark is left alone', () => {
  assert.deepEqual(invoke('event', { HERDR_PLUGIN_EVENT_JSON: JSON.stringify({ data: { workspace_id: 'w3' } }) }), []);
});
check('manifest: startup and three events run the wrapper', () => {
  const manifest = fs.readFileSync(path.join(plugin, 'herdr-plugin.toml'), 'utf8');
  assert.match(manifest, /^id = "runmark\.herdr"$/m);
  for (const on of ['workspace.created', 'worktree.created', 'pane.agent_status_changed']) {
    assert(manifest.includes(`on = "${on}"`), on);
  }
  assert(fs.statSync(path.join(plugin, 'runmark-herdr.sh')).mode & 0o111, 'wrapper must be executable');
});
fs.rmSync(bin, { recursive: true, force: true });
console.log(`RESULT ${failures} failure(s)`);
process.exit(failures ? 1 : 0);
NODE
