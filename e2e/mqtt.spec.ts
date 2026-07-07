import { test, expect } from '@playwright/test';

/**
 * MQTT integration e2e — runs ONLY under AQUALINK_MQTT=enabled (see
 * playwright.config.ts). In that mode Playwright starts a loopback MQTT broker
 * (e2e/support/mqtt-broker.mjs) and boots the app with --mqtt + --home-assistant
 * pointed at it. This is the only e2e launch mode that actually exercises the
 * MQTT layer — MqttClient connect/CONNACK/recv-loop/flush, MqttIntegration state
 * publishing, MqttHub, and Home-Assistant auto-discovery — so it drives that
 * code under the gcov-instrumented coverage build.
 *
 * The assertions double as a guard: a silent broker / protocol / auth failure
 * surfaces as a red spec instead of merely producing no coverage.
 */

test('MQTT client connects to the broker and publishes with HA discovery', async ({ request }) => {
  const read = async () => (await request.get('/api/diagnostics/mqtt')).json();

  // Poll up to ~12s for the client to connect and complete its initial burst.
  let diag = await read();
  for (let i = 0; i < 24 && !(diag.connected && diag.published > 0); i++) {
    await new Promise((r) => setTimeout(r, 500));
    diag = await read();
  }

  expect(diag.enabled).toBe(true);
  expect(diag.connected).toBe(true);
  expect(diag.state).toBe('Connected');
  expect(diag.home_assistant_enabled).toBe(true);
  expect(diag.published).toBeGreaterThan(0);
  expect(diag.dropped).toBe(0);

  // With 1s status/stats intervals the publish/flush path keeps running, so the
  // published counter must climb over a few seconds.
  const before = diag.published as number;
  await new Promise((r) => setTimeout(r, 3000));
  const after = (await read()).published as number;
  expect(after).toBeGreaterThan(before);
});

test('The Diagnostics MQTT card reflects the live broker connection', async ({ page }) => {
  await page.goto('/');
  await page.locator('.nav-link', { hasText: 'Diagnostics' }).click();

  const card = page.locator('.mqtt-card');
  await expect(card).toBeVisible({ timeout: 10_000 });
  // In the default (broker-less) run this reads "Disabled"; with a live broker it
  // must reflect the connected state instead.
  await expect(card.locator('.mqtt-status')).not.toHaveText(/Disabled/i, { timeout: 10_000 });
});
