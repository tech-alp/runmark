'use strict';
// Runmark in herdr. Modes:
//   startup | event   publish `rmk status --line` as the $runmark token
//   action <pane>     headless: open that popup in the focused pane's directory
//   pane <name>       inside the popup: resume, continue, fix or projects
const { execFileSync, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');

const herdr = process.env.HERDR_BIN_PATH || 'herdr';
const run = (command, args, cwd) => execFileSync(command, args,
  { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], timeout: 5000 }).trim();
const parse = text => { try { return JSON.parse(text || '{}'); } catch { return {}; } };
const context = parse(process.env.HERDR_PLUGIN_CONTEXT_JSON);
const [mode, name] = process.argv.slice(2);

// Every workspace when no targets are given.
function publish(targets) {
  // A workspace's directory is its first pane's: `workspace list` has none.
  const directories = new Map();
  try {
    for (const pane of parse(run(herdr, ['pane', 'list'])).result?.panes ?? []) {
      if (!directories.has(pane.workspace_id)) directories.set(pane.workspace_id, pane.cwd);
    }
  } catch { /* herdr unreachable: nothing to publish to */ }

  for (const workspace of targets ?? directories.keys()) {
    const cwd = directories.get(workspace) ?? (workspace === context.workspace_id ? context.workspace_cwd : undefined);
    if (!workspace || !cwd) continue;
    let line;
    try { line = run('rmk', ['status', '--line'], cwd); } catch { continue; } // not a Runmark project
    try {
      run(herdr, ['workspace', 'report-metadata', workspace, '--source', 'runmark', '--token', `runmark=${line}`]);
    } catch { /* a closed workspace or a stopped server is not an error */ }
  }
}

function openPane() {
  const cwd = context.focused_pane_cwd || context.workspace_cwd || os.homedir();
  run(herdr, ['plugin', 'pane', 'open', '--plugin', process.env.HERDR_PLUGIN_ID || 'runmark.herdr',
    '--entrypoint', name, '--cwd', cwd, '--focus']);
}

// Pager for read-only popups; PAGER=cat in tests.
const page = text => spawnSync('sh', ['-c', process.env.PAGER || 'less -R'], { input: text, stdio: ['pipe', 'inherit', 'inherit'] });
// One reader for every question: an interface per question drops the lines it
// had buffered when it closes, and the next answer never arrives.
let lines;
async function ask(question) {
  lines ??= readline.createInterface({ input: process.stdin })[Symbol.asyncIterator]();
  process.stdout.write(question);
  const { value } = await lines.next();
  return (value ?? '').trim();
}

function resume() {
  const result = spawnSync('rmk', ['resume', '--markdown'], { encoding: 'utf8' });
  page(result.status === 0 ? result.stdout : `No Runmark project here.\n\n${result.stderr}`);
}

function projects() {
  const home = process.env.RUNMARK_CONFIG_HOME || path.join(os.homedir(), '.config', 'runmark');
  let listed = [];
  try { listed = parse(fs.readFileSync(path.join(home, 'projects.json'), 'utf8')).projects ?? []; } catch { /* none registered */ }
  const rows = listed.map(config => {
    const project = path.basename(path.dirname(path.dirname(config)));
    let line;
    try { line = run('rmk', ['--project', config, 'status', '--line']) || 'nothing open'; } catch { line = 'unreadable'; }
    return `${project.padEnd(24)} ${line}`;
  });
  page(rows.length ? `${rows.join('\n')}\n` : 'No projects registered. Run `rmk init` in a project.\n');
}

// An interrupted execution is continued by `rmk start`, which takes over its
// branch and worktree; one that never finished is resumed where it is. Either
// way the agent starts with a prompt, so nothing has to wait for it.
async function continueWork() {
  const open = parse(spawnSync('rmk', ['status'], { encoding: 'utf8' }).stdout).open_work ?? [];
  if (!open.length) {
    await ask('No open work in this project. Enter to close. ');
    return;
  }
  open.forEach((work, index) => console.log(`${index + 1}) ${work.task.padEnd(10)} ${
    work.outcome === 'interrupted' ? 'interrupted  ' : 'never finished'}  last activity ${work.last_activity ?? 'unknown'}`));
  const work = open[Number(await ask('Continue which? ')) - 1];
  if (!work) return;
  const agent = (await ask('Agent [claude/codex] (claude): ')) || 'claude';
  if (agent !== 'claude' && agent !== 'codex') return;

  let { worktree } = work;
  if (work.outcome === 'interrupted') {
    const started = spawnSync('rmk', ['start', work.task, '--agent', agent], { encoding: 'utf8' });
    if (started.status !== 0) {
      await ask(`rmk start failed:\n${started.stderr}\nEnter to close. `);
      return;
    }
    worktree = parse(started.stdout).worktree;
  } else if (!fs.existsSync(worktree)) {
    await ask(`${worktree} is gone. Close the execution with \`rmk finish ${work.exec} --outcome interrupted\` and continue it again. Enter to close. `);
    return;
  }
  const created = parse(run(herdr, ['workspace', 'create', '--cwd', worktree, '--label', work.task, '--focus']));
  // `pane run` types into a shell: single-quote the prompt so nothing in it runs.
  const prompt = `Continue ${work.task}: read the output of rmk resume ${work.task} and pick up where it left off.`;
  run(herdr, ['pane', 'run', created.result.root_pane.pane_id, `${agent} '${prompt.replaceAll("'", "'\\''")}'`]);
}

// A finding whose fix is one command carries it as argv (`command` in status
// JSON); it runs without a shell, after the user confirms it.
async function fix() {
  const findings = (parse(spawnSync('rmk', ['status'], { encoding: 'utf8' }).stdout).findings ?? [])
    .filter(finding => finding.command?.length);
  if (!findings.length) {
    await ask('No warning here has a command that fixes it. Enter to close. ');
    return;
  }
  findings.forEach((finding, index) => console.log(`${index + 1}) ${finding.title}\n   ${finding.command.join(' ')}`));
  const finding = findings[Number(await ask('Fix which? ')) - 1];
  if (!finding || (await ask(`Run ${finding.command.join(' ')}? [y/N] `)).toLowerCase() !== 'y') return;
  const [program, ...args] = finding.command;
  const result = spawnSync(program, args, { stdio: ['ignore', 'inherit', 'inherit'] });
  publish([process.env.HERDR_WORKSPACE_ID]);
  await ask(`${result.status === 0 ? 'Done' : `Failed (exit ${result.status ?? result.error?.code})`}. Enter to close. `);
}

if (mode === 'action') openPane();
else if (mode === 'pane' && name === 'resume') resume();
else if (mode === 'pane' && name === 'projects') projects();
// The reader keeps stdin open; exit so the popup closes once the work is handed over.
else if (mode === 'pane' && name === 'continue') continueWork().finally(() => process.exit());
else if (mode === 'pane' && name === 'fix') fix().finally(() => process.exit());
// An event updates its own workspace, not the focused one the context describes.
else publish(mode === 'startup' ? undefined
  : [(parse(process.env.HERDR_PLUGIN_EVENT_JSON).data ?? {}).workspace_id ?? context.workspace_id]);
