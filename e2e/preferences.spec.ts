import { test, expect } from '@playwright/test';

/**
 * E2E — user/admin preferences (server-backed Settings).
 *
 * The preferences service is always present (it works in-memory even without
 * --preferences-file), so these run in the default mode.
 */

test('preferences API round-trips and validates', async ({ request }) => {
  const get1 = await request.get('/api/preferences');
  expect(get1.ok()).toBeTruthy();
  const before = await get1.json();
  expect(before).toHaveProperty('alert');
  expect(before).toHaveProperty('history');

  // A valid partial update applies...
  const put = await request.put('/api/preferences', { data: { alert: { salt_low_ppm: 2900 } } });
  expect(put.ok()).toBeTruthy();

  const after = await (await request.get('/api/preferences')).json();
  expect(after.alert.salt_low_ppm).toBe(2900);

  // ...and an out-of-range value is rejected (whole request).
  const bad = await request.put('/api/preferences', { data: { alert: { salt_low_ppm: 99999 } } });
  expect(bad.status()).toBe(400);

  // The rejected value did not change anything.
  const unchanged = await (await request.get('/api/preferences')).json();
  expect(unchanged.alert.salt_low_ppm).toBe(2900);
});

test('label override produces a display_label on the buttons endpoint', async ({ request }) => {
  const buttons = (await (await request.get('/api/equipment/buttons')).json()).buttons;
  test.skip(!Array.isArray(buttons) || buttons.length === 0, 'no devices discovered in this fixture');

  const target = buttons.find((b: any) => b.label);
  test.skip(!target, 'no labelled device');
  const canonical = target.label;

  const put = await request.put('/api/preferences', { data: { label_overrides: { [canonical]: 'My Renamed Device' } } });
  expect(put.ok()).toBeTruthy();

  const after = (await (await request.get('/api/equipment/buttons')).json()).buttons;
  const renamed = after.find((b: any) => b.label === canonical);
  expect(renamed.display_label).toBe('My Renamed Device');
  expect(renamed.label).toBe(canonical); // canonical label unchanged

  // Clearing the override -> display_label falls back to the canonical label.
  await request.put('/api/preferences', { data: { label_overrides: {} } });
  const cleared = (await (await request.get('/api/equipment/buttons')).json()).buttons;
  expect(cleared.find((b: any) => b.label === canonical).display_label).toBe(canonical);
});

test('Settings view shows and saves server preferences', async ({ page }) => {
  await page.goto('/');
  await page.locator('.nav-link', { hasText: 'Settings' }).click();

  await expect(page.getByText('System Preferences')).toBeVisible();

  // Temperature units live in the Appearance card and apply instantly on
  // change (no Save button) — wait for the PUT that the change fires.
  const appearanceCard = page.locator('.settings-card', { hasText: 'Appearance' });
  const unitsPut = page.waitForResponse(
    (r) => r.url().includes('/api/preferences') && r.request().method() === 'PUT',
  );
  await appearanceCard.locator('#pref-units').selectOption('Fahrenheit');
  expect((await unitsPut).ok()).toBeTruthy();

  // The change is reflected by the API.
  const prefs = await (await page.request.get('/api/preferences')).json();
  expect(prefs.temperature_units).toBe('Fahrenheit');

  // The System Preferences card batch-saves the alert/history fields via its
  // Save button and confirms with a "Saved" flash.
  const sysCard = page.locator('.settings-card', { hasText: 'System Preferences' });
  await sysCard.locator('#pref-retention').fill('120');
  await sysCard.getByRole('button', { name: 'Save' }).click();
  await expect(sysCard.getByText('Saved ✓')).toBeVisible({ timeout: 5_000 });

  const after = await (await page.request.get('/api/preferences')).json();
  expect(after.history.retention_days).toBe(120);
});
