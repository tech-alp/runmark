import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readdirSync, existsSync, cpSync, mkdirSync, realpathSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export function validateVersion(version) {
  assert.match(version ?? '', /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/, 'Expected stable x.y.z version');
  return version;
}

function run(command, args, options = {}) {
  return execFileSync(command, args, { stdio: 'inherit', ...options });
}

export function verifyPublication() {
  assert.equal(process.env.GITHUB_REF, 'refs/heads/main', 'Release requires main');
  assert(existsSync('LICENSE'), 'Choose a Runmark LICENSE before public release');
  assert(existsSync('THIRD_PARTY_NOTICES') && readdirSync('THIRD_PARTY_NOTICES').length,
    'Provide reviewed third-party license texts before public release');
  const tags = run('git', ['tag', '--merged', 'HEAD', '--list', 'v[0-9]*'], { encoding: 'utf8', stdio: 'pipe' });
  assert(tags.split('\n').some(tag => /^v\d+\.\d+\.\d+$/.test(tag)),
    'Confirm/tag the last released version first; do not accidentally bootstrap 1.0.0');
}

export function smoke(directory, version) {
  validateVersion(version);
  assert(!existsSync(resolve(directory, 'bin/runmark-desktop')), 'CLI package must not contain runmark-desktop');
  const binary = resolve(directory, 'bin/rmk');
  const environment = { PATH: '/usr/bin:/bin', TMPDIR: tmpdir(), DYLD_PRINT_LIBRARIES: '1' };
  const result = spawnSync(binary, ['--version'], { env: environment, encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout.trim(), `rmk ${version}`);
  const qtImages = [...result.stderr.matchAll(/^dyld\[\d+\]: <[^>]+> (.*QtCore.framework\/.*QtCore)$/gm)];
  assert(qtImages.length > 0, 'dyld must report the loaded Qt Core');
  for (const [, path] of qtImages) {
    assert(realpathSync(path).startsWith(realpathSync(directory) + '/'), 'Qt must load from the relocated package');
  }
  const fixture = mkdtempSync(join(tmpdir(), 'rmk-package-fixture-'));
  const config = join(fixture, 'project.json');
  writeFileSync(config, JSON.stringify({ version: 1, name: 'package-smoke', worktree_root: 'worktrees',
    task_id_pattern: 'RM-\\d+', plan: { paths: ['plan.md'] },
    repos: [{ name: 'fixture', path: '.', base: { remote: 'origin', branch: 'main' } }] }));
  const inspected = execFileSync(binary, ['-p', config, 'inspect'], { env: environment, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  assert.equal(JSON.parse(inspected).name, 'package-smoke');
  // BundleUtilities verifies dependency closure; check its relocated copy too.
  run('cmake', ['-DCLI=' + binary, '-P', 'tools/release/verify-package.cmake']);
  console.log(`PASS relocated package ${version}`);
}

export function prepare(version) {
  validateVersion(version);
  assert.equal(process.platform, 'darwin');
  assert.equal(process.arch, 'arm64');
  const build = mkdtempSync(join(tmpdir(), 'rmk-release-build-'));
  run('cmake', ['--preset', 'release', '-B', build, `-DRUNMARK_VERSION=${version}`, '-DRUNMARK_BUNDLE_RUNTIME=ON', '-DRUNMARK_BUILD_UI=OFF']);
  run('cmake', ['--build', build, '--parallel', '4']);
  try {
    run('ctest', ['--test-dir', build, '--output-on-failure', '--no-tests=error']);
  } finally {
    mkdirSync('build/artifacts', { recursive: true });
    if (existsSync(join(build, 'Testing'))) cpSync(join(build, 'Testing'), 'build/artifacts/Testing', { recursive: true });
  }
  run('sh', ['plugins/runmark-agent/test.sh']);
  run('cpack', ['--config', join(build, 'CPackConfig.cmake'), '-B', 'build/artifacts']);
  const archive = resolve('build/artifacts', `runmark-${version}-macos-arm64.tar.gz`);
  const relocated = mkdtempSync(join(tmpdir(), 'rmk-relocated-'));
  run('tar', ['-xzf', archive, '-C', relocated]);
  smoke(join(relocated, `runmark-${version}-macos-arm64`), version);
  const checksum = execFileSync('shasum', ['-a', '256', archive], { encoding: 'utf8' }).split(' ')[0];
  writeFileSync(`${archive}.sha256`, `${checksum}  runmark-${version}-macos-arm64.tar.gz\n`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [command, argument, version] = process.argv.slice(2);
  if (command === 'prepare') prepare(argument);
  else if (command === 'verify') verifyPublication();
  else if (command === 'smoke') smoke(argument, version);
  else throw new Error('Usage: release.mjs prepare VERSION | verify | smoke DIRECTORY VERSION');
}
