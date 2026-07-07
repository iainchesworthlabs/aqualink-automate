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
// requires each to be present (see option_dependency_helper.cpp). Note: audit is
// deliberately absent — it is a separate subsystem, not an operational log channel
// (docs/logging-sinks-redesign.md §10), so there is no --loglevel-audit.
const LOG_CHANNELS = [
  'main', 'certificates', 'coroutines', 'developer', 'devices',
  'equipment', 'exceptions', 'messages', 'mqtt', 'navigation', 'options',
  'platform', 'profiling', 'protocol', 'scraping', 'serial', 'signals', 'web',
];

const HOST = '127.0.0.1';
const PORT = Number(process.env.AQUALINK_TEST_PORT ?? 18080);

// TLS mode: when AQUALINK_TLS=enabled, serve the UI + API over HTTPS instead of
// plain HTTP (the app self-provisions a self-signed cert when --https-port is set
// and --disable-https is absent). This exercises the TLS/SSL session path in the
// HTTP server, the certificate self-provisioning, and the HTTPS bootstrap branch
// — none of which the plain-HTTP default run reaches. The general (non-identity)
// spec suite runs unchanged against the HTTPS origin; Chromium is told to accept
// the self-signed cert via `ignoreHTTPSErrors`.
const TLS_MODE = process.env.AQUALINK_TLS === 'enabled';
const SCHEME = TLS_MODE ? 'https' : 'http';
const BASE_URL = `${SCHEME}://${HOST}:${PORT}`;

// Open-bind mode: when AQUALINK_OPEN_BIND is set, bind a NON-loopback address
// (0.0.0.0) with auth off, which trips the app's "equipment-control API exposed
// without authentication" start-up guard. AQUALINK_OPEN_BIND=warn exercises the
// prominent-warning branch; =ack additionally passes --insecure-no-auth to
// exercise the acknowledged-posture branch. The browser still reaches the server
// via loopback (0.0.0.0 accepts it), so the default spec suite runs unchanged.
const OPEN_BIND = process.env.AQUALINK_OPEN_BIND;   // undefined | 'warn' | 'ack'
const BIND_ADDRESS = OPEN_BIND ? '0.0.0.0' : HOST;

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

// MQTT mode: when AQUALINK_MQTT=enabled, start a loopback MQTT broker
// (e2e/support/mqtt-broker.mjs) and boot the app with --mqtt + --home-assistant
// pointed at it. This is the only launch mode that exercises the MQTT layer —
// MqttClient connect/CONNACK/recv-loop/flush, MqttIntegration state publishing,
// MqttHub, and Home-Assistant auto-discovery — none of which the broker-less
// default reaches. Short publish intervals keep the publish/flush paths busy.
// Runs ONLY mqtt.spec.ts (it asserts a live broker connection).
const MQTT_MODE = process.env.AQUALINK_MQTT === 'enabled';
const MQTT_PORT = Number(process.env.AQUALINK_MQTT_PORT ?? 11883);

// Persistence modes: point the equipment cache / preferences at real files so
// their load-or-init + write-scheduling paths run (the default in-memory run
// skips them). Both use the normal spec suite; they only add a --file flag.
const EQUIPMENT_CACHE = process.env.AQUALINK_EQUIPMENT_CACHE;
const PREFERENCES_FILE = process.env.AQUALINK_PREFERENCES_FILE;

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

// The loopback MQTT broker Playwright starts alongside the app in MQTT mode.
// Playwright waits for its TCP port to accept connections before starting the
// app, so the MqttClient's first connect attempt lands on a live broker.
const brokerServer = {
  command: `node "${join(ROOT, 'e2e', 'support', 'mqtt-broker.mjs')}"`,
  port: MQTT_PORT,
  reuseExistingServer: false,
  stdout: 'pipe' as const,
  stderr: 'pipe' as const,
  env: { AQUALINK_MQTT_PORT: String(MQTT_PORT) },
  gracefulShutdown: { signal: 'SIGTERM' as const, timeout: 5_000 },
};

export default defineConfig({
  testDir: './e2e',
  // The identity specs (auth + admin + guest) need an identity-enabled server
  // (AQUALINK_AUTH_MODE=enabled); everything else needs an unauthenticated one.
  //
  // IMPORTANT: the identity specs have INCOMPATIBLE store preconditions and share
  // one webServer + auth-state dir per run — auth.spec's first test needs an EMPTY
  // store to exercise the first-run setup wizard, while admin.spec self-seeds an
  // admin (setup-if-empty, else login). Run them in SEPARATE invocations so each
  // gets its own fresh state dir:
  //   AQUALINK_AUTH_MODE=enabled npx playwright test e2e/auth.spec.ts
  //   AQUALINK_AUTH_MODE=enabled npx playwright test e2e/admin.spec.ts
  //   AQUALINK_AUTH_MODE=enabled npx playwright test e2e/guest.spec.ts
  // (the positional filter narrows the match to one file). A bare auth-mode run
  // would run admin.spec first (alphabetical), seeding the store and breaking
  // auth.spec's wizard assertion — which is why CI drives one spec per step.
  testMatch: AUTH_MODE
    ? ['**/auth.spec.ts', '**/admin.spec.ts', '**/guest.spec.ts']
    : (HISTORY_DB ? ['**/trends.spec.ts'] : (SCHEDULES_FILE ? ['**/schedules.spec.ts'] : (MQTT_MODE ? ['**/mqtt.spec.ts'] : undefined))),
  testIgnore: (AUTH_MODE || HISTORY_DB || SCHEDULES_FILE || MQTT_MODE) ? undefined : ['**/auth.spec.ts', '**/admin.spec.ts', '**/guest.spec.ts', '**/mqtt.spec.ts'],
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
    // In TLS mode the server presents a self-signed certificate; accept it so the
    // browser can reach the HTTPS origin under test.
    ignoreHTTPSErrors: TLS_MODE,
  },

  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],

  // In MQTT mode the broker is started first (Playwright waits for its port),
  // then the app; otherwise just the app.
  webServer: [
    ...(MQTT_MODE ? [brokerServer] : []),
    {
    command: [
      `"${APP_EXE}"`,
      '--dev-mode',
      `--replay-filename "${REPLAY_FIXTURE}"`,
      // TLS mode serves HTTPS only (self-signed cert auto-provisioned); the plain
      // default serves HTTP only. --disable-http conflicts with --http-port and
      // --disable-https conflicts with --https-port, so each mode passes exactly
      // the port flag for its transport plus the disable for the other.
      ...(TLS_MODE
        ? [`--https-port ${PORT}`, '--disable-http']
        : [`--http-port ${PORT}`, '--disable-https']),
      `--address ${BIND_ADDRESS}`,
      ...(OPEN_BIND === 'ack' ? ['--insecure-no-auth'] : []),
      `--doc-root "${DOC_ROOT}"`,
      '--jandy-disable-emulation',
      '--profiler tracy',
      ...LOG_CHANNELS.map((ch) => `--loglevel-${ch} info`),
      ...(AUTH_MODE
        ? [
            '--auth-mode enabled',
            `--auth-state-dir "${AUTH_STATE_DIR}"`,
            ...(BOOTSTRAP_ADMIN ? [`--bootstrap-admin ${BOOTSTRAP_ADMIN}`] : []),
          ]
        : []),
      ...(HISTORY_DB ? [`--history-db "${HISTORY_DB}"`, '--history-flush-seconds 1'] : []),
      ...(SCHEDULES_FILE ? [`--schedules-file "${SCHEDULES_FILE}"`] : []),
      // MQTT mode: connect to the loopback broker + enable HA discovery, with
      // 1s status/stats intervals so the publish + flush paths run repeatedly
      // during the spec (they are otherwise on a multi-second cadence).
      ...(MQTT_MODE
        ? [
            '--mqtt',
            `--mqtt-host ${HOST}`,
            `--mqtt-port ${MQTT_PORT}`,
            '--home-assistant',
            '--mqtt-status-interval 1',
            '--mqtt-stats-interval 1',
          ]
        : []),
      ...(EQUIPMENT_CACHE ? [`--equipment-cache-file "${EQUIPMENT_CACHE}"`] : []),
      ...(PREFERENCES_FILE ? [`--preferences-file "${PREFERENCES_FILE}"`] : []),
    ].join(' '),
    url: BASE_URL,
    // The HTTPS readiness probe must accept the self-signed cert too, or the
    // webServer would never be considered "up" in TLS mode.
    ignoreHTTPSErrors: TLS_MODE,
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
  ],
});
