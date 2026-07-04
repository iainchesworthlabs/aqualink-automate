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

if (!fs.existsSync(EXE)) {
  console.error(`Application binary not found at ${EXE}.\nSet AQUALINK_EXE to a built exe.`);
  process.exit(1);
}
fs.mkdirSync(OUT, { recursive: true });

function startApp() {
  const log = fs.createWriteStream(path.join(OUT, 'app.log'));
  const child = spawn(EXE, [
    '--dev-mode',
    '--replay-filename', path.join(FIXTURES, 'onetouch_equipment_toggle.cap'),
    '--http-port', String(PORT),
    '--address', '127.0.0.1',
    '--disable-https',
    '--doc-root', DOC_ROOT,
    '--jandy-disable-emulation',
    '--profiler', 'tracy',
    ...LOG_CHANNELS.flatMap((ch) => [`--loglevel-${ch}`, 'info']),
  ], { cwd: path.dirname(EXE), stdio: ['ignore', 'pipe', 'pipe'] });
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

    for (const vp of VIEWPORTS) {
      const ctx = await browser.newContext({
        baseURL: base,
        viewport: { width: vp.width, height: vp.height },
        deviceScaleFactor: 1,
        serviceWorkers: 'block',
      });
      const page = await ctx.newPage();
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
      await ctx.close();
    }
  } finally {
    if (browser) await browser.close();
    app.kill();
  }
  console.log(`\nDone. Screenshots in ${OUT}`);
})().catch((e) => { console.error(e); process.exit(1); });
