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

// Fake herdr and rmk on PATH. herdr answers `pane list` and `workspace
// create` and records every call but `pane list`; rmk answers the commands the
// plugin uses: the status line only in the two Runmark directories.
// realpath: on macOS the temp dir is a symlink, and the fake rmk compares $PWD.
const bin = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'runmark-herdr-')));
const dir = name => path.join(bin, name);
for (const name of ['a', 'b', 'elsewhere', 'plain', 'wt']) fs.mkdirSync(dir(name));
const log = dir('calls.log');
const panes = JSON.stringify({ result: { panes: [
  { workspace_id: 'w1', pane_id: 'w1:p1', cwd: dir('a') },
  { workspace_id: 'w1', pane_id: 'w1:p2', cwd: dir('elsewhere') },
  { workspace_id: 'w2', pane_id: 'w2:p1', cwd: dir('b') },
  { workspace_id: 'w3', pane_id: 'w3:p1', cwd: dir('plain') },
] } });
fs.writeFileSync(dir('herdr'), `#!/bin/sh
case "$1 $2" in
  "pane list") printf '%s' '${panes}'; exit 0 ;;
  "workspace create") printf '%s\\n' "$*" >> '${log}'; printf '%s' '{"result":{"root_pane":{"pane_id":"w9:p1"}}}'; exit 0 ;;
esac
printf '%s\\n' "$*" >> '${log}'
`, { mode: 0o755 });
fs.writeFileSync(dir('rmk'), `#!/bin/sh
case "$*" in
  "status --line") case "$PWD" in '${dir('a')}') echo '3/7 · 1 open' ;; '${dir('b')}') echo '0/2' ;; *) exit 1 ;; esac ;;
  "resume --markdown") echo '# Runmark resume: MF-1' ;;
  "status") printf '%s' "$OPEN_WORK" ;;
  "start MF-1 --agent "*) printf '{"worktree":"%s"}' '${dir('wt')}' ;;
  "--project "*" status --line") echo "line for $2" ;;
  *) exit 1 ;;
esac
`, { mode: 0o755 });

function invoke(mode, env = {}, input = '') {
  fs.rmSync(log, { force: true });
  const args = Array.isArray(mode) ? mode : [mode];
  const result = spawnSync('sh', [path.join(plugin, 'runmark-herdr.sh'), ...args], {
    env: { PATH: `${bin}:${process.env.PATH}`, HOME: process.env.HOME, PAGER: 'cat', ...env }, encoding: 'utf8', input });
  assert.equal(result.status, 0, result.stderr);
  invoke.stdout = result.stdout;
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
check('an action opens its popup in the focused pane directory', () => {
  assert.deepEqual(invoke(['action', 'continue'], {
    HERDR_PLUGIN_ID: 'runmark.herdr',
    HERDR_PLUGIN_CONTEXT_JSON: JSON.stringify({ workspace_cwd: dir('a'), focused_pane_cwd: dir('b') }) }),
  [`plugin pane open --plugin runmark.herdr --entrypoint continue --cwd ${dir('b')} --focus`]);
});
check('the resume popup pages rmk resume', () => {
  invoke(['pane', 'resume']);
  assert.match(invoke.stdout, /# Runmark resume: MF-1/);
});
check('the projects popup lists every registered project with its line', () => {
  const home = dir('config');
  fs.mkdirSync(home, { recursive: true });
  fs.writeFileSync(path.join(home, 'projects.json'), JSON.stringify({ projects: ['/x/alpha/.runmark/project.json'] }));
  invoke(['pane', 'projects'], { RUNMARK_CONFIG_HOME: home });
  assert.match(invoke.stdout, /^alpha\s+line for \/x\/alpha\/\.runmark\/project\.json$/m);
});
const openWork = work => JSON.stringify({ open_work: [work] });
check('continue takes an interrupted task over and starts the agent there', () => {
  const calls = invoke(['pane', 'continue'], {
    OPEN_WORK: openWork({ task: 'MF-1', exec: 'e1', outcome: 'interrupted', worktree: dir('old') }) }, '1\ncodex\n');
  assert.equal(calls[0], `workspace create --cwd ${dir('wt')} --label MF-1 --focus`);
  assert.equal(calls[1], "pane run w9:p1 codex 'Continue MF-1: read the output of rmk resume MF-1 and pick up where it left off.'");
});
check('continue resumes a never-finished task in its worktree, claude by default', () => {
  const calls = invoke(['pane', 'continue'], {
    OPEN_WORK: openWork({ task: 'MF-2', exec: 'e2', outcome: null, worktree: dir('wt') }) }, '1\n\n');
  assert.equal(calls[0], `workspace create --cwd ${dir('wt')} --label MF-2 --focus`);
  assert.match(calls[1], /^pane run w9:p1 claude 'Continue MF-2/);
});
check('continue leaves a missing worktree alone', () => {
  assert.deepEqual(invoke(['pane', 'continue'], {
    OPEN_WORK: openWork({ task: 'MF-2', exec: 'e2', outcome: null, worktree: dir('gone') }) }, '1\n\n\n'), []);
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
