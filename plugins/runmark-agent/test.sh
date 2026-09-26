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
const repo = path.resolve(plugin, '../..');
const json = file => JSON.parse(fs.readFileSync(file, 'utf8'));
let failures = 0;
function check(name, test) {
  try { test(); console.log(`PASS ${name}`); }
  catch (error) { failures++; console.error(`FAIL ${name}: ${error.message}`); }
}
function scan(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) scan(file);
    else assert(!fs.readFileSync(file, 'utf8').includes('.runmark/'), file);
  }
}
check('TC-007: hooks/ and skills/ contain no .runmark/ string', () => {
  scan(path.join(plugin, 'hooks'));
  scan(path.join(plugin, 'skills'));
});
check('four plugin JSON files parse', () => {
  for (const file of ['.claude-plugin/plugin.json', '.codex-plugin/plugin.json',
    'hooks/hooks.json', 'skills/runmark/compatibility.json']) json(path.join(plugin, file));
});
check('four session hooks and manifest contracts', () => {
  const claude = json(path.join(plugin, '.claude-plugin/plugin.json'));
  const codex = json(path.join(plugin, '.codex-plugin/plugin.json'));
  assert.deepEqual(Object.keys(claude).sort(), ['author', 'description', 'name', 'version']);
  assert.equal(claude.name, 'runmark-agent');
  assert.equal(codex.name, claude.name);
  // Codex wants the `hooks` field to be a file path. With an empty object the
  // plugin's hooks are never discovered (it does not appear under "From
  // Plugins" in Codex desktop); the working examples all write a path.
  assert.equal(codex.hooks, './hooks/hooks.json');
  for (const field of ['displayName', 'shortDescription', 'longDescription', 'developerName', 'category']) {
    assert.equal(typeof codex.interface[field], 'string');
    assert(codex.interface[field].trim());
  }
  assert(Array.isArray(codex.interface.capabilities));
  assert(Array.isArray(codex.interface.defaultPrompt));
  assert(codex.interface.defaultPrompt.length > 0 && codex.interface.defaultPrompt.length <= 3);
  assert(codex.interface.defaultPrompt.every(prompt => typeof prompt === 'string' && prompt.length <= 128));
  const hooks = json(path.join(plugin, 'hooks/hooks.json')).hooks;
  // Every session is recorded however it was started (eng review D13):
  // start, each prompt, each finished reply, end.
  const events = { SessionStart: 'session-start', UserPromptSubmit: 'prompt-submit', Stop: 'stop', SessionEnd: 'session-end' };
  assert.deepEqual(Object.keys(hooks), Object.keys(events));
  for (const [name, argument] of Object.entries(events)) {
    assert.equal(hooks[name].length, 1);
    const hook = hooks[name][0].hooks;
    assert.equal(hook.length, 1);
    assert.equal(hook[0].type, 'command');
    // Hooks can run with a narrow PATH (under Codex node was missing, exit 127).
    // The entry point is therefore an sh wrapper that widens PATH; the shebang,
    // the +x bit and the install directories are all part of the contract.
    assert.equal(hook[0].command, `\${CLAUDE_PLUGIN_ROOT}/hooks/runmark-hook.sh ${argument}`);
    assert.equal(hook[0].commandWindows, `node "\${CLAUDE_PLUGIN_ROOT}/hooks/runmark-hook.js" ${argument}`);
    // Stop and prompt-submit run on every turn: keep them short.
    assert(hook[0].timeout <= (name === 'SessionStart' ? 10 : 5), `${name} timeout`);
  }
  // /clear starts a new session, /compact drops the context: both need it again.
  assert.equal(hooks.SessionStart[0].matcher, 'startup|resume|clear|compact');
  const entry = path.join(plugin, 'hooks/runmark-hook.sh');
  const wrapper = fs.readFileSync(entry, 'utf8');
  assert(wrapper.startsWith('#!/bin/sh\n'), 'shebang');
  assert(fs.statSync(entry).mode & 0o111, 'wrapper must be executable');
  for (const dir of ['/opt/homebrew/bin', '/usr/local/bin', '$HOME/.local/bin']) {
    assert(wrapper.includes(dir), dir);
  }
});
for (const runtime of ['claude', 'codex']) {
  check(`${runtime} marketplace JSON and source directory`, () => {
    const file = runtime === 'claude' ? '.claude-plugin/marketplace.json' : '.agents/plugins/marketplace.json';
    const market = json(path.join(repo, file));
    assert(market.plugins.some(entry => entry.name === 'runmark-agent'));
    for (const entry of market.plugins) {
      const source = runtime === 'claude' ? entry.source : entry.source.path;
      assert(source.startsWith('./') && !source.split('/').includes('..'));
      assert(fs.statSync(path.resolve(repo, source)).isDirectory());
      if (runtime === 'codex') {
        assert.equal(entry.source.source, 'local');
        assert.equal(entry.policy.installation, 'AVAILABLE');
        assert.equal(entry.policy.authentication, 'ON_INSTALL');
        assert(entry.category);
      }
    }
  });
}
check('root CMakeLists.txt contains no plugins', () => {
  assert(!fs.readFileSync(path.join(repo, 'CMakeLists.txt'), 'utf8').includes('plugins'));
});
check('skill frontmatter and ADR-002 recording guidance', () => {
  const skill = fs.readFileSync(path.join(plugin, 'skills/runmark/SKILL.md'), 'utf8');
  assert.match(skill, /^---\nname: runmark\ndescription: .+\n---\n/);
  for (const text of ['rmk evidence', 'rmk note', '--kind agent_summary', 'ADR-002', 'claim, not a measurement']) {
    assert(skill.includes(text), text);
  }
});

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'runmark-hook-test-'));
try {
  const log = path.join(temporary, 'calls.jsonl');
  const bin = path.join(temporary, 'bin');
  fs.mkdirSync(bin);
  fs.writeFileSync(path.join(bin, 'rmk'), `#!${process.execPath}
const fs = require('node:fs');
const stdin = process.argv.includes('hook') ? fs.readFileSync(0, 'utf8') : '';
fs.appendFileSync(process.env.CALL_LOG, JSON.stringify({ argv: process.argv.slice(2), stdin }) + '\\n');
if (process.env.FAKE_MODE === 'timeout') setTimeout(() => {}, 10000);
else if (process.env.FAKE_MODE === 'failure') { console.error('CLI failed'); process.exit(1); }
else if (process.argv.includes('--version')) console.log(process.env.FAKE_VERSION);
else process.stdout.write(process.env.FAKE_OUTPUT || '');
`, { mode: 0o755 });
  const valid = JSON.stringify({ cwd: repo, session_id: 'x' });
  // Run the command the manifest DECLARES, not the script. Calling the script
  // directly worked while the command in hooks.json exited 127 under Codex:
  // a test that checks the wrong thing is green about the wrong thing.
  const manifest = json(path.join(plugin, 'hooks/hooks.json')).hooks;
  function run(input = valid, env = {}, event = 'SessionStart') {
    fs.writeFileSync(log, '');
    const result = spawnSync('/bin/sh', ['-c', manifest[event][0].hooks[0].command], {
      input, encoding: 'utf8', timeout: 6000,
      env: { ...process.env, CLAUDE_PLUGIN_ROOT: plugin, PATH: bin, HOME: temporary,
        CALL_LOG: log, FAKE_VERSION: 'rmk 0.4.0', FAKE_MODE: '', ...env },
    });
    assert.ifError(result.error);
    assert.equal(result.status, 0);
    const lines = fs.readFileSync(log, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse);
    result.calls = lines.map(line => line.argv);
    result.stdins = lines.map(line => line.stdin);
    return result;
  }
  check('empty, malformed and invalid input: silent exit 0, no CLI call', () => {
    for (const input of ['', '{', 'null', '[]', '{}', '{"cwd":42}', '{"cwd":"relative"}', JSON.stringify({ cwd: repo })]) {
      const result = run(input);
      assert.equal(result.stderr, '');
      assert.deepEqual(result.calls, []);
      assert.equal(result.stdout, '');
    }
  });
  check('missing CLI: silent exit 0', () => {
    const result = run(valid, { PATH: temporary });
    assert.equal(result.stderr, '');
    assert.deepEqual(result.calls, []);
  });
  check('TC-006: session start forwards the runtime JSON to rmk hook and passes its context through', () => {
    for (const version of ['0.4.0', '0.4.0+build.1', '0.10.0', '1.0.0', '0.5.0-rc.1']) {
      const result = run(valid, { FAKE_VERSION: `rmk ${version}`, FAKE_OUTPUT: '# Runmark resume: MF-1\n' });
      assert.equal(result.stderr, '');
      // The hook only forwards; it passes no selector and makes no decision (TC-006).
      assert.deepEqual(result.calls, [['--version'], ['hook', 'session-start']]);
      assert.equal(result.stdins[1], valid);
      // Plain text: both runtimes take stdout as context directly.
      assert.equal(result.stdout, '# Runmark resume: MF-1\n');
    }
  });
  check('every-turn events skip the version check and pass rmk output through', () => {
    const block = '{"decision":"block","reason":"record a note"}\n';
    for (const [event, argument] of [['UserPromptSubmit', 'prompt-submit'], ['Stop', 'stop'], ['SessionEnd', 'session-end']]) {
      const result = run(valid, { FAKE_OUTPUT: event === 'Stop' ? block : '' }, event);
      assert.equal(result.stderr, '');
      assert.deepEqual(result.calls, [['hook', argument]]);
      assert.equal(result.stdins[0], valid);
      assert.equal(result.stdout, event === 'Stop' ? block : '');
    }
  });
  check('empty output: no injection', () => {
    const result = run(valid, { FAKE_OUTPUT: '   \n' });
    assert.equal(result.stderr, '');
    assert.equal(result.stdout, '');
    assert.deepEqual(result.calls, [['--version'], ['hook', 'session-start']]);
  });
  check('incompatible or unknown version: explicit minimum on stderr, exit 0', () => {
    for (const version of ['rmk 0.3.0', 'rmk 0.4.0-rc.1', 'unknown']) {
      const result = run(valid, { FAKE_VERSION: version });
      assert.match(result.stderr, /requires >= 0\.4\.0/);
      assert.deepEqual(result.calls, [['--version']]);
      assert.equal(result.stdout, '');
    }
  });
  check('CLI failure and timeout: silent exit 0', () => {
    for (const mode of ['failure', 'timeout']) {
      const result = run(valid, { FAKE_MODE: mode });
      assert.equal(result.stderr, '');
      assert.deepEqual(result.calls, [['--version']]);
      assert.equal(result.stdout, '');
    }
  });
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}
console.log(`RESULT ${failures} failure(s)`);
process.exitCode = failures ? 1 : 0;
NODE
