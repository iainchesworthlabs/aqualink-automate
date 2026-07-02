import { test, expect } from '@playwright/test';

/**
 * Equipment-control icons: each controllable device renders a line-art SVG icon
 * from the sprite (id="icon-<key>"), chosen by device type / label — not emoji.
 *
 * The default replay fixture only materialises the (non-controllable) chlorinator,
 * so we inject mock controllable devices into the pool store to exercise the grid.
 */
const MOCK = [
  { id: '1', label: 'Pool Light', display_label: 'Pool Light', status: 'On', device_type: 'Light', controllable: true },
  { id: '2', label: 'Filter Pump', display_label: 'Filter Pump', status: 'Running', device_type: 'Pump', controllable: true },
  { id: '3', label: 'Waterfall', display_label: 'Waterfall', status: 'Off', device_type: 'Spillover', controllable: true },
  { id: '4', label: 'Cleaner', display_label: 'Cleaner', status: 'Off', device_type: 'Cleaner', controllable: true },
  { id: '5', label: 'Jets', display_label: 'Jets', status: 'Off', device_type: 'Auxillary', controllable: true },
  { id: '6', label: 'Sprinklers', display_label: 'Sprinklers', status: 'Off', device_type: 'Sprinkler', controllable: true },
];

test('equipment controls render line-art SVG icons (not emoji)', async ({ page }) => {
  // Serve the mock from the buttons endpoint itself. Writing it into the
  // Alpine store after goto raced the app's own in-flight buttons fetch,
  // which could land later and overwrite the mock with fixture data
  // (0 controllable devices) — a long-standing flake.
  await page.route('**/api/equipment/buttons', (route) => route.fulfill({ json: { buttons: MOCK } }));
  await page.goto('/');

  const controls = page.locator('.eq-control');
  await expect(controls).toHaveCount(MOCK.length);

  // Each control renders an <svg> icon (svg.eq-control-icon > use) referencing a
  // sprite symbol — proving the icons are line-art SVG, not emoji text.
  for (let i = 0; i < MOCK.length; i++) {
    await expect(controls.nth(i).locator('svg.eq-control-icon use')).toHaveAttribute('href', /^#icon-/);
  }
});
