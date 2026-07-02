import { defineConfig, devices } from '@playwright/test';
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

/**
 * Playwright end-to-end UI test configuration for Aqualink Automate.
 *
 * The `webServer` block boots the *real* application binary against a recorded
 * RS-485 replay fixture (an AquaRite salt-water generator session reporting
 * 40% output / 3200 PPM).  The app serves its Alpine.js web UI over plain HTTP
 * on a fixed test port; the tests then drive that UI in a real browser.
 *
 * App launch (see flags below):
 *   --dev-mode                       enable developer mode (gates --replay-filename)
 *   --replay-filename <fixture.cap>  source deterministic RS-485 data from the fixture
 *   --http-port <PORT>               serve the UI + REST/WS API on a fixed test port
 *   --address 127.0.0.1              bind to loopback only
 *   --disable-https                  no TLS in tests (avoids cert provisioning)
 *   --doc-root <assets/web>          serve THIS worktree's web UI assets
 *   --jandy-disable-emulation        skip the equipment-discovery phase so the
 *                                    WebSocket reports the system "ready" (commands
 *                                    enabled) immediately, instead of "starting".
 *   --profiler tracy                 required: --replay-filename has a hard
 *                                    dependency on --profiler.  Tracy is not
 *                                    compiled into the debug build, so the profiler
 *                                    factory falls back to a no-op profiler.
 *   --loglevel-<channel> info        required: --replay-filename also depends on
 *                                    EVERY per-channel log-level option (they carry
 *                                    no default, so the dependency only clears when
 *                                    each is passed explicitly).
 *
 * Conflict / dependency notes (verified against src/core/options/* and a live run):
 *   * --disable-https conflicts with --https-port / --cert / --cert-key, so none
 *     of those are passed.  The cert options carry *default* values, but the
 *     conflict checker ignores defaulted options, so --disable-https is accepted.
 *   * --disable-http conflicts with --http-port, so --disable-http is NOT passed.
 *   * --replay-filename depends on --dev-mode, --profiler and all --loglevel-*
 *     options, and conflicts with --record-serial (not passed).
 */

// Every per-channel log level the developer options expose; --replay-filename
// requires each to be present (see option_dependency_helper.cpp).
const LOG_CHANNELS = [
  'audit', 'main', 'certificates', 'coroutines', 'developer', 'devices',
  'equipment', 'exceptions', 'messages', 'mqtt', 'navigation', 'options',
  'platform', 'profiling', 'protocol', 'scraping', 'serial', 'signals', 'web',
];

const HOST = '127.0.0.1';
const PORT = Number(process.env.AQUALINK_TEST_PORT ?? 18080);
const BASE_URL = `http://${HOST}:${PORT}`;

// WS5 legacy auth mode: when AQUALINK_AUTH_TOKEN is set, boot the app with the
// old shared --api-auth-token (kept for backwards compatibility; the Wave A auth
// spec no longer uses it).
const AUTH_TOKEN = process.env.AQUALINK_AUTH_TOKEN;

// Wave A identity mode: when AQUALINK_AUTH_MODE=enabled, boot the app with the
// full identity system (`--auth-mode enabled`) against a FRESH temp
// --auth-state-dir so the user store starts EMPTY (setup_required = true). The
// auth spec then drives the first-run wizard, login/logout, and session flows.
// No bootstrap admin is created: the spec's first test creates it via the setup
// wizard and the empty-store precondition is exactly what that test needs.
//
// To instead boot with a pre-seeded administrator (e.g. to run only the
// login/session tests), set AQUALINK_BOOTSTRAP_ADMIN + AQUALINK_BOOTSTRAP_ADMIN_PASSWORD;
// the config passes `--bootstrap-admin <user>` and hands the password to the
// binary via the AQUALINK_BOOTSTRAP_ADMIN_PASSWORD env it already reads.
const AUTH_MODE = process.env.AQUALINK_AUTH_MODE === 'enabled';
const BOOTSTRAP_ADMIN = process.env.AQUALINK_BOOTSTRAP_ADMIN;
const AUTH_STATE_DIR = AUTH_MODE
  ? (process.env.AQUALINK_AUTH_STATE_DIR ?? mkdtempSync(join(tmpdir(), 'aqualink-auth-')))
  : undefined;

// WS2 history mode: when AQUALINK_HISTORY_DB is set, boot with --history-db and
// run ONLY the trends spec (which then expects recorded series + a chart).
const HISTORY_DB = process.env.AQUALINK_HISTORY_DB;

// WS4 scheduler mode: when AQUALINK_SCHEDULES_FILE is set, boot with
// --schedules-file and run ONLY the schedules spec (schedule CRUD).
const SCHEDULES_FILE = process.env.AQUALINK_SCHEDULES_FILE;

const ROOT = __dirname;

// Resolve the built application binary.  The `wt` CMake preset emits it under
// build/wt/src/.  Allow an override for CI / alternate build dirs.
const APP_EXE =
  process.env.AQUALINK_EXE ?? join(ROOT, 'build', 'wt', 'src', 'aqualink-automate.exe');

const DOC_ROOT = join(ROOT, 'assets', 'web');
const REPLAY_FIXTURE = join(ROOT, 'test', 'fixtures', 'sample_session.cap');

if (!existsSync(APP_EXE)) {
  // Fail loudly with an actionable message rather than letting Playwright emit a
  // generic "webServer failed to start" after a long timeout.
  throw new Error(
    `Application binary not found at ${APP_EXE}.\n` +
      `Build it first (from the VS Dev Shell):\n` +
      `  cmake --preset wt && cmake --build --preset wt --target aqualink-automate\n` +
      `or set AQUALINK_EXE to the binary path.`,
  );
}

export default defineConfig({
  testDir: './e2e',
  // The auth spec needs an identity-enabled server; everything else needs an
  // unauthenticated one. Select by AQUALINK_AUTH_MODE / AQUALINK_AUTH_TOKEN.
  testMatch: (AUTH_MODE || AUTH_TOKEN)
    ? ['**/auth.spec.ts']
    : (HISTORY_DB ? ['**/trends.spec.ts'] : (SCHEDULES_FILE ? ['**/schedules.spec.ts'] : undefined)),
  testIgnore: (AUTH_MODE || AUTH_TOKEN || HISTORY_DB || SCHEDULES_FILE) ? undefined : ['**/auth.spec.ts'],
  // Replay is deterministic but the UI is global mutable state behind one backend;
  // run serially so command-button tests don't race each other's optimistic updates.
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: [['list'], ['html', { open: 'never' }]],

  timeout: 30_000,
  expect: { timeout: 10_000 },

  use: {
    baseURL: BASE_URL,
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
  },

  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],

  webServer: {
    command: [
      `"${APP_EXE}"`,
      '--dev-mode',
      `--replay-filename "${REPLAY_FIXTURE}"`,
      `--http-port ${PORT}`,
      `--address ${HOST}`,
      '--disable-https',
      `--doc-root "${DOC_ROOT}"`,
      '--jandy-disable-emulation',
      '--profiler tracy',
      ...LOG_CHANNELS.map((ch) => `--loglevel-${ch} info`),
      ...(AUTH_TOKEN ? [`--api-auth-token ${AUTH_TOKEN}`] : []),
      ...(AUTH_MODE
        ? [
            '--auth-mode enabled',
            `--auth-state-dir "${AUTH_STATE_DIR}"`,
            ...(BOOTSTRAP_ADMIN ? [`--bootstrap-admin ${BOOTSTRAP_ADMIN}`] : []),
          ]
        : []),
      ...(HISTORY_DB ? [`--history-db "${HISTORY_DB}"`, '--history-flush-seconds 1'] : []),
      ...(SCHEDULES_FILE ? [`--schedules-file "${SCHEDULES_FILE}"`] : []),
    ].join(' '),
    url: BASE_URL,
    reuseExistingServer: false,
    timeout: 120_000,
    stdout: 'pipe',
    stderr: 'pipe',
    // The bootstrap password (when seeding an admin) is passed via the env the
    // binary reads, never on the command line.
    env: BOOTSTRAP_ADMIN && process.env.AQUALINK_BOOTSTRAP_ADMIN_PASSWORD
      ? { AQUALINK_BOOTSTRAP_ADMIN_PASSWORD: process.env.AQUALINK_BOOTSTRAP_ADMIN_PASSWORD }
      : undefined,
    // Stop the app with a *catchable* signal instead of Playwright's default
    // SIGKILL. The binary installs a boost::asio signal_set on SIGINT/SIGTERM
    // (see aqualink-automate.cpp) and returns from main on receipt, so a clean
    // exit runs its shutdown sequence. This also matters for the coverage e2e
    // job (automated-codescanning.yml): gcov flushes the per-TU .gcda counters
    // from an atexit handler that ONLY runs on a clean exit — a SIGKILL would
    // discard all e2e coverage. The timeout bounds the wait before Playwright
    // escalates to SIGKILL if the app fails to stop.
    gracefulShutdown: { signal: 'SIGTERM', timeout: 15_000 },
  },
});
