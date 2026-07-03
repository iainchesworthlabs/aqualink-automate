#!/usr/bin/env node
/**
 * Regenerate the documentation screenshots in docs/assets/.
 *
 * The images embedded in README.md, docs/SECURITY.md, docs/usage-and-api.md,
 * docs/index.md and docs/CONTRIBUTING.md are treated like any other doc: when a
 * UI change makes them stale (layout, theme, navigation, new/renamed views,
 * auth screens), re-run this script and commit the refreshed PNGs alongside
 * the change.
 *
 * Everything is driven by the REAL application binary replaying recorded
 * RS-485 fixtures (test/fixtures/*.cap) — the same harness the Playwright e2e
 * suite uses — so the pictures show genuine data paths:
 *   - equipment-rich dashboard  <- onetouch_equipment_toggle.cap
 *   - live chemistry (3200 ppm) <- sample_session.cap (AquaRite session)
 *   - schedules                 <- seeded through the real /api/schedules API
 *   - trends                    <- representative series stubbed at the
 *                                  network layer (the fixtures record no
 *                                  temperature history); keys use the
 *                                  canonical names from trends-view.js
 *   - auth screens              <- --auth-mode enabled against a fresh,
 *                                  throw-away state dir
 *
 * Usage (from the repo root, Node >= 20 with dev deps installed):
 *
 *   AQUALINK_EXE=<path-to-built-exe> node scripts/capture-doc-screenshots.js
 *   node scripts/capture-doc-screenshots.js --only hero,trends
 *
 * AQUALINK_EXE defaults to build/wt/src/aqualink-automate.exe, matching
 * playwright.config.ts; AQUALINK_DOC_ROOT overrides the web assets served
 * (defaults to this repo's assets/web). Playwright browsers must be
 * installed (`npx playwright install chromium`).
 *
 * Shots: hero, trends, schedules, settings, about, rtl, auth (wizard +
 * admin + account menu + login).
 */
const { chromium } = require('@playwright/test');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const OUT = path.join(ROOT, 'docs', 'assets');
const DOC_ROOT = process.env.AQUALINK_DOC_ROOT ?? path.join(ROOT, 'assets', 'web');
const FIXTURES = path.join(ROOT, 'test', 'fixtures');
const EXE = process.env.AQUALINK_EXE ?? path.join(ROOT, 'build', 'wt', 'src', 'aqualink-automate.exe');
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), 'aqualink-doc-shots-'));

const ONLY = (() => {
  const arg = process.argv.find((a) => a.startsWith('--only'));
  if (!arg) return null;
  const v = arg.includes('=') ? arg.split('=')[1] : process.argv[process.argv.indexOf(arg) + 1];
  return new Set((v ?? '').split(',').map((s) => s.trim()).filter(Boolean));
})();
const want = (name) => !ONLY || ONLY.has(name);

if (!fs.existsSync(EXE)) {
  console.error(`Application binary not found at ${EXE}.\nBuild it first or set AQUALINK_EXE.`);
  process.exit(1);
}

// --replay-filename depends on --dev-mode, --profiler and every per-channel
// log level (see playwright.config.ts for the full explanation).
const LOG_CHANNELS = [
  'audit', 'main', 'certificates', 'coroutines', 'developer', 'devices',
  'equipment', 'exceptions', 'messages', 'mqtt', 'navigation', 'options',
  'platform', 'profiling', 'protocol', 'scraping', 'serial', 'signals', 'web',
];

function startApp(port, fixture, extraArgs, logName) {
  const log = fs.createWriteStream(path.join(SCRATCH, logName));
  const child = spawn(EXE, [
    '--dev-mode',
    '--replay-filename', path.join(FIXTURES, fixture),
    '--http-port', String(port),
    '--address', '127.0.0.1',
    '--disable-https',
    '--doc-root', DOC_ROOT,
    '--jandy-disable-emulation',
    '--profiler', 'tracy',
    ...LOG_CHANNELS.flatMap((ch) => [`--loglevel-${ch}`, 'info']),
    ...extraArgs,
  ], { cwd: path.dirname(EXE), stdio: ['ignore', 'pipe', 'pipe'] });
  child.stdout.pipe(log);
  child.stderr.pipe(log);
  return child;
}

async function waitReady(port, timeoutMs = 90_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      if ((await fetch(`http://127.0.0.1:${port}/`)).ok) return;
    } catch { /* not up yet */ }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`app on :${port} not ready after ${timeoutMs}ms`);
}

async function withApp(port, fixture, extraArgs, logName, fn) {
  const app = startApp(port, fixture, extraArgs, logName);
  try {
    await waitReady(port);
    await fn(`http://127.0.0.1:${port}`);
  } finally {
    app.kill();
    await new Promise((r) => setTimeout(r, 2000));
  }
}

async function newPage(browser, baseURL, viewport = { width: 1280, height: 800 }) {
  const ctx = await browser.newContext({
    baseURL,
    viewport,
    deviceScaleFactor: 2,          // crisp on high-DPI doc renderings
    serviceWorkers: 'block',       // so page.route() can stub /api fetches
  });
  return { ctx, page: await ctx.newPage() };
}

const png = (name) => ({ path: path.join(OUT, name) });
const failures = [];
async function shot(name, fn) {
  try {
    await fn();
    console.log(`  ok  ${name}`);
  } catch (e) {
    failures.push(`${name}: ${e.message.split('\n')[0]}`);
    console.log(`  FAIL ${name}: ${e.message.split('\n')[0]}`);
  }
}

// Wait for the dashboard to show live replay data (locale-dependent text —
// call only while the UI is in English).
async function waitLiveDashboard(page) {
  await page.locator('.section-title', { hasText: 'Water Chemistry' }).waitFor({ timeout: 20_000 });
  const saltCard = page.locator('.gauge-card').filter({
    has: page.locator('.gauge-label', { hasText: 'Salt' }),
  });
  await saltCard.locator('.chem-dial-num').filter({ hasText: /\d/ }).waitFor({ timeout: 20_000 });
  await page.waitForTimeout(750);
}

// ---------------------------------------------------------------------------
// Hero dashboard — OneTouch fixture (12 aux devices, temps, setpoints).
async function captureHero(browser) {
  console.log('--- hero dashboard (onetouch_equipment_toggle.cap) ---');
  await withApp(18190, 'onetouch_equipment_toggle.cap', [], 'app-hero.log', async (base) => {
    // Let the replay drain first so a late WS frame cannot undo the DOM
    // patches below.
    await new Promise((r) => setTimeout(r, 12_000));
    const { ctx, page } = await newPage(browser, base, { width: 1280, height: 1500 });
    await shot('aqualink-automate-dashboard.png', async () => {
      await page.goto('/');
      await waitLiveDashboard(page);
      await page.waitForTimeout(1500);
      await page.evaluate(() => {
        // Dismiss any alert toasts so the hero is clean.
        const toasts = document.querySelector('.toast-container');
        if (toasts) toasts.style.display = 'none';
        // The spa is off in this capture; the raw wire value (1 degree) is a
        // pump-off sentinel, so present the honest unknown placeholder.
        for (const el of document.querySelectorAll('*')) {
          if (el.children.length === 0 && /^1°C$/.test(el.textContent.trim())) el.textContent = '--';
        }
      });
      await page.waitForTimeout(300);
      await page.screenshot(png('aqualink-automate-dashboard.png'));
    });
    await ctx.close();
  });
}

// ---------------------------------------------------------------------------
// Anonymous instance — trends / schedules / settings / about / RTL.
async function captureAnonymous(browser) {
  console.log('--- web-UI views (sample_session.cap) ---');
  const schedulesFile = path.join(SCRATCH, 'schedules.json');
  await withApp(18191, 'sample_session.cap', ['--schedules-file', schedulesFile], 'app-views.log', async (base) => {
    const { ctx, page } = await newPage(browser, base);

    // Representative history series (canonical keys from trends-view.js:
    // temp/* and chem/salt_ppm auto-select; other keys draw as runtime bars).
    const now = Math.floor(Date.now() / 1000);
    const DAY = 24 * 3600;
    const mk = (fn, n = 144) => Array.from({ length: n }, (_, i) => ({
      ts: now - DAY + Math.round((i * DAY) / (n - 1)), value: fn(i / (n - 1)),
    }));
    const onDuring = (ranges) => (x) => (ranges.some(([a, b]) => x >= a && x <= b) ? 1 : 0);
    const series = {
      'temp/pool': { unit: 'C', label: 'Pool', points: mk((x) => +(24.5 + 3.3 * Math.exp(-((x - 0.62) ** 2) / 0.045) + 0.35 * Math.sin(x * 11)).toFixed(1)) },
      'temp/air': { unit: 'C', label: 'Air', points: mk((x) => +(16.5 + 10.5 * Math.exp(-((x - 0.58) ** 2) / 0.035) + 0.5 * Math.sin(x * 17)).toFixed(1)) },
      'chem/salt_ppm': { unit: 'ppm', label: 'Salt', points: mk((x) => Math.round(3255 - 85 * x + 20 * Math.sin(x * 6))) },
      'device/filter_pump': { unit: '', label: 'Filter Pump', points: mk(onDuring([[0.27, 0.46], [0.75, 0.875]]), 288) },
      'device/pool_light': { unit: '', label: 'Pool Light', points: mk(onDuring([[0.82, 0.95]]), 288) },
    };
    await page.route('**/api/history/series*', async (route) => {
      const key = new URL(route.request().url()).searchParams.get('key');
      if (!key) {
        await route.fulfill({ json: Object.entries(series).map(([k, s]) => ({
          key: k, unit: s.unit, label: s.label, name: s.label, count: s.points.length,
          first_ts: s.points[0].ts, last_ts: s.points.at(-1).ts,
        })) });
      } else {
        await route.fulfill({ json: { points: series[key]?.points ?? [] } });
      }
    });

    // Schedules that read like a real pool day, addressed at real buttons.
    const buttons = (await (await ctx.request.get(`${base}/api/equipment/buttons`)).json()).buttons ?? [];
    const target = (re, fallback) => (buttons.find((b) => re.test(b.label ?? ''))?.label) ?? fallback;
    for (const s of [
      { name: 'Morning filtration', enabled: true, days_of_week: 127, time_local: '06:30', action: { type: 'button_toggle', target: target(/pump/i, 'Pool Pump') } },
      { name: 'Evening filtration', enabled: true, days_of_week: 127, time_local: '18:00', action: { type: 'button_toggle', target: target(/pump/i, 'Pool Pump') } },
      { name: 'Weekend spa warm-up', enabled: true, days_of_week: 65, time_local: '16:00', action: { type: 'button_toggle', target: target(/spa/i, 'Spa') } },
    ]) {
      const r = await ctx.request.post(`${base}/api/schedules`, { data: s });
      if (!r.ok()) console.log(`  (schedule seed "${s.name}" -> ${r.status()})`);
    }

    await page.goto('/');
    await waitLiveDashboard(page);

    if (want('trends')) await shot('webui-trends.png', async () => {
      await page.locator('.nav-link', { hasText: 'Trends' }).click();
      await page.waitForTimeout(1500);
      for (const label of ['Filter Pump', 'Pool Light']) {
        const chip = page.locator('.trends-chip', { hasText: label }).first();
        if (await chip.count()) await chip.click().catch(() => {});
      }
      await page.waitForTimeout(1500);
      await page.screenshot(png('webui-trends.png'));
    });

    if (want('schedules')) await shot('webui-schedules.png', async () => {
      await page.locator('.nav-link', { hasText: 'Schedules' }).click();
      await page.locator('text=Morning filtration').waitFor({ timeout: 10_000 });
      await page.waitForTimeout(500);
      await page.screenshot(png('webui-schedules.png'));
    });

    if (want('settings')) await shot('webui-settings-language.png', async () => {
      await page.locator('.nav-link', { hasText: 'Settings' }).click();
      await page.locator('text=System Preferences').waitFor({ timeout: 10_000 });
      await page.waitForTimeout(500);
      await page.screenshot(png('webui-settings-language.png'));
    });

    if (want('about')) await shot('webui-about-languages.png', async () => {
      await page.locator('.nav-link', { hasText: 'About' }).click();
      await page.waitForTimeout(750);
      // Only the Language-support card (the software-info card above shows
      // the build's git state, which would churn on every regeneration).
      const card = page.locator('.settings-card, .info-card, section, div')
        .filter({ has: page.getByText('Available languages') }).last();
      await card.screenshot(png('webui-about-languages.png'));
    });

    // RTL LAST: switching mirrors ui.locale into the server prefs, so nothing
    // English-dependent may be captured from this instance afterwards.
    if (want('rtl')) await shot('webui-dashboard-arabic-rtl.png', async () => {
      await page.locator('.nav-link, [class*=nav]').first().waitFor();
      await page.evaluate(async () => { await window.Alpine.store('i18n').setLocale('ar'); });
      await page.locator('html[dir="rtl"]').waitFor({ timeout: 10_000 });
      await page.goto('/');
      await page.locator('html[dir="rtl"]').waitFor({ timeout: 10_000 });
      await page.waitForTimeout(2000);
      await page.screenshot(png('webui-dashboard-arabic-rtl.png'));
    });

    await ctx.close();
  });
}

// ---------------------------------------------------------------------------
// Identity-enabled instance — setup wizard, admin overlay, login card.
async function captureAuth(browser) {
  console.log('--- auth screens (fresh --auth-state-dir) ---');
  const stateDir = path.join(SCRATCH, 'auth-state');
  fs.mkdirSync(stateDir, { recursive: true });
  const ADMIN_USER = 'admin';
  const ADMIN_PASS = 'docs-screenshot-passphrase';

  await withApp(18192, 'sample_session.cap',
    ['--auth-mode', 'enabled', '--auth-state-dir', stateDir], 'app-auth.log', async (base) => {
    const { ctx, page } = await newPage(browser, base);
    const overlay = page.locator('.login-overlay');

    await shot('webui-auth-setup-wizard.png', async () => {
      await page.goto('/');
      await overlay.locator('input[aria-label="Admin username"]').waitFor({ timeout: 15_000 });
      await overlay.locator('input[aria-label="Admin username"]').fill(ADMIN_USER);
      await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
      await overlay.locator('input[aria-label="Confirm password"]').fill(ADMIN_PASS);
      await page.waitForTimeout(300);
      await page.screenshot(png('webui-auth-setup-wizard.png'));
    });

    // Complete the wizard -> live dashboard as the new administrator.
    await overlay.locator('.login-submit').click();
    await overlay.waitFor({ state: 'hidden', timeout: 15_000 });
    await waitLiveDashboard(page);

    await shot('webui-admin-users.png', async () => {
      await page.locator('button[title="Administration"]').click();
      await page.locator('.admin-card').waitFor();
      await page.locator('.admin-tab', { hasText: 'Users' }).click();
      // Seed a second user so the list looks real.
      await page.getByRole('button', { name: 'New user' }).click();
      const panel = page.locator('.admin-panel', { hasText: 'New user' });
      await panel.locator('input[aria-label="Username"]').fill('family');
      await panel.locator('input[aria-label="Password"]').fill('family-docs-passphrase');
      await panel.locator('input[aria-label="Confirm password"]').fill('family-docs-passphrase');
      for (const ent of ['equipment.view', 'equipment.control']) {
        // Exact match: 'equipment.control' must not sweep up the whole
        // 'equipment.control.*' selector family.
        const chip = panel.locator('.ent-actions .admin-check-chip')
          .filter({ has: page.getByText(ent, { exact: true }) }).first();
        if (await chip.count()) await chip.locator('input[type="checkbox"]').check();
      }
      await panel.getByRole('button', { name: 'Create user' }).click();
      await page.locator('section[x-data="adminUsersView()"] .admin-row')
        .filter({ has: page.locator('.admin-row-title', { hasText: 'family' }) })
        .waitFor({ timeout: 10_000 });
      await page.waitForTimeout(400);
      await page.screenshot(png('webui-admin-users.png'));
    });

    await page.keyboard.press('Escape');
    await page.waitForTimeout(300);

    await shot('webui-account-menu.png', async () => {
      await page.locator('button[title="Account"]').click();
      await page.locator('.account-card').waitFor({ timeout: 5_000 });
      // The identity line must show the username, not the subject UUID.
      const identity = (await page.locator('.account-username').textContent())?.trim();
      if (identity !== ADMIN_USER) throw new Error(`account identity shows "${identity}", expected "${ADMIN_USER}"`);
      await page.waitForTimeout(400);
      await page.screenshot(png('webui-account-menu.png'));
      await page.keyboard.press('Escape');
      await page.waitForTimeout(300);
    });

    await shot('webui-auth-login.png', async () => {
      await page.locator('button[title="Account"]').click();
      await page.locator('.account-card').waitFor({ timeout: 5_000 });
      await page.locator('.account-card').getByRole('button', { name: 'Sign out', exact: true }).click();
      await overlay.waitFor({ state: 'visible', timeout: 10_000 });
      await overlay.locator('input[aria-label="Username"]').fill(ADMIN_USER);
      await page.waitForTimeout(300);
      await page.screenshot(png('webui-auth-login.png'));
    });

    await ctx.close();
  });
}

// ---------------------------------------------------------------------------
(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  console.log(`binary:  ${EXE}`);
  console.log(`output:  ${OUT}`);
  console.log(`scratch: ${SCRATCH}`);
  const browser = await chromium.launch();
  try {
    if (want('hero')) await captureHero(browser);
    if (['trends', 'schedules', 'settings', 'about', 'rtl'].some(want)) await captureAnonymous(browser);
    if (want('auth')) await captureAuth(browser);
  } finally {
    await browser.close();
  }
  console.log(failures.length ? `\nFAILURES:\n${failures.join('\n')}` : '\nAll screenshots captured.');
  process.exit(failures.length ? 1 : 0);
})();
