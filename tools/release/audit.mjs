import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const auditAllowlist = {
  'GHSA-vfj7-8cjw-p6xm': {
    reason: 'braces <=3.0.3 has no patched version. It is used only by release devDependencies '
      + '(semantic-release -> @semantic-release/* -> micromatch -> braces), not shipped in Runmark. '
      + 'Temporarily accept the deeply nested pattern stack-exhaustion DoS; revisit upstream by expiry.',
    expires: '2026-11-04',
  },
};

const blocking = severity => severity === 'high' || severity === 'critical';

export function checkAudit(report, allowlist = auditAllowlist, today = new Date().toISOString().slice(0, 10)) {
  for (const [id, entry] of Object.entries(allowlist)) {
    assert(typeof entry.reason === 'string' && entry.reason.trim(), `Missing reason for ${id}`);
    assert(/^\d{4}-\d{2}-\d{2}$/.test(entry.expires)
      && new Date(entry.expires).toISOString().slice(0, 10) === entry.expires, `Invalid expiry for ${id}`);
    assert(entry.expires > today, `Expired audit exception: ${id} (${entry.expires})`);
  }
  assert(report && !report.error && report.auditReportVersion === 2
    && report.vulnerabilities && typeof report.vulnerabilities === 'object'
    && !Array.isArray(report.vulnerabilities), 'Invalid npm audit report');

  const accepted = new Map();
  for (const [name, vulnerability] of Object.entries(report.vulnerabilities)) {
    if (!blocking(vulnerability.severity)) continue;
    // Follow package references with a visited set: npm's via graph can contain cycles.
    const pending = [name];
    const visited = new Set();
    let foundAdvisory = false;
    while (pending.length) {
      const dependency = pending.pop();
      if (visited.has(dependency)) continue;
      visited.add(dependency);
      const node = report.vulnerabilities[dependency];
      assert(node && Array.isArray(node.via) && node.via.length, `Missing audit details for ${dependency}`);
      for (const via of node.via) {
        if (typeof via === 'string') {
          pending.push(via);
        } else {
          assert(via && typeof via.severity === 'string', `Invalid advisory for ${dependency}`);
          if (!blocking(via.severity)) continue;
          foundAdvisory = true;
          const id = /^https:\/\/github\.com\/advisories\/(GHSA-[a-z0-9-]+)$/.exec(via.url)?.[1];
          assert(id && Object.hasOwn(allowlist, id), `Unlisted ${via.severity} advisory: ${via.url ?? dependency}`);
          accepted.set(id, allowlist[id]);
        }
      }
    }
    assert(foundAdvisory, `No underlying high/critical advisory for ${name}`);
  }
  return accepted;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const result = spawnSync('npm', ['audit', '--json', '--audit-level=high'], {
      cwd: fileURLToPath(new URL('./', import.meta.url)), encoding: 'utf8',
    });
    if (result.error) throw result.error;
    assert(result.status === 0 || result.status === 1, `npm audit failed (${result.status}): ${result.stderr}`);
    const accepted = checkAudit(JSON.parse(result.stdout));
    assert(result.status === 0 || accepted.size > 0, 'npm audit failed without advisory details');
    for (const [id, entry] of accepted) console.log(`ALLOW ${id} until ${entry.expires} (UTC, exclusive): ${entry.reason}`);
    console.log('PASS npm audit: no unlisted high/critical advisories');
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
