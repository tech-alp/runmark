'use strict';
// Publishes `rmk status --line` as the $runmark token of herdr workspaces.
// startup: every workspace; event: the workspace the event names.
const { execFileSync } = require('node:child_process');

const herdr = process.env.HERDR_BIN_PATH || 'herdr';
const run = (command, args, cwd) => execFileSync(command, args,
  { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], timeout: 5000 }).trim();
const parse = text => { try { return JSON.parse(text || '{}'); } catch { return {}; } };

// A workspace's directory is its first pane's: `workspace list` has none.
const directories = new Map();
try {
  for (const pane of parse(run(herdr, ['pane', 'list'])).result?.panes ?? []) {
    if (!directories.has(pane.workspace_id)) directories.set(pane.workspace_id, pane.cwd);
  }
} catch { /* herdr unreachable: nothing to publish to */ }

// The event's own workspace, not the focused one the context describes.
const context = parse(process.env.HERDR_PLUGIN_CONTEXT_JSON);
const event = parse(process.env.HERDR_PLUGIN_EVENT_JSON).data ?? {};
const targets = process.argv[2] === 'startup' ? [...directories.keys()]
  : [event.workspace_id ?? context.workspace_id];

for (const workspace of targets) {
  const cwd = directories.get(workspace) ?? (workspace === context.workspace_id ? context.workspace_cwd : undefined);
  if (!workspace || !cwd) continue;
  let line;
  try { line = run('rmk', ['status', '--line'], cwd); } catch { continue; } // not a Runmark project
  try {
    run(herdr, ['workspace', 'report-metadata', workspace, '--source', 'runmark', '--token', `runmark=${line}`]);
  } catch { /* a closed workspace or a stopped server is not an error */ }
}
