import { test, expect } from '@playwright/test';

/**
 * E2E — Schedules / scheduler (WS4).
 *
 * Selected by AQUALINK_SCHEDULES_FILE in playwright.config.ts:
 *   - default: the Schedules view shows a graceful "disabled" message.
 *   - scheduler enabled: schedule CRUD works (the engine firing a command may
 *     503 in replay mode — that is accepted; we assert the CRUD itself).
 */

const SCHEDULER_ENABLED = !!process.env.AQUALINK_SCHEDULES_FILE;

test('Schedules shows a disabled message when the scheduler is off', async ({ page }) => {
  test.skip(SCHEDULER_ENABLED, 'scheduler is enabled in this run');

  await page.goto('/');
  await page.locator('.nav-link', { hasText: 'Schedules' }).click();
  await expect(page.getByText(/App scheduler disabled/i)).toBeVisible();
});

test('Schedule CRUD works when the scheduler is on', async ({ page, request }) => {
  test.skip(!SCHEDULER_ENABLED, 'scheduler is disabled in this run');

  // Reject an invalid schedule (bad time).
  const bad = await request.post('/api/schedules', {
    data: { name: 'bad', days_of_week: 1, time_local: '99:99', action: { type: 'button_toggle', target: 'Pool Pump' } },
  });
  expect(bad.status()).toBe(400);

  // Create a valid schedule; the server assigns the uuid.
  const createResp = await request.post('/api/schedules', {
    data: {
      name: 'E2E schedule',
      enabled: true,
      days_of_week: 127,
      time_local: '06:30',
      action: { type: 'button_toggle', target: 'Pool Pump' },
    },
  });
  expect(createResp.status()).toBe(201);
  const created = await createResp.json();
  expect(created.uuid).toBeTruthy();

  // It appears in the list...
  const listResp = await request.get('/api/schedules');
  expect(listResp.ok()).toBeTruthy();
  const list = await listResp.json();
  expect(list.some((s: { uuid: string }) => s.uuid === created.uuid)).toBeTruthy();

  // ...and the Schedules view renders the row.
  await page.goto('/');
  await page.locator('.nav-link', { hasText: 'Schedules' }).click();
  await expect(page.locator('.sched-name', { hasText: 'E2E schedule' })).toBeVisible({ timeout: 10_000 });

  // Delete it.
  const delResp = await request.delete(`/api/schedules/${created.uuid}`);
  expect(delResp.status()).toBe(204);
});

test('Controller-schedule write route is wired with the command dispatcher', async ({ request }) => {
  // Regression (app wiring, aqualink-automate.cpp): RunApplication must hand the
  // /api/controller/schedules write handler the app's live ICommandDispatcher. A wiring
  // regression that leaves the route with a null dispatcher makes RequireCommandDispatcher
  // answer 503 with the plain-text sentinel "Command dispatcher not available" for EVERY
  // write, before any body/feasibility check. With the dispatcher wired the request instead
  // reaches the real handler (a JSON queued/rejected result, or a 400 not-representable), so
  // the sentinel is an exact discriminator for the wiring — independent of whether an emulated
  // controller is present (this run boots --jandy-disable-emulation, so no device accepts the
  // write; that is fine, we assert only that the dispatcher gate was passed).
  //
  // Not gated on the app scheduler: the controller store and dispatcher are always registered.

  // The read store is always registered, so the collection route is reachable.
  const getResp = await request.get('/api/controller/schedules');
  expect(getResp.status()).toBe(200);

  // A write must get PAST the null-dispatcher gate into the real handler.
  const postResp = await request.post('/api/controller/schedules', {
    data: { target: 'Pool Pump', days_of_week: 127, on_local: '08:00', off_local: '10:00' },
  });
  expect(await postResp.text()).not.toContain('Command dispatcher not available');
});
