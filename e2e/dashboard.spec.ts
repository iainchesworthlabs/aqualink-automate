import { test, expect } from '@playwright/test';

/**
 * Dashboard UX redesign:
 *   - chemistry consolidated into one section (no duplicate chlorinator card),
 *   - equipment controls limited to controllable devices, rendered as switches,
 *   - a `controllable` flag on the buttons API drives the split.
 */

function isHeater(b: any): boolean {
  return b.device_type === 'Heater' || (b.label && String(b.label).toLowerCase().includes('heat'));
}

test('buttons API exposes a controllable flag; chlorinator/unknown are not controllable', async ({ request }) => {
  const buttons = (await (await request.get('/api/equipment/buttons')).json()).buttons;
  test.skip(!Array.isArray(buttons) || buttons.length === 0, 'no devices discovered in this fixture');

  for (const b of buttons) {
    expect(typeof b.controllable).toBe('boolean');
  }
  for (const b of buttons.filter((x: any) => x.device_type === 'Chlorinator' || x.device_type === 'Unknown')) {
    expect(b.controllable).toBe(false);
  }
});

test('dashboard has one consolidated chemistry section — no duplicate chlorinator card', async ({ page }) => {
  await page.goto('/');
  // The dashboard section title and the Detailed view's card header both read
  // "Water Chemistry" (both always in the DOM via x-show), so target the
  // dashboard section title specifically.
  await expect(page.locator('.section-title', { hasText: 'Water Chemistry' })).toBeVisible();
  await expect(page.getByText('Chemistry / Chlorinator')).toHaveCount(0);
});

test('chlorinator output control: API validates, and the slider renders when present', async ({ page, request }) => {
  // A valid percentage is handled (200 toggled, or 503 when no commandable
  // chlorinator/iAQ device is present in this minimal fixture).
  const ok = await request.post('/api/equipment/chlorinator', { data: { percentage: 60 } });
  expect([200, 503]).toContain(ok.status());

  // Out-of-range is rejected BEFORE dispatch.
  expect((await request.post('/api/equipment/chlorinator', { data: { percentage: 150 } })).status()).toBe(400);
  expect((await request.post('/api/equipment/chlorinator', { data: { boost: 'yes' } })).status()).toBe(400);

  // Boost is accepted/handled.
  expect([200, 503]).toContain((await request.post('/api/equipment/chlorinator', { data: { boost: true } })).status());

  // The target control is integrated into the SWG Output tile (the fixture
  // materialises a chlorinator). The tile is x-show="present"/x-cloak until the
  // replayed AquaRite feed flips chlorinatorPresent, so allow it time to appear.
  await page.goto('/');
  // Scope to the SWG tile: the Detailed view also has a "SWG output" kv row.
  await expect(page.locator('.swg-card').getByText('SWG Output')).toBeVisible({ timeout: 15_000 });
  await expect(page.locator('.swg-card .swg-slider')).toBeVisible({ timeout: 15_000 });
  await expect(page.locator('.swg-card').getByRole('button', { name: 'Boost' })).toBeVisible({ timeout: 15_000 });

  // A 0% reading must explain itself. generating_percent is the INSTANTANEOUS output and
  // reads 0 whenever the cell is idle, which alone cannot say whether the chlorinator is
  // switched off or simply waiting for the filter pump; the card captions it with the
  // server-derived reason. The caption is deliberately hidden while the cell IS generating,
  // so key the assertion off what the API reports rather than the fixture's mood.
  const swg = (await (await request.get('/api/equipment')).json()).chemistry?.chlorinator;
  if (swg) {
    expect(swg).toHaveProperty('generating_reason');
    const reasonCaption = page.locator('.swg-card .swg-reason');
    if (swg.generating_reason === 'Generating') {
      await expect(reasonCaption).toBeHidden();
    } else {
      await expect(reasonCaption).toBeVisible({ timeout: 15_000 });
      await expect(reasonCaption).not.toBeEmpty();
    }
  }
});

test('equipment controls render as switches for controllable devices only', async ({ page, request }) => {
  const buttons = (await (await request.get('/api/equipment/buttons')).json()).buttons;
  const controllable = buttons.filter((b: any) => b.controllable && !isHeater(b));
  test.skip(controllable.length === 0, 'no controllable non-heater devices in this fixture');

  await page.goto('/');
  await expect(page.locator('.eq-control').first()).toBeVisible();
  await expect(page.locator('.eq-switch').first()).toBeVisible();
  // The legacy icon-tile control is gone.
  await expect(page.locator('.eq-button')).toHaveCount(0);
});
