import { test, expect } from '@playwright/test';

/**
 * E2E test 2 — command plumbing + chlorinator placement after the dashboard
 * UX redesign.
 *
 * The replay fixture (AquaRite SWG) materialises a single chlorinator auxiliary
 * device labelled "AquaPure" (see aquarite_device.cpp -> EnsureChlorinatorDevice
 * Exists()). In the redesign the chlorinator is configurable-but-not-controllable:
 * it is surfaced in the Water Chemistry section (SWG output tile), NOT as a
 * toggle in Equipment Controls. So we assert:
 *   1. the chlorinator is NOT rendered as a clickable equipment control, and
 *   2. the backend command endpoint still accepts a POST for the device — i.e.
 *      the UI->backend command plumbing is intact (200 toggled, or 503 when the
 *      pool configuration has not been discovered from the minimal fixture).
 *
 * (The browser-click->optimistic-badge path needs a *controllable* device; this
 * SWG-only fixture has none, so dashboard.spec.ts exercises that path only when
 * a controllable device is present.)
 */

const BUTTONS_COMMAND_RE = /\/api\/equipment\/buttons\/[^/]+$/;

test('chlorinator is read-only in chemistry, and the command endpoint still handles a POST', async ({ page }) => {
  await page.goto('/');

  // The chlorinator lives in Water Chemistry now (SWG output), not the controls.
  // The dashboard section title and the Detailed view's card header both read
  // "Water Chemistry" (both are always in the DOM via x-show), so target the
  // dashboard section title specifically.
  await expect(page.locator('.section-title', { hasText: 'Water Chemistry' })).toBeVisible();
  await expect(page.locator('.eq-control').filter({ hasText: /AquaPure/i })).toHaveCount(0);

  // Commands are enabled in replay mode (system reports "ready"). The flag
  // flips when the system status arrives after the auth gate opens — poll
  // instead of sampling once at load.
  await expect
    .poll(async () => page.evaluate(() => {
      // @ts-expect-error Alpine is a global injected by alpine.min.js
      return window.Alpine?.store('system')?.commandsEnabled;
    }), { message: 'system store should report commands enabled in replay mode' })
    .toBe(true);

  // The command endpoint still resolves the device id and handles the POST.
  const buttons = (await (await page.request.get('/api/equipment/buttons')).json()).buttons;
  const aquaPure = buttons.find((b: any) => /aquapure/i.test(b.label || ''));
  expect(aquaPure, 'fixture should materialise the AquaPure chlorinator').toBeTruthy();
  expect(aquaPure.controllable, 'chlorinator is configurable, not a toggle').toBe(false);

  const resp = await page.request.post(`/api/equipment/buttons/${aquaPure.id}`);
  expect(resp.url()).toMatch(BUTTONS_COMMAND_RE);
  expect([200, 503]).toContain(resp.status());
});
