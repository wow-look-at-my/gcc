#!/usr/bin/env node
// Gate the fork's dejagnu .sum files against the published vanilla baseline.
//
// Usage:
//   node testsuite-diff.mjs --baseline DIR --current DIR \
//        [--waivers FILE] [--compare-tests PATH] [--report FILE]
//
// For every *.sum in CURRENT the same-named .sum must exist in BASELINE
// (a missing counterpart is a hard error: the baseline predates a new
// testsuite shard and must be regenerated). Each pair is
//   (a) run through contrib/compare_tests for the classic human-readable
//       report (informational -- its exit code does not gate), and
//   (b) parsed for the GATE, which fails on
//         new FAIL  -- a test that FAILs now but did not FAIL in the
//                      baseline. Mirrors compare_tests' normalization:
//                      XFAIL and ERROR count as FAIL on both sides (so
//                      PASS->XFAIL gates, XFAIL->FAIL does not), and a test
//                      absent from the baseline that FAILs now gates too.
//         new XPASS -- a test that XPASSes now but did not XPASS in the
//                      baseline. compare_tests normalizes XPASS to PASS, so
//                      this class is invisible to it -- gated here.
//       Progressions (new PASSes, fixed FAILs) and disappeared tests never
//       gate; they still show up in the embedded compare_tests report.
//
// Waivers (--waivers; missing file = no waivers): one entry per line, '#'
// comments and blank lines ignored. An entry is either
//     <test text>            waives that test text in every .sum
//     <sum>:<test text>      one .sum only, e.g. "g++.sum:g++.dg/foo.C ..."
// matched EXACTLY against the text after "FAIL: "/"XPASS: " (trimmed).
// Waived findings are reported but do not fail the gate.
//
// Exit: 0 gate passed; 1 gate failed or structural error; 2 usage.
// Node ESM, standard-library only (matches ci-verify-cache.mjs conventions).

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

function usage() {
  process.stderr.write(
    'usage: node testsuite-diff.mjs --baseline DIR --current DIR\n' +
    '         [--waivers FILE] [--compare-tests PATH] [--report FILE]\n');
  process.exit(2);
}

const opts = {};
for (let i = 2; i < process.argv.length; i += 2) {
  const k = process.argv[i], v = process.argv[i + 1];
  if (!k.startsWith('--') || v === undefined) usage();
  opts[k.slice(2)] = v;
}
if (!opts.baseline || !opts.current) usage();

// ---------------------------------------------------------------------------
// .sum parsing
// ---------------------------------------------------------------------------

const RESULT_RE =
  /^(PASS|FAIL|XFAIL|XPASS|KFAIL|KPASS|UNRESOLVED|UNSUPPORTED|UNTESTED|ERROR|WARNING):\s*(.*)$/;

// Statuses that count as "failing" for the new-FAIL gate, mirroring
// compare_tests' sed normalization (XFAIL->FAIL, ERROR->FAIL).
const FAILISH = new Set(['FAIL', 'XFAIL', 'ERROR']);

// file -> { results: Map<testText, Set<status>>, counts: Map<status, n> }
function parseSum(file) {
  const results = new Map();
  const counts = new Map();
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const m = RESULT_RE.exec(line);
    if (!m) continue;
    const status = m[1];
    const text = m[2].trim();
    if (!text) continue;
    counts.set(status, (counts.get(status) || 0) + 1);
    let set = results.get(text);
    if (!set) results.set(text, (set = new Set()));
    set.add(status);
  }
  return { results, counts };
}

const hasAny = (set, statuses) => {
  for (const s of set) if (statuses.has(s)) return true;
  return false;
};

// ---------------------------------------------------------------------------
// Waivers
// ---------------------------------------------------------------------------

// entry -> { hits: number }; keys are "<text>" or "<sum>:<text>".
const waivers = new Map();
if (opts.waivers && fs.existsSync(opts.waivers)) {
  for (const raw of fs.readFileSync(opts.waivers, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    waivers.set(line, { hits: 0 });
  }
}

function waived(sumName, text) {
  for (const key of [`${sumName}:${text}`, text]) {
    const w = waivers.get(key);
    if (w) { w.hits++; return true; }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Diff every current .sum against its baseline counterpart
// ---------------------------------------------------------------------------

const sumNames = fs.readdirSync(opts.current).filter((f) => f.endsWith('.sum')).sort();
if (sumNames.length === 0) {
  process.stderr.write(`error: no .sum files found in ${opts.current}\n`);
  process.exit(1);
}

const report = [];
report.push('# GCC testsuite diff vs vanilla fork-point baseline', '');

let gateFailures = 0;
let waivedCount = 0;
let structuralError = false;

for (const name of sumNames) {
  const curFile = path.join(opts.current, name);
  const baseFile = path.join(opts.baseline, name);
  report.push(`## ${name}`, '');

  if (!fs.existsSync(baseFile)) {
    structuralError = true;
    const msg = `baseline has no ${name} -- regenerate it (workflow_dispatch with build_baseline=true)`;
    process.stderr.write(`error: ${msg}\n`);
    report.push(`**ERROR**: ${msg}`, '');
    continue;
  }

  const base = parseSum(baseFile);
  const cur = parseSum(curFile);

  // Result-class counts, baseline vs current.
  const classes = ['PASS', 'FAIL', 'XFAIL', 'XPASS', 'ERROR', 'UNRESOLVED', 'UNSUPPORTED', 'UNTESTED'];
  report.push('| result | baseline | current |', '|---|---:|---:|');
  for (const c of classes)
    report.push(`| ${c} | ${base.counts.get(c) || 0} | ${cur.counts.get(c) || 0} |`);
  report.push('');

  // The gate.
  const newFails = [];
  const newXpasses = [];
  for (const [text, statuses] of cur.results) {
    const baseStatuses = base.results.get(text);
    if (hasAny(statuses, FAILISH) && !(baseStatuses && hasAny(baseStatuses, FAILISH)))
      newFails.push(text);
    if (statuses.has('XPASS') && !(baseStatuses && baseStatuses.has('XPASS')))
      newXpasses.push(text);
  }
  newFails.sort();
  newXpasses.sort();

  for (const [kind, list] of [['new FAIL', newFails], ['new XPASS', newXpasses]]) {
    if (list.length === 0) continue;
    report.push(`### ${kind}s (${list.length})`, '');
    for (const text of list) {
      if (waived(name, text)) {
        waivedCount++;
        report.push(`- ${text} **(waived)**`);
        process.stdout.write(`WAIVED ${kind}: ${name}: ${text}\n`);
      } else {
        gateFailures++;
        report.push(`- ${text}`);
        process.stdout.write(`GATE ${kind}: ${name}: ${text}\n`);
      }
    }
    report.push('');
  }
  if (newFails.length === 0 && newXpasses.length === 0)
    report.push('No new FAILs or XPASSes.', '');

  // Classic compare_tests report (informational).
  if (opts['compare-tests']) {
    const ct = spawnSync('sh', [opts['compare-tests'], baseFile, curFile],
                         { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    let out = ((ct.stdout || '') + (ct.stderr || '')).trimEnd();
    const lines = out.split('\n');
    const MAX = 400;
    if (lines.length > MAX)
      out = lines.slice(0, MAX).join('\n') +
            `\n... (${lines.length - MAX} more lines; full .sum files are in the job artifact)`;
    report.push(`<details><summary>contrib/compare_tests (exit ${ct.status})</summary>`, '',
                '```', out || '(no output)', '```', '', '</details>', '');
  }
}

// Waiver hygiene: entries that matched nothing are stale (or typo'd).
const unused = [...waivers.entries()].filter(([, w]) => w.hits === 0).map(([k]) => k);
if (unused.length) {
  report.push('### Unused waiver entries', '');
  for (const k of unused) {
    report.push(`- \`${k}\``);
    process.stdout.write(`note: unused waiver entry: ${k}\n`);
  }
  report.push('');
}

const verdict = structuralError
  ? 'FAILED (structural error: baseline incomplete)'
  : gateFailures
    ? `FAILED: ${gateFailures} unwaived new FAIL/XPASS finding(s)`
    : `PASSED (${waivedCount} waived finding(s))`;
report.push(`**Gate ${verdict}**`, '');

const reportText = report.join('\n');
if (opts.report) {
  fs.mkdirSync(path.dirname(opts.report), { recursive: true });
  fs.writeFileSync(opts.report, reportText);
}
if (process.env.GITHUB_STEP_SUMMARY) {
  // The run-summary cap is 1 MiB; leave room for other steps.
  const cap = 900 * 1024;
  fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY,
    reportText.length > cap ? reportText.slice(0, cap) + '\n... (truncated)\n' : reportText);
}

process.stdout.write(`\nTESTSUITE GATE ${verdict}\n`);
process.exit(structuralError || gateFailures ? 1 : 0);
