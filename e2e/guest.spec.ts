import { test, expect, Page, BrowserContext } from '@playwright/test';

/**
 * E2E — Guest mode + kiosk PIN elevation (Slice 5).
 *
 * Runs under the identity-enabled server (AQUALINK_AUTH_MODE=enabled) against a
 * FRESH temp --auth-state-dir, so the store starts EMPTY. Like auth.spec, the
 * first test creates the administrator via the first-run wizard; the store then
 * persists for the serial tests that follow. Run standalone:
 *
 *   AQUALINK_AUTH_MODE=enabled npx playwright test e2e/guest.spec.ts
 *
 * The replay fixture is an AquaRite SWG whose one auxiliary is the chlorinator,
 * so the reliably-present lockable control here is the SWG (chlorinator) card —
 * gated on `equipment.control.chlorinator`. The tests therefore assert guest
 * gating through that control (the per-aux path is covered by unit tests and a
 * skip-if-absent case below).
 *
 * What this covers:
 *   1. with the Guest scope granted equipment.view, an ANONYMOUS visitor (no
 *      session) reaches the live dashboard as a guest — no login wall — with a
 *      "Guest" pill + Sign-in affordance, and the SWG control locked;
 *   2. granting the Guest scope chlorinator control unlocks that control;
 *   3. signing in from the guest dashboard elevates to full control;
 *   4. a configured kiosk PIN elevates an anonymous visitor into the target
 *      group, and the admin Kiosk tab reflects the config.
 */

const ADMIN_USER = 'guestadmin';
const ADMIN_PASS = 'correct-horse-battery-staple'; // >= 12 chars
const KIOSK_PIN = '2468';

async function expectLiveDashboard(page: Page) {
  await expect(page.locator('.login-overlay')).toBeHidden();
  await expect(page.locator('.section-title', { hasText: 'Water Chemistry' })).toBeVisible();
  const saltCard = page
    .locator('.gauge-card')
    .filter({ has: page.locator('.gauge-label', { hasText: 'Salt' }) });
  // i18n number formatter groups thousands per locale ("3,200" in en) — tolerate
  // an optional grouping separator (comma / dot / whitespace incl. nbsp via \s).
  await expect(saltCard.locator('.chem-dial-num')).toHaveText(/3[\s,.]?200/, { timeout: 15_000 });
}

// The SWG (chlorinator) control card is present in this fixture and gated on
// equipment.control.chlorinator: locked shows the "Sign in to adjust" note and
// hides the Boost button; unlocked shows Boost.
function swgCard(page: Page) {
  return page.locator('.swg-card');
}
async function expectSwgLocked(page: Page) {
  await expect(swgCard(page)).toBeVisible();
  await expect(swgCard(page).locator('.swg-locked-note')).toBeVisible();
  await expect(swgCard(page).getByRole('button', { name: 'Boost' })).toHaveCount(0);
}
async function expectSwgUnlocked(page: Page) {
  await expect(swgCard(page)).toBeVisible();
  await expect(swgCard(page).getByRole('button', { name: 'Boost' })).toBeVisible();
}

// Land authenticated as the administrator. The store persists across this
// serial suite, so the FIRST test hits the first-run wizard (empty store) and
// later tests hit the login card — handle both.
async function setupAdmin(page: Page) {
  await page.goto('/');
  const overlay = page.locator('.login-overlay');
  const signIn = page.locator('button[title="Sign in"]');

  // Once an admin exists AND the Guest scope grants viewing, a fresh (tokenless)
  // page browses as a guest — the login overlay is hidden. Open it via the
  // Sign-in affordance in that case; the first-run/empty-store case shows the
  // overlay (setup wizard) immediately.
  await Promise.race([
    overlay.waitFor({ state: 'visible' }).catch(() => {}),
    signIn.waitFor({ state: 'visible' }).catch(() => {}),
  ]);
  if (!(await overlay.isVisible().catch(() => false))) {
    await signIn.click();
    await expect(overlay).toBeVisible();
  }

  const setup = overlay.locator('input[aria-label="Admin username"]');
  if (await setup.isVisible().catch(() => false)) {
    await setup.fill(ADMIN_USER);
    await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
    await overlay.locator('input[aria-label="Confirm password"]').fill(ADMIN_PASS);
    await overlay.locator('.login-submit').click();
  } else {
    await overlay.locator('input[aria-label="Username"]').fill(ADMIN_USER);
    await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
    await overlay.locator('.login-submit').click();
  }
  await expectLiveDashboard(page);
}

// Call an admin API from the page context so the fetch wrapper attaches the
// admin's bearer token. Returns { status, json }.
async function adminApi(page: Page, method: string, url: string, body?: unknown) {
  return page.evaluate(
    async ([m, u, b]) => {
      const init: RequestInit = { method: m as string };
      if (b !== null) {
        init.headers = { 'Content-Type': 'application/json' };
        init.body = JSON.stringify(b);
      }
      const resp = await fetch(u as string, init);
      let json: unknown = null;
      try { json = await resp.json(); } catch { /* empty body */ }
      return { status: resp.status, json };
    },
    [method, url, body ?? null] as const,
  );
}

// Set the Guest group's entitlements (deny-by-default scope editor).
async function setGuestScope(page: Page, entitlements: string[]) {
  const res = await adminApi(page, 'POST', '/api/groups', { name: 'Guest', entitlements });
  expect(res.status, `set Guest scope -> ${res.status}`).toBeLessThan(300);
}

// A fresh, tokenless browser context = an anonymous visitor.
async function anonymousPage(context: BrowserContext): Promise<Page> {
  const fresh = await context.browser()!.newContext();
  return fresh.newPage();
}

test.describe.serial('Guest mode + kiosk PIN', () => {
  test('anonymous visitor browses as guest with a locked control when the Guest scope allows viewing', async ({ page }) => {
    await setupAdmin(page);
    await setGuestScope(page, ['equipment.view']); // read-only guest

    const guest = await anonymousPage(page.context());
    await guest.goto('/');

    // Guest mode: the dashboard renders (no login wall) with the guest pill and
    // Sign-in affordance both present.
    await expectLiveDashboard(guest);
    await expect(guest.locator('.guest-pill')).toBeVisible();
    await expect(guest.locator('button[title="Sign in"]')).toBeVisible();

    // The SWG control is locked for a view-only guest.
    await expectSwgLocked(guest);

    await guest.context().close();
  });

  test('granting the Guest scope chlorinator control unlocks that control', async ({ page }) => {
    await setupAdmin(page);
    await setGuestScope(page, ['equipment.view', 'equipment.control.chlorinator']);

    const guest = await anonymousPage(page.context());
    await guest.goto('/');
    await expectLiveDashboard(guest);

    // The granted control is now operable for the anonymous guest.
    await expectSwgUnlocked(guest);

    await guest.context().close();
  });

  test('a per-aux Guest grant unlocks exactly that aux control (skips if the fixture has none)', async ({ page }) => {
    await setupAdmin(page);
    const res = await adminApi(page, 'GET', '/api/equipment/buttons');
    const data: any = res.json;
    const list = Array.isArray(data) ? data : (data && Array.isArray(data.buttons) ? data.buttons : []);
    const aux = list.find((b: any) => b.controllable);
    test.skip(!aux, 'no controllable aux device in the replay fixture');

    await setGuestScope(page, ['equipment.view', `equipment.control.aux:${aux.id}`]);
    const guest = await anonymousPage(page.context());
    await guest.goto('/');
    await expectLiveDashboard(guest);

    const total = await guest.locator('.eq-control').count();
    const locked = await guest.locator('.eq-control.is-locked').count();
    expect(locked, 'the per-aux grant unlocks at least one control').toBeLessThan(total);

    await guest.context().close();
  });

  test('signing in from the guest dashboard elevates to full control', async ({ page }) => {
    await setupAdmin(page);
    await setGuestScope(page, ['equipment.view']);

    const guest = await anonymousPage(page.context());
    await guest.goto('/');
    await expectLiveDashboard(guest);
    await expectSwgLocked(guest);

    // Open the login overlay from the guest dashboard and sign in as the admin.
    await guest.locator('button[title="Sign in"]').click();
    const overlay = guest.locator('.login-overlay');
    await expect(overlay).toBeVisible();
    await overlay.locator('input[aria-label="Username"]').fill(ADMIN_USER);
    await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
    await overlay.locator('.login-submit').click();

    // Elevated: the guest pill is gone and the control is operable.
    await expectLiveDashboard(guest);
    await expect(guest.locator('.guest-pill')).toBeHidden();
    await expectSwgUnlocked(guest);

    await guest.context().close();
  });

  test('a kiosk PIN elevates an anonymous visitor into the target group', async ({ page }) => {
    await setupAdmin(page);
    await setGuestScope(page, ['equipment.view']);

    // A "Household" group with chlorinator control, then a kiosk PIN targeting it.
    const grp = await adminApi(page, 'POST', '/api/groups', { name: 'Household', entitlements: ['equipment.view', 'equipment.control.chlorinator'] });
    expect(grp.status).toBeLessThan(300);
    const kiosk = await adminApi(page, 'PUT', '/api/kiosk', { pin: KIOSK_PIN, target_group: 'Household' });
    expect(kiosk.status, `configure kiosk -> ${kiosk.status}`).toBeLessThan(300);

    // The admin Kiosk tab reflects the configuration.
    await page.locator('button[title="Administration"]').click();
    await expect(page.locator('.admin-card')).toBeVisible();
    await page.locator('.admin-tab', { hasText: 'Kiosk' }).click();
    await expect(page.locator('.admin-kiosk-status .admin-badge', { hasText: 'Enabled' })).toBeVisible();

    // A fresh anonymous visitor sees the locked control and a PIN entry.
    const guest = await anonymousPage(page.context());
    await guest.goto('/');
    await expectLiveDashboard(guest);
    await expectSwgLocked(guest);

    await guest.locator('button[title="Sign in"]').click();
    const overlay = guest.locator('.login-overlay');
    await expect(overlay.locator('.login-pin')).toBeVisible();
    await overlay.locator('input[aria-label="Kiosk PIN"]').fill(KIOSK_PIN);
    await overlay.locator('.login-pin-submit').click();

    // Elevated into Household: the chlorinator control is now operable.
    await expectLiveDashboard(guest);
    await expectSwgUnlocked(guest);

    await guest.context().close();
  });
});
