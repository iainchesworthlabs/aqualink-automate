import { test, expect } from '@playwright/test';
import { spawn } from 'node:child_process';
import { existsSync, openSync, closeSync, fstatSync, readFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/**
 * Backend logging-sink end-to-end coverage (docs/logging-sinks-redesign.md §14.3).
 *
 * These tests do NOT use the shared Playwright webServer or a browser. They spawn
 * their own short-lived application processes with specific logging arguments and
 * environment, capture the process's stderr, and assert on the emitted records —
 * exercising the real sink layer (console formatter, the `auto` policy, and the
 * sd-daemon `<N>` priority prefixes) through the actual binary.
 */

const ROOT = join(__dirname, '..');
const APP_EXE = process.env.AQUALINK_EXE ?? join(ROOT, 'build', 'wt', 'src', 'aqualink-automate.exe');
const REPLAY_FIXTURE = join(ROOT, 'test', 'fixtures', 'sample_session.cap');
const DOC_ROOT = join(ROOT, 'assets', 'web');

// Every operational channel; --replay-filename depends on each being present. Audit
// is deliberately absent — it is not an operational log channel.
const LOG_CHANNELS = [
  'main', 'certificates', 'coroutines', 'developer', 'devices', 'equipment',
  'exceptions', 'messages', 'mqtt', 'navigation', 'options', 'platform',
  'profiling', 'protocol', 'scraping', 'serial', 'signals', 'web',
];

// Base arguments that let the binary start deterministically against the replay
// fixture (mirrors playwright.config.ts). A unique port per call avoids clashes.
function baseArgs(port: number): string[] {
  const args = [
    '--dev-mode',
    '--replay-filename', REPLAY_FIXTURE,
    '--http-port', String(port),
    '--address', '127.0.0.1',
    '--disable-https',
    '--doc-root', DOC_ROOT,
    '--jandy-disable-emulation',
    '--profiler', 'tracy',
  ];
  for (const channel of LOG_CHANNELS) {
    args.push(`--loglevel-${channel}`, 'info');
  }
  return args;
}

// Run the app until `settleMs` of startup output has accumulated, then stop it and
// return the captured stderr. When `stderrFd` is provided the child writes stderr
// there (used for the journald-inode trick); otherwise stderr is piped and collected.
async function runAndCaptureStderr(
  extraArgs: string[],
  opts: { port: number; env?: NodeJS.ProcessEnv; stderrFd?: number; settleMs?: number },
): Promise<string> {
  const settleMs = opts.settleMs ?? 3500;

  const child = spawn(APP_EXE, [...baseArgs(opts.port), ...extraArgs], {
    cwd: ROOT,
    env: { ...process.env, ...(opts.env ?? {}) },
    stdio: ['ignore', 'ignore', opts.stderrFd !== undefined ? opts.stderrFd : 'pipe'],
  });

  let captured = '';
  if (opts.stderrFd === undefined && child.stderr) {
    child.stderr.on('data', (chunk: Buffer) => { captured += chunk.toString(); });
  }

  await new Promise<void>((resolve) => setTimeout(resolve, settleMs));

  // SIGTERM so the app runs its clean shutdown (which flushes + removes sinks).
  child.kill('SIGTERM');
  await new Promise<void>((resolve) => {
    const t = setTimeout(() => { child.kill('SIGKILL'); resolve(); }, 8000);
    child.on('exit', () => { clearTimeout(t); resolve(); });
  });

  return captured;
}

test.beforeAll(() => {
  expect(existsSync(APP_EXE), `Application binary not found at ${APP_EXE}; build it or set AQUALINK_EXE.`).toBeTruthy();
});

test('logs in the human text format by default (no journald prefixes on a pipe)', async () => {
  const stderr = await runAndCaptureStderr([], { port: 18101 });

  // The text formatter renders "<line-id>: <Severity>\t(Channel) message".
  expect(stderr).toMatch(/\d{8}: <(Info|Notify|Warning|Error)>\s+\(\w+\)/);
  expect(stderr).toContain('Configuring application options');

  // stderr is a pipe, not the journal, so NO line may begin with a "<N>" prefix.
  expect(stderr).not.toMatch(/^<\d>/m);
});

test('console-only sink still delivers records with --log-sinks console', async () => {
  const stderr = await runAndCaptureStderr(['--log-sinks', 'console'], { port: 18102 });
  expect(stderr).toContain('Configuring application options');
  expect(stderr).not.toMatch(/^<\d>/m);
});

// journald priority prefixes only apply on a systemd/POSIX host where stderr is the
// journal stream. We emulate that: point the child's stderr at a file, then set
// JOURNAL_STREAM to that file's device:inode so the app's own fstat(stderr) matches
// and the real detection path (not a test flag) activates.
test('emits sd-daemon <N> priority prefixes when stderr is the journal (Linux)', async () => {
  test.skip(process.platform !== 'linux', 'journald stream detection is POSIX/systemd-only');

  const dir = mkdtempSync(join(tmpdir(), 'aa-journal-'));
  const logPath = join(dir, 'stderr.log');
  const fd = openSync(logPath, 'w');
  try {
    const st = fstatSync(fd);
    const journalStream = `${st.dev}:${st.ino}`;

    await runAndCaptureStderr([], { port: 18103, stderrFd: fd, env: { JOURNAL_STREAM: journalStream } });

    const contents = readFileSync(logPath, 'utf8');
    // At least one record must carry a "<N>" prefix, and an Info record must be <6>.
    expect(contents).toMatch(/^<\d>/m);
    expect(contents).toMatch(/^<6>\d{8}: <Info>/m);
  } finally {
    closeSync(fd);
  }
});
