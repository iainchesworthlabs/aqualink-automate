import { test, expect } from '@playwright/test';

/**
 * E2E test 1 — UI render from the replay fixture.
 *
 * The app is booted (by playwright.config.ts `webServer`) against
 * test/fixtures/sample_session.cap, which replays an AquaRite salt-water
 * generator reporting 40% output / 3200 PPM salt.  The chlorinator chemistry
 * reaches the browser via the /ws/equipment WebSocket (ChemistryUpdate ->
 * salt_level) and/or the GET /api/equipment REST snapshot (chemistry.salt_ppm).
 *
 * The dashboard renders the salt value in a "Salt" chemistry dial that splits the
 * reading into a numeric node (.chem-dial-num = "3200") and a unit node
 * (.chem-dial-unit = "ppm") — see assets/web/scripts/components/chemistry-gauge.js
 * (salt config: decimals 0, unit ' ppm').
 */
test('dashboard renders the replayed AquaRite salt reading (3200 ppm)', async ({ page }) => {
  // Surface backend/UI console errors in the test log to aid debugging.
  page.on('console', (msg) => {
    if (msg.type() === 'error') console.log('[browser console error]', msg.text());
  });

  await page.goto('/');

  // The dashboard view is the default route.
  await expect(page.locator('.section-title', { hasText: 'Water Chemistry' })).toBeVisible();

  // Locate the Salt gauge card by its label, then read its value cell.
  const saltCard = page
    .locator('.gauge-card')
    .filter({ has: page.locator('.gauge-label', { hasText: 'Salt' }) });
  await expect(saltCard).toBeVisible();

  // The replayed PPM (3200) must appear once the WS/REST feed has been applied.
  // The redesigned dial splits it into a numeric node and a unit node; the
  // number renders with locale digit grouping ("3,200" in the en test locale
  // — see docs/i18n.md value formatting).
  await expect(saltCard.locator('.chem-dial-num')).toHaveText(/3,?200/, { timeout: 15_000 });
  await expect(saltCard.locator('.chem-dial-unit')).toHaveText(/ppm/i);
});

/**
 * Cross-check the data path at the source: the REST snapshot the UI fetches on
 * load must itself carry salt_ppm = 3200.  This isolates a UI-rendering
 * failure from a backend-decode failure if test 1 ever regresses.
 */
test('REST /api/equipment exposes the replayed salt concentration', async ({ request }) => {
  const resp = await request.get('/api/equipment');
  expect(resp.ok()).toBeTruthy();

  const body = await resp.json();
  expect(body).toHaveProperty('chemistry');
  expect(body.chemistry.salt_ppm).toBe(3200);
});
