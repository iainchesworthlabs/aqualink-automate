#!/usr/bin/env node
/**
 * capture-responsive-screenshots.js — device-viewport verification harness.
 *
 * Boots the REAL application binary against the OneTouch equipment fixture
 * (dual-body, heaters, aux equipment and setpoints all present) and screenshots
 * each view at phone / tablet / desktop viewports, so the responsive reflow can
 * be eyeballed as it is built. Unlike an interactive browser bridge, Playwright
 * sets a TRUE device-emulated viewport, so the <640 / 640-1023 / >=1024 layouts
 * actually render.
 *
 * This is the bring-forward of the Phase 7 test matrix. Phase 7 turns these
 * captures into e2e/responsive.spec.ts with toHaveScreenshot + layout assertions
 * (nav pattern, grid columns, no horizontal overflow, contrast).
 *
 * Usage (from the worktree root):
 *   AQUALINK_EXE=<built exe> AQUALINK_DOC_ROOT=<assets/web> OUT_DIR=<dir> \
 *   NODE_PATH=<node_modules> node scripts/capture-responsive-screenshots.js \
 *     [--only dashboard,trends]
 */
const { chromium } = require('@playwright/test');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');

const ROOT = path.resolve(__dirname, '..');
const FIXTURES = path.join(ROOT, 'test', 'fixtures');
const EXE = process.env.AQUALINK_EXE ?? path.join(ROOT, 'build', 'wt', 'src', 'aqualink-automate.exe');
const DOC_ROOT = process.env.AQUALINK_DOC_ROOT ?? path.join(ROOT, 'assets', 'web');
const OUT = process.env.OUT_DIR ?? path.join(os.tmpdir(), 'aqualink-responsive-shots');
const PORT = Number(process.env.PORT ?? 18200);

// Auth-mode: boot the identity system with a bootstrapped admin so the login,
// account and admin modals can be triggered and captured. Set AQUALINK_AUTH_MODE=enabled.
const AUTH = process.env.AQUALINK_AUTH_MODE === 'enabled';
const ADMIN_USER = 'admin';
const ADMIN_PW = process.env.AQUALINK_BOOTSTRAP_ADMIN_PASSWORD || 'aqualink-test-pw-123456';
const AUTH_STATE_DIR = AUTH ? fs.mkdtempSync(path.join(os.tmpdir(), 'aqualink-resp-auth-')) : null;

// --replay-filename depends on --dev-mode, --profiler and every per-channel log
// level (matches the manual boot recipe; the profiler falls back to no-op).
const LOG_CHANNELS = [
  'audit', 'main', 'certificates', 'coroutines', 'developer', 'devices',
  'equipment', 'exceptions', 'messages', 'mqtt', 'navigation', 'options',
  'platform', 'profiling', 'protocol', 'scraping', 'serial', 'signals', 'web',
];

const VIEWPORTS = [
  { name: 'phone-se', width: 375, height: 667 },
  { name: 'phone', width: 390, height: 780 },
  { name: 'tablet-portrait', width: 820, height: 1180 },
  { name: 'tablet-landscape', width: 1180, height: 820 },
  { name: 'desktop', width: 1280, height: 900 },
];
const VIEWS = ['dashboard', 'detailed', 'trends', 'schedules', 'settings', 'diagnostics', 'about'];

const ONLY = (() => {
  const i = process.argv.indexOf('--only');
  if (i === -1) return null;
  return new Set((process.argv[i + 1] || '').split(',').map((s) => s.trim()).filter(Boolean));
})();
const wantView = (v) => !ONLY || ONLY.has(v);

// The replay fixture carries no recorded history and no schedules, so Trends and
// Schedules render empty. Inject synthetic data (ported from
// capture-doc-screenshots.js): stub /api/history/series per page for Trends, and
// seed a few schedules via the API (persisted with --schedules-file) for Schedules.
const SCHEDULES_FILE = path.join(OUT, 'schedules.json');
const NOW = Math.floor(Date.now() / 1000);
const DAY = 24 * 3600;
const mk = (fn, n = 144) => Array.from({ length: n }, (_, i) => ({ ts: NOW - DAY + Math.round((i * DAY) / (n - 1)), value: fn(i / (n - 1)) }));
const onDuring = (ranges) => (x) => (ranges.some(([a, b]) => x >= a && x <= b) ? 1 : 0);
const SERIES = {
  'temp/pool': { unit: 'C', label: 'Pool', points: mk((x) => +(24.5 + 3.3 * Math.exp(-((x - 0.62) ** 2) / 0.045)).toFixed(1)) },
  'temp/air': { unit: 'C', label: 'Air', points: mk((x) => +(16.5 + 10.5 * Math.exp(-((x - 0.58) ** 2) / 0.035)).toFixed(1)) },
  'chem/salt_ppm': { unit: 'ppm', label: 'Salt', points: mk((x) => Math.round(3255 - 85 * x)) },
  'device/filter_pump': { unit: '', label: 'Filter Pump', points: mk(onDuring([[0.27, 0.46], [0.75, 0.875]]), 288) },
  'device/pool_light': { unit: '', label: 'Pool Light', points: mk(onDuring([[0.82, 0.95]]), 288) },
};
async function stubHistory(page) {
  await page.route('**/api/history/series*', async (route) => {
    const key = new URL(route.request().url()).searchParams.get('key');
    if (!key) {
      await route.fulfill({ json: Object.entries(SERIES).map(([k, s]) => ({
        key: k, unit: s.unit, label: s.label, name: s.label, count: s.points.length,
        first_ts: s.points[0].ts, last_ts: s.points.at(-1).ts,
      })) });
    } else {
      await route.fulfill({ json: { points: SERIES[key]?.points ?? [] } });
    }
  });
}
async function seedSchedules(base) {
  try {
    const buttons = (await (await fetch(`${base}/api/equipment/buttons`)).json()).buttons ?? [];
    const target = (re, fb) => (buttons.find((b) => re.test(b.label ?? ''))?.label) ?? fb;
    const rows = [
      { name: 'Morning filtration', enabled: true, days_of_week: 127, time_local: '06:30', action: { type: 'button_toggle', target: target(/pump/i, 'Filter Pump') } },
      { name: 'Evening filtration', enabled: true, days_of_week: 127, time_local: '18:00', action: { type: 'button_toggle', target: target(/pump/i, 'Filter Pump') } },
      { name: 'Weekend spa warm-up', enabled: true, days_of_week: 65, time_local: '16:00', action: { type: 'button_toggle', target: target(/spa/i, 'Spa') } },
    ];
    for (const s of rows) {
      const r = await fetch(`${base}/api/schedules`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(s) });
      if (!r.ok) console.log(`  (schedule seed "${s.name}" -> ${r.status})`);
    }
  } catch (e) { console.log('  (schedule seed skipped: ' + e.message + ')'); }
}

// Auth-mode: capture the login, account and admin modals at each viewport.
async function captureAuthModals(browser, base) {
  for (const vp of VIEWPORTS) {
    const ctx = await browser.newContext({ baseURL: base, viewport: { width: vp.width, height: vp.height }, deviceScaleFactor: 1, serviceWorkers: 'block' });
    const page = await ctx.newPage();
    await page.goto('/');
    // Login overlay appears (identity on, not authenticated).
    await page.locator('.login-overlay .login-card').waitFor({ timeout: 15000 }).catch(() => {});
    await page.waitForTimeout(700);
    await page.screenshot({ path: path.join(OUT, `modal-login-${vp.name}.png`) });
    console.log(`ok  modal-login-${vp.name}`);
    // Sign in as the bootstrapped admin.
    const inputs = page.locator('.login-overlay .login-input');
    if ((await inputs.count()) >= 2) {
      await inputs.nth(0).fill(ADMIN_USER);
      await inputs.nth(1).fill(ADMIN_PW);
      await page.locator('.login-overlay .login-submit').first().click();
      await page.locator('.login-overlay').waitFor({ state: 'hidden', timeout: 15000 }).catch(() => {});
      await page.waitForTimeout(1200);
    }
    // Account modal.
    await page.evaluate(() => window.dispatchEvent(new CustomEvent('account:open')));
    await page.waitForTimeout(800);
    await page.screenshot({ path: path.join(OUT, `modal-account-${vp.name}.png`) });
    console.log(`ok  modal-account-${vp.name}`);
    await page.keyboard.press('Escape');
    await page.waitForTimeout(400);
    // Admin modal.
    await page.evaluate(() => window.dispatchEvent(new CustomEvent('admin:open')));
    await page.waitForTimeout(1000);
    await page.screenshot({ path: path.join(OUT, `modal-admin-${vp.name}.png`) });
    console.log(`ok  modal-admin-${vp.name}`);
    await ctx.close();
  }
}

if (!fs.existsSync(EXE)) {
  console.error(`Application binary not found at ${EXE}.\nSet AQUALINK_EXE to a built exe.`);
  process.exit(1);
}
fs.mkdirSync(OUT, { recursive: true });

function startApp() {
  const log = fs.createWriteStream(path.join(OUT, 'app.log'));
  try { fs.rmSync(SCHEDULES_FILE, { force: true }); } catch { /* fresh each run */ }
  const child = spawn(EXE, [
    '--dev-mode',
    '--replay-filename', path.join(FIXTURES, 'onetouch_equipment_toggle.cap'),
    '--http-port', String(PORT),
    '--address', '127.0.0.1',
    '--disable-https',
    '--doc-root', DOC_ROOT,
    '--schedules-file', SCHEDULES_FILE,
    '--jandy-disable-emulation',
    '--profiler', 'tracy',
    ...LOG_CHANNELS.flatMap((ch) => [`--loglevel-${ch}`, 'info']),
    ...(AUTH ? ['--auth-mode', 'enabled', '--auth-state-dir', AUTH_STATE_DIR, '--bootstrap-admin', ADMIN_USER] : []),
  ], {
    cwd: path.dirname(EXE),
    stdio: ['ignore', 'pipe', 'pipe'],
    env: AUTH ? { ...process.env, AQUALINK_BOOTSTRAP_ADMIN_PASSWORD: ADMIN_PW } : process.env,
  });
  child.stdout.pipe(log);
  child.stderr.pipe(log);
  return child;
}

async function waitReady(timeoutMs = 90_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try { if ((await fetch(`http://127.0.0.1:${PORT}/`)).ok) return; } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`app on :${PORT} not ready after ${timeoutMs}ms`);
}

(async () => {
  const app = startApp();
  let browser;
  try {
    await waitReady();
    // Let the replay drain so a late WS frame cannot undo the DOM before capture.
    await new Promise((r) => setTimeout(r, 10_000));
    browser = await chromium.launch();
    const base = `http://127.0.0.1:${PORT}`;
    if (AUTH) {
      await captureAuthModals(browser, base);
    } else {
    await seedSchedules(base);

    for (const vp of VIEWPORTS) {
      const ctx = await browser.newContext({
        baseURL: base,
        viewport: { width: vp.width, height: vp.height },
        deviceScaleFactor: 1,
        serviceWorkers: 'block',
      });
      const page = await ctx.newPage();
      await stubHistory(page);
      // Load once, then hash-navigate between views.
      await page.goto('/#dashboard');
      await page.waitForTimeout(2500);
      for (const view of VIEWS) {
        if (!wantView(view)) continue;
        await page.evaluate((v) => { window.location.hash = '#' + v; }, view);
        await page.waitForTimeout(1400);
        await page.evaluate(() => { const t = document.querySelector('.toast-container'); if (t) t.style.display = 'none'; });
        await page.waitForTimeout(150);
        const file = path.join(OUT, `${view}-${vp.name}.png`);
        await page.screenshot({ path: file, fullPage: true });
        console.log(`ok  ${view}-${vp.name}`);
      }

      // Modals (run only with `--only modals`): the non-auth-gated ones we can
      // trigger without the identity system. Auth-gated login/account/admin need
      // AQUALINK_AUTH_MODE and are verified separately.
      if (ONLY && ONLY.has('modals')) {
        await page.evaluate(() => { window.location.hash = '#schedules'; });
        await page.waitForTimeout(1200);
        const nb = page.locator('.sched-new-btn').first();
        if (await nb.count()) {
          await nb.click();
          await page.waitForTimeout(700);
          await page.screenshot({ path: path.join(OUT, `modal-schedule-${vp.name}.png`) });
          console.log(`ok  modal-schedule-${vp.name}`);
          await page.keyboard.press('Escape');
          await page.waitForTimeout(300);
        }
      }
      await ctx.close();
    }
    }
  } finally {
    if (browser) await browser.close();
    app.kill();
  }
  console.log(`\nDone. Screenshots in ${OUT}`);
})().catch((e) => { console.error(e); process.exit(1); });
