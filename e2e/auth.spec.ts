import { test, expect, Page } from '@playwright/test';

/**
 * E2E — UI authentication (Wave A: identity system, username/password sessions).
 *
 * Runs ONLY when playwright.config.ts is invoked with AQUALINK_AUTH_MODE=enabled,
 * which boots the real binary with:
 *
 *   --auth-mode enabled --auth-state-dir <fresh temp dir>
 *
 * The fresh state dir means the user store starts EMPTY, so `/api/auth/me`
 * reports `setup_required: true` and the SPA shows the first-run setup wizard.
 * These tests run SERIALLY against ONE server instance (workers: 1,
 * fullyParallel: false), so the admin created by the first test persists on disk
 * for the login/session tests that follow. Test order below is therefore
 * significant and enforced with test.describe.serial.
 *
 * What this covers (the Wave A acceptance list):
 *   1. first-run setup wizard -> logged in;
 *   2. logout -> login card;
 *   3. wrong password shows the one indistinguishable error;
 *   4. password change (min-12, confirm);
 *   5. session list + revoke.
 *
 * The credentials are created by test 1 and reused by the later tests.
 */

const ADMIN_USER = 'e2eadmin';
const ADMIN_PASS = 'correct-horse-battery-staple'; // >= 12 chars
const NEW_PASS = 'even-longer-passphrase-2026';     // >= 12 chars

// The chemistry section proves the authenticated /ws/equipment socket delivered
// live replay data — i.e. the WS bearer-subprotocol path works end to end.
async function expectLiveDashboard(page: Page) {
  await expect(page.locator('.login-overlay')).toBeHidden();
  await expect(page.locator('.section-title', { hasText: 'Water Chemistry' })).toBeVisible();
  const saltCard = page
    .locator('.gauge-card')
    .filter({ has: page.locator('.gauge-label', { hasText: 'Salt' }) });
  await expect(saltCard.locator('.chem-dial-num')).toHaveText(/3200/, { timeout: 15_000 });
}

async function loginViaCard(page: Page, user: string, pass: string, remember = false) {
  const overlay = page.locator('.login-overlay');
  await overlay.locator('input[aria-label="Username"]').fill(user);
  await overlay.locator('input[aria-label="Password"]').fill(pass);
  if (remember) {
    await overlay.locator('.login-remember input').check();
  }
  await overlay.locator('.login-submit').click();
}

test.describe.serial('UI authentication (identity system)', () => {
  test('first-run setup wizard creates the admin and lands on the live dashboard', async ({ page }) => {
    await page.goto('/');

    // Empty user store -> the setup wizard is shown (not the login card): it asks
    // for a username plus password + confirm.
    const overlay = page.locator('.login-overlay');
    await expect(overlay).toBeVisible();
    await expect(overlay.locator('input[aria-label="Admin username"]')).toBeVisible();

    const pw = overlay.locator('input[aria-label="Password"]');
    const confirm = overlay.locator('input[aria-label="Confirm password"]');

    // Too-short password keeps the submit disabled and shows the min-length hint.
    await overlay.locator('input[aria-label="Admin username"]').fill(ADMIN_USER);
    await pw.fill('short');
    await confirm.fill('short');
    await expect(overlay.locator('.login-submit')).toBeDisabled();

    // A valid, matching, >=12-char password enables submit.
    await pw.fill(ADMIN_PASS);
    await confirm.fill(ADMIN_PASS);
    await expect(overlay.locator('.login-submit')).toBeEnabled();
    await overlay.locator('.login-submit').click();

    // Setup auto-logs-in -> overlay hides, the live dashboard renders.
    await expectLiveDashboard(page);
  });

  test('wrong password shows the indistinguishable error; correct password signs in', async ({ page }) => {
    await page.goto('/');
    const overlay = page.locator('.login-overlay');

    // Accounts now exist -> the login card (not the setup wizard) is shown.
    await expect(overlay).toBeVisible();
    await expect(overlay.locator('input[aria-label="Username"]')).toBeVisible();

    // Wrong password -> the single generic error, still on the login card.
    await loginViaCard(page, ADMIN_USER, 'this-is-the-wrong-password');
    await expect(overlay.locator('.login-error')).toBeVisible();
    await expect(overlay).toBeVisible();

    // Correct password (remember me) -> live dashboard.
    await loginViaCard(page, ADMIN_USER, ADMIN_PASS, true);
    await expectLiveDashboard(page);

    // "Remember me" persisted the refresh token: a reload stays signed in.
    await page.reload();
    await expectLiveDashboard(page);
  });

  test('logout returns to the login card; logging back in works', async ({ page }) => {
    await page.goto('/');
    await loginViaCard(page, ADMIN_USER, ADMIN_PASS, true);
    await expectLiveDashboard(page);

    // Open the account menu and sign out.
    await page.locator('button[title="Account"]').click();
    await page.locator('.account-card').getByRole('button', { name: 'Sign out', exact: true }).click();

    // Back to the login card.
    await expect(page.locator('.login-overlay')).toBeVisible();
    await expect(page.locator('.login-overlay input[aria-label="Username"]')).toBeVisible();

    // Log back in.
    await loginViaCard(page, ADMIN_USER, ADMIN_PASS);
    await expectLiveDashboard(page);
  });

  test('account menu lists this session and can revoke another', async ({ page }) => {
    // Open a first session (remember me so the refresh token survives).
    await page.goto('/');
    await loginViaCard(page, ADMIN_USER, ADMIN_PASS, true);
    await expectLiveDashboard(page);

    // Open a SECOND, independent browser session for the same user so there are
    // at least two sessions to see and revoke.
    const second = await page.context().browser()!.newContext();
    const secondPage = await second.newPage();
    await secondPage.goto('/');
    await loginViaCard(secondPage, ADMIN_USER, ADMIN_PASS);
    await expectLiveDashboard(secondPage);

    // Back on the first page, open the account menu and read the session list.
    await page.locator('button[title="Account"]').click();
    const card = page.locator('.account-card');
    await expect(card).toBeVisible();
    const sessions = card.locator('.account-session');
    await expect(sessions.first()).toBeVisible();
    const before = await sessions.count();
    expect(before).toBeGreaterThanOrEqual(2);

    // Revoke one; the row count drops.
    await sessions.first().locator('.account-revoke').click();
    await expect(sessions).toHaveCount(before - 1);

    await second.close();
  });

  test('password change requires min-12 + confirm and reflects the session cull', async ({ page }) => {
    await page.goto('/');
    await loginViaCard(page, ADMIN_USER, ADMIN_PASS, true);
    await expectLiveDashboard(page);

    await page.locator('button[title="Account"]').click();
    const card = page.locator('.account-card');
    const newPw = card.locator('input[aria-label="New password"]');
    const confirmPw = card.locator('input[aria-label="Confirm new password"]');

    // Too short -> client-side error, no request.
    await newPw.fill('short');
    await confirmPw.fill('short');
    await card.getByRole('button', { name: /Update password/ }).click();
    await expect(card.locator('.login-error')).toBeVisible();

    // Mismatch -> error.
    await newPw.fill(NEW_PASS);
    await confirmPw.fill(NEW_PASS + 'x');
    await card.getByRole('button', { name: /Update password/ }).click();
    await expect(card.locator('.login-error')).toBeVisible();

    // Valid + matching -> success; the "other sessions signed out" note appears.
    await newPw.fill(NEW_PASS);
    await confirmPw.fill(NEW_PASS);
    await card.getByRole('button', { name: /Update password/ }).click();
    await expect(card.locator('.account-ok')).toBeVisible();

    // The new password now works for a fresh login; restore the original so the
    // serial suite's later reruns (if any) remain deterministic is unnecessary —
    // this is the last test — but prove the change took effect.
    await card.getByRole('button', { name: 'Sign out', exact: true }).click();
    await expect(page.locator('.login-overlay')).toBeVisible();
    await loginViaCard(page, ADMIN_USER, NEW_PASS);
    await expectLiveDashboard(page);
  });
});
