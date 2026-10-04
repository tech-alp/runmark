import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateVersion } from './release.mjs';
import { auditAllowlist, checkAudit } from './audit.mjs';
import { analyzeCommits } from '@semantic-release/commit-analyzer';
import releaseConfig from '../../release.config.cjs';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

const auditFixture = () => JSON.parse(readFileSync(new URL('./fixtures/npm-audit.json', import.meta.url), 'utf8'));
const auditDate = '2026-10-04';

test('audit allows only the listed advisory, including cyclic transitive findings', () => {
  const accepted = checkAudit(auditFixture(), auditAllowlist, auditDate);
  assert.deepEqual([...accepted.keys()], ['GHSA-vfj7-8cjw-p6xm']);
});

test('audit rejects an unlisted high or critical advisory in an already allowed package', () => {
  for (const severity of ['high', 'critical']) {
    const report = auditFixture();
    report.vulnerabilities.braces.via.push({
      severity, url: 'https://github.com/advisories/GHSA-xxxx-yyyy-zzzz',
    });
    assert.throws(() => checkAudit(report, auditAllowlist, auditDate), /Unlisted .* advisory/);
  }
});

test('audit exceptions expire at the start of their expiry date in UTC', () => {
  assert.doesNotThrow(() => checkAudit(auditFixture(), auditAllowlist, '2026-11-03'));
  for (const today of ['2026-11-04', '2026-11-05']) {
    assert.throws(() => checkAudit(auditFixture(), auditAllowlist, today), /Expired audit exception/);
  }
});

test('audit fails closed on errors, missing details and invalid exception metadata', () => {
  for (const report of [null, {}, { ...auditFixture(), error: { code: 'ENOAUDIT' } }]) {
    assert.throws(() => checkAudit(report, auditAllowlist, auditDate), /Invalid npm audit report/);
  }
  const report = auditFixture();
  delete report.vulnerabilities.braces;
  assert.throws(() => checkAudit(report, auditAllowlist, auditDate), /Missing audit details/);
  for (const entry of [{ reason: '', expires: '2026-11-04' }, { reason: 'temporary', expires: '2026-11-31' }]) {
    assert.throws(() => checkAudit(auditFixture(), { 'GHSA-vfj7-8cjw-p6xm': entry }, auditDate), /Missing reason|Invalid expiry/);
  }
});

test('only stable SemVer values may reach build commands', () => {
  for (const version of ['0.3.0', '1.0.0', '12.34.56']) assert.equal(validateVersion(version), version);
  for (const version of ['', '01.2.3', 'v1.2.3', '1.2', '1.2.3-beta.1', '1.2.3;echo bad', undefined]) {
    assert.throws(() => validateVersion(version));
  }
});

test('commit policy handles bang/footer breaking changes and non-release commits', async () => {
  const options = releaseConfig.plugins[0][1];
  for (const [message, expected] of [
    ['fix: repair', 'patch'], ['feat(cli): add command', 'minor'],
    ['feat!: remove command', 'major'], ['fix: change\n\nBREAKING CHANGE: incompatible', 'major'],
    ['docs: clarify', null], ['test: coverage', null], ['chore: tools', null],
  ]) {
    const result = await analyzeCommits(options, { cwd: process.cwd(), commits: [{ hash: '123', message }], logger: { log() {} } });
    assert.equal(result, expected, message);
  }
});

test('release excludes npm publishing and version/changelog commits', () => {
  assert.deepEqual(releaseConfig.branches, ['main']);
  assert.equal(releaseConfig.tagFormat, 'v${version}');
  assert.equal(releaseConfig.plugins.length, 4);
  for (const plugin of releaseConfig.plugins) {
    const name = Array.isArray(plugin) ? plugin[0] : plugin;
    assert(!name.includes('@semantic-release/npm/'));
    assert(!name.includes('@semantic-release/git/'));
  }
  const require = createRequire(import.meta.url);
  assert.throws(() => require('@semantic-release/npm'), /Runmark does not publish npm packages/);
  assert.throws(() => require.resolve('npm/package.json'), { code: 'MODULE_NOT_FOUND' });
});

test('real semantic-release dry-run computes 0.3.1 without creating a tag', async () => {
  const fixture = mkdtempSync(join(tmpdir(), 'rmk-semantic-test-'));
  const remote = join(fixture, 'remote.git');
  const repo = join(fixture, 'repo');
  const git = (...args) => execFileSync('git', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  git('init', '--bare', '-b', 'main', remote);
  git('init', '-b', 'main', repo);
  git('-C', repo, 'config', 'user.name', 'Release test');
  git('-C', repo, 'config', 'user.email', 'release-test@example.invalid');
  git('-C', repo, 'config', 'commit.gpgsign', 'false');
  git('-C', repo, 'config', 'tag.gpgsign', 'false');
  writeFileSync(join(repo, 'fixture.txt'), 'baseline');
  git('-C', repo, 'add', 'fixture.txt');
  git('-C', repo, 'commit', '-m', 'feat: baseline');
  git('-C', repo, 'tag', 'v0.3.0');
  git('-C', repo, 'remote', 'add', 'origin', remote);
  writeFileSync(join(repo, 'fixture.txt'), 'fixed');
  git('-C', repo, 'commit', '-am', 'fix: package');
  git('-C', repo, 'push', '-u', 'origin', 'main', '--tags');
  const root = fileURLToPath(new URL('../../', import.meta.url));
  const options = {
    branches: ['main'], tagFormat: releaseConfig.tagFormat, repositoryUrl: pathToFileURL(remote).href,
    ci: false, dryRun: true,
    plugins: releaseConfig.plugins.slice(0, 2).map(([name, options]) => [resolve(root, name), options]),
  };
  // semantic-release hooks process streams; keep it out of node:test's IPC process.
  const script = `import semanticRelease from ${JSON.stringify(new URL('./node_modules/semantic-release/index.js', import.meta.url).href)};
    const result = await semanticRelease(${JSON.stringify(options)}, { cwd: ${JSON.stringify(repo)} });
    console.log('DRY_RUN_VERSION=' + result.nextRelease.version);`;
  const output = execFileSync(process.execPath, ['--input-type=module', '-e', script], {
    cwd: repo, env: { PATH: process.env.PATH, HOME: process.env.HOME }, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'],
  });
  assert.match(output, /DRY_RUN_VERSION=0\.3\.1/);
  assert.equal(git('-C', repo, 'tag', '--list').trim(), 'v0.3.0');
});

test('publication requires main, license texts and an existing release baseline', () => {
  const repo = mkdtempSync(join(tmpdir(), 'rmk-publication-test-'));
  const git = (...args) => execFileSync('git', ['-C', repo, ...args], { stdio: 'pipe' });
  git('init', '-b', 'main');
  git('config', 'user.name', 'Release test');
  git('config', 'user.email', 'release-test@example.invalid');
  git('config', 'commit.gpgsign', 'false');
  git('config', 'tag.gpgsign', 'false');
  git('commit', '--allow-empty', '-m', 'chore: fixture');
  const script = fileURLToPath(new URL('./release.mjs', import.meta.url));
  const verify = (ref = 'refs/heads/main') => execFileSync(process.execPath, [script, 'verify'], {
    cwd: repo, env: { PATH: process.env.PATH, HOME: process.env.HOME, GITHUB_REF: ref }, stdio: 'pipe',
  });
  assert.throws(() => verify('refs/heads/topic'), /Release requires main/);
  assert.throws(() => verify(), /Choose a Runmark LICENSE/);
  writeFileSync(join(repo, 'LICENSE'), 'TEST FIXTURE ONLY');
  assert.throws(() => verify(), /third-party license texts/);
  mkdirSync(join(repo, 'THIRD_PARTY_NOTICES'));
  writeFileSync(join(repo, 'THIRD_PARTY_NOTICES', 'fixture.txt'), 'TEST FIXTURE ONLY');
  assert.throws(() => verify(), /last released version first/);
  git('tag', 'v0.3.0');
  assert.doesNotThrow(() => verify());
});
