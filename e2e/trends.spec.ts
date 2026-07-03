import { test, expect } from '@playwright/test';

/**
 * E2E — Trends / history view (WS2).
 *
 * Two modes, selected by AQUALINK_HISTORY_DB in playwright.config.ts:
 *   - default (no history db): the Trends view shows a graceful "disabled"
 *     message (the /api/history/series 503 path).
 *   - history enabled: the replayed AquaRite salt reading is recorded, so the
 *     Trends view lists at least one series and draws the chart canvas.
 */

const HISTORY_ENABLED = !!process.env.AQUALINK_HISTORY_DB;

// The service worker proxies /api fetches (respondWith(fetch(...))), and
// Playwright cannot route SW-originated requests — block it so the
// display-units test's history-API stubs actually intercept.
test.use({ serviceWorkers: 'block' });

test('Trends shows a disabled message when history is off', async ({ page }) => {
  test.skip(HISTORY_ENABLED, 'history is enabled in this run');

  await page.goto('/');
  await page.locator('.nav-link', { hasText: 'Trends' }).click();
  await expect(page.getByText(/History recording is disabled/i)).toBeVisible();
});

test('Trends temperatures follow the Fahrenheit display-units preference', async ({ page, request }) => {
  // The replay fixture records no temperature series, so stub the history API
  // with synthetic °C points; the view converts at display time.
  const now = Math.floor(Date.now() / 1000);
  await page.route('**/api/history/series*', async (route) => {
    const url = new URL(route.request().url());
    if (!url.searchParams.get('key')) {
      await route.fulfill({ json: [{ key: 'temp/pool', unit: 'C', label: '', count: 3, first_ts: now - 600, last_ts: now }] });
      return;
    }
    await route.fulfill({ json: { points: [
      { ts: now - 600, value: 26 }, { ts: now - 300, value: 27 }, { ts: now, value: 28 },
    ] } });
  });

  const put = await request.put('/api/preferences', { data: { temperature_units: 'Fahrenheit' } });
  expect(put.ok()).toBeTruthy();
  try {
    await page.goto('/');
    await page.locator('.nav-link', { hasText: 'Trends' }).click();

    // Chip (currentLabel) and stat strip both flow through _fmt: 28 °C → 82.4 °F.
    await expect(page.locator('.trends-chip-val').first()).toHaveText(/82\.4.*°F/, { timeout: 15_000 });
    await expect(page.locator('.trends-stat-value').first()).toHaveText(/82\.4.*°F/);
    // min 26 °C → 78.8 °F inside the min/max/avg template.
    await expect(page.locator('.trends-stat-sub').first()).toContainText(/78\.8.*°F/);
  } finally {
    await request.put('/api/preferences', { data: { temperature_units: 'Celsius' } });
  }
});

test('Trends lists series and renders the chart when history is on', async ({ page }) => {
  test.skip(!HISTORY_ENABLED, 'history is disabled in this run');

  await page.goto('/');

  // Give the replayed AquaRite salt reading time to be recorded (a chemistry
  // event -> chem/salt_ppm sample), then open the Trends view.
  await page.waitForTimeout(4000);
  await page.locator('.nav-link', { hasText: 'Trends' }).click();

  // At least one metric chip appears and the chart canvas is present.
  await expect(page.locator('.trends-chip').first()).toBeVisible({ timeout: 15_000 });
  await expect(page.locator('.trends-chart-card canvas')).toBeVisible();
});
