import { test, expect, Page } from '@playwright/test';

/**
 * I18n runtime + guard-rail tests (see docs/i18n.md).
 *
 * Two jobs:
 *   1. Verify the translation machinery: lookup, placeholders, plurals,
 *      fallback, the Settings language picker, persistence, catalog parity,
 *      and the <html lang>/<html dir> contract (Arabic flips to RTL).
 *   2. Guard against regressions as features evolve:
 *      - "no missing keys" — walk every route and fail on any
 *        "[i18n] missing catalog key" console warning (a t()/$t() call whose
 *        key was never added to en.js);
 *      - "pseudo-locale scan" — switch to a runtime-generated locale whose
 *        every value is wrapped in ⟦…⟧, walk every route, and fail on visible
 *        UI text that did NOT come through the catalog (a brand-new hardcoded
 *        string that bypassed i18n entirely).
 */

const ROUTES = ['dashboard', 'detailed', 'trends', 'schedules', 'settings', 'about', 'diagnostics'];

async function walkRoutes(page: Page) {
  for (const route of ROUTES) {
    await page.evaluate((r) => { window.location.hash = '#' + r; }, route);
    // Give Alpine a beat to render the newly-shown view (x-show templates are
    // already in the DOM; x-if templates mount on first show).
    await page.waitForTimeout(500);
  }
}

test.describe('i18n runtime', () => {
  test('lookup, placeholders, plurals and fallback behave', async ({ page }) => {
    await page.goto('/');
    const r = await page.evaluate(() => {
      const store = (window as any).Alpine.store('i18n');
      const api = (window as any).AquaI18n;
      return {
        simple: store.t('nav.dashboard'),
        placeholder: store.t('dash.set_value', { value: '28 °C' }),
        pluralOne: store.tn('sched.conflicts', 1),
        pluralOther: store.tn('sched.conflicts', 3),
        fallbackReturnsKey: store.t('definitely.not_a_key'),
        bridgeMatchesStore: api.t('nav.dashboard') === store.t('nav.dashboard'),
        locales: store.locales.map((l: any) => l.code),
      };
    });
    expect(r.simple).toBe('Dashboard');
    expect(r.placeholder).toBe('Set 28 °C');
    expect(r.pluralOne).toBe('1 schedule conflicts with a controller program');
    expect(r.pluralOther).toBe('3 schedules conflict with a controller program');
    expect(r.fallbackReturnsKey).toBe('definitely.not_a_key');
    expect(r.bridgeMatchesStore).toBe(true);
    expect(r.locales).toEqual(expect.arrayContaining(['en', 'de', 'ar', 'ja']));
  });

  test('defaults to English with LTR document attributes', async ({ page }) => {
    await page.goto('/');
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
    await expect(page.locator('html')).toHaveAttribute('dir', 'ltr');
    await expect(page.locator('.nav-link').first()).toHaveText('Dashboard');
  });

  test('language picker switches to German, persists, and survives reload', async ({ page }) => {
    await page.goto('/#settings');
    // The picker lives in Settings → Appearance; switching re-renders every binding.
    await page.locator('.settings-card select.settings-input').first().selectOption('de');
    const deDashboard = await page.evaluate(() => (window as any).AquaI18n.catalogs.de['nav.dashboard']);
    await expect(page.locator('.nav-link').first()).toHaveText(deDashboard);
    await expect(page.locator('html')).toHaveAttribute('lang', 'de');
    expect(await page.evaluate(() => localStorage.getItem('locale'))).toBe('de');

    await page.reload();
    await expect(page.locator('html')).toHaveAttribute('lang', 'de');
    await expect(page.locator('.nav-link').first()).toHaveText(deDashboard);

    // The saved locale's catalog must load SYNCHRONOUSLY at boot (parser-
    // inserted via document.write), not via async injection — async loading
    // renders an English first paint and freezes boot-time toasts in English.
    // A parser-inserted script has async === false; a dynamically created one
    // defaults to async === true.
    const syncLoaded = await page.evaluate(() => {
      const s = document.querySelector('script[src="/i18n/de.js"]') as HTMLScriptElement | null;
      return s ? s.async === false : false;
    });
    expect(syncLoaded, 'saved locale catalog must be parser-loaded at boot (no English flash)').toBe(true);

    // Restore English so later tests start clean.
    await page.evaluate(async () => { await (window as any).Alpine.store('i18n').setLocale('en'); localStorage.removeItem('locale'); });
  });

  test('Arabic flips the document to RTL and back', async ({ page }) => {
    await page.goto('/');
    await page.evaluate(async () => { await (window as any).Alpine.store('i18n').setLocale('ar'); });
    await expect(page.locator('html')).toHaveAttribute('dir', 'rtl');
    await expect(page.locator('html')).toHaveAttribute('lang', 'ar');
    await page.evaluate(async () => { await (window as any).Alpine.store('i18n').setLocale('en'); localStorage.removeItem('locale'); });
    await expect(page.locator('html')).toHaveAttribute('dir', 'ltr');
  });

  test('every shipped locale has full key parity with English', async ({ page }) => {
    await page.goto('/');
    const result = await page.evaluate(async () => {
      const store = (window as any).Alpine.store('i18n');
      const api = (window as any).AquaI18n;
      const problems: string[] = [];
      const enKeys = Object.keys(api.catalogs.en);
      for (const loc of store.locales) {
        if (loc.code === 'en') continue;
        await store.setLocale(loc.code); // loads the catalog on demand
        const cat = api.catalogs[loc.code];
        if (!cat) { problems.push(`${loc.code}: catalog failed to load`); continue; }
        const keys = new Set(Object.keys(cat));
        for (const k of enKeys) if (!keys.has(k)) problems.push(`${loc.code}: missing ${k}`);
        for (const k of keys) if (!enKeys.includes(k)) problems.push(`${loc.code}: extra ${k}`);
      }
      await store.setLocale('en');
      localStorage.removeItem('locale');
      return problems;
    });
    expect(result).toEqual([]);
  });
});

test.describe('i18n guard rails', () => {
  test('no missing catalog keys anywhere in the UI', async ({ page }) => {
    const missing: string[] = [];
    page.on('console', (msg) => {
      if (msg.text().includes('[i18n] missing catalog key')) missing.push(msg.text());
    });
    await page.goto('/');
    await walkRoutes(page);
    // Open the diagnostics modals too — their templates mount on demand.
    await page.evaluate(() => { window.location.hash = '#diagnostics'; });
    await page.waitForTimeout(500);
    const logBtn = page.locator('.log-configure-btn');
    if (await logBtn.isEnabled().catch(() => false)) {
      await logBtn.click();
      await page.waitForTimeout(300);
      await page.keyboard.press('Escape');
    }
    expect(missing).toEqual([]);
  });

  test('pseudo-locale scan: all visible UI text comes through the catalog', async ({ page }) => {
    await page.goto('/');

    // Register a runtime pseudo-locale: every English value wrapped in ⟦…⟧.
    await page.evaluate(async () => {
      const api = (window as any).AquaI18n;
      const xx: Record<string, string> = {};
      for (const [k, v] of Object.entries(api.catalogs.en)) xx[k] = '⟦' + v + '⟧';
      api.catalogs.xx = xx;
      api.SUPPORTED_LOCALES.push({ code: 'xx', name: 'Pseudo', dir: 'ltr' });
      await (window as any).Alpine.store('i18n').setLocale('xx');
    });

    await walkRoutes(page);

    const offenders = await page.evaluate(() => {
      // Elements whose text legitimately does NOT come from the catalog:
      // server/device-originated data, technical tokens, and the brand.
      const EXEMPT_SELECTOR = [
        'code', 'pre', 'script', 'style', 'option',
        '[data-i18n-exempt]',
        '.nav-brand', '.login-title',                       // brand
        '.heater-label', '.eq-control-label', '.other-equip-label', // device labels (API)
        '.sched-name', '.sched-trk-label', '.sched-meta',   // schedule/device names + summaries incl. targets
        '.sched-conflict-item', '.sched-row-conflict-msg',  // embed device names
        '.dmc-name', '.dmc-addr', '.dmc-summary', '.diag-modal-summary', // device diagnostics data
        '.diag-screen', '.diag-raw', '.diag-cmd-desc', '.diag-cmd-outcome',
        '.mstat-type', '.log-chan-name', '.logsum-chip', '.log-seg-btn', // wire tokens (message types, channels, severities)
        '.logsum-global-val', '.badge', '.mqtt-val', '.kv-v', '.health-kv :last-child',
        '.matter-kv :last-child', '.spaside-key-fn', '.prof-backend-btn', '.prof-state',
        '.accent-swatch-name',                              // theme names (client config values)
        '.trends-chip', '.trends-stat-label', '.trends-readout',
        '.rec-input-prefix',                                // literal path fragment
        '.health-endpoint', '.sched-empty-pill', '.trends-empty-pill', // endpoint literals
        '.info-grid dd',                                    // About values (server version data / brand fallback)
        '.trends-error',                                    // snapshot error string (translated at assignment time)
      ].join(',');
      // Token shapes that are legitimately untranslated wherever they appear.
      const TOKEN_RE = /^(?:[-–—·.,:;()%°#'"\/\\+\d\s]|GET|POST|PUT|HTTP|API|MQTT|TLS|SWG|ORP|pH|ppm|mV|µs|ms|kB|KB|MB|GB|B|P\d+|v\d[\w.]*|0x[0-9a-fA-F]+|--|…)+$/;

      const bad = new Set<string>();
      const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
      let node: Node | null;
      while ((node = walker.nextNode())) {
        const text = (node.textContent || '').trim();
        // Either bracket proves catalog origin: an x-html value with inner
        // <code> tags splits into text nodes that carry only one of the pair.
        if (!text || text.includes('⟦') || text.includes('⟧')) continue;
        if (!/[A-Za-z]{3,}/.test(text)) continue;               // numbers / punctuation / short tokens
        if (TOKEN_RE.test(text)) continue;
        const el = node.parentElement;
        if (!el) continue;
        const style = window.getComputedStyle(el);
        if (style.display === 'none' || style.visibility === 'hidden') continue;
        if (el.closest(EXEMPT_SELECTOR)) continue;
        if (el.closest('[x-cloak]')) continue;                  // never shown in this state
        bad.add(`${el.tagName.toLowerCase()}.${el.className}: "${text.slice(0, 80)}"`);
      }
      return [...bad].sort();
    });

    expect(offenders, 'visible text that bypassed the i18n catalog — add it to en.js (and every locale) or mark the element data-i18n-exempt if it is server/device data').toEqual([]);

    // Clean up.
    await page.evaluate(async () => {
      await (window as any).Alpine.store('i18n').setLocale('en');
      localStorage.removeItem('locale');
    });
  });
});
