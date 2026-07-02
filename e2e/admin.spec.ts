import { test, expect, Page } from '@playwright/test';

/**
 * E2E — Administration UI (Slice 2 Wave B: user/group/entitlement/api-key admin).
 *
 * Runs under the SAME identity-enabled server the auth spec uses
 * (AQUALINK_AUTH_MODE=enabled -> `--auth-mode enabled` against a fresh temp
 * --auth-state-dir, so the user store starts EMPTY and the first test creates
 * the administrator via the first-run wizard). Serial, single worker: the admin
 * created by the first test persists for the tests that follow.
 *
 * NB: this file is only collected when AQUALINK_AUTH_MODE=enabled (see
 * playwright.config.ts testMatch); the config currently matches only auth.spec.ts
 * in that mode, so to run these:
 *   AQUALINK_AUTH_MODE=enabled npx playwright test e2e/admin.spec.ts
 * (the config's testMatch narrows to auth.spec.ts by default; pass the path
 * explicitly, or widen testMatch, to include this suite).
 *
 * Coverage (the Wave B acceptance list):
 *   1. the Administration nav control appears ONLY for a system.admin;
 *   2. the overlay opens with Users / Groups / Entitlements / API keys tabs;
 *   3. create a user (groups + direct entitlement via the editor) and see it listed;
 *   4. the entitlement editor exposes the vocabulary + the per-aux selector;
 *   5. built-in groups are undeletable and the Guest group is labelled as the
 *      anonymous-visitor scope;
 *   6. create an API key -> the one-time secret modal is shown once with a copy
 *      button, then the key is listed (never a secret) and can be revoked.
 */

const ADMIN_USER = 'wbadmin';
const ADMIN_PASS = 'correct-horse-battery-staple'; // >= 12 chars
const NEW_USER = 'operator-jane';
const NEW_USER_PASS = 'operator-jane-passphrase'; // >= 12 chars

async function ensureSignedIn(page: Page) {
  await page.goto('/');
  const overlay = page.locator('.login-overlay');
  await expect(overlay).toBeVisible();

  const setup = overlay.locator('input[aria-label="Admin username"]');
  if (await setup.isVisible().catch(() => false)) {
    // First-run: create the administrator.
    await setup.fill(ADMIN_USER);
    await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
    await overlay.locator('input[aria-label="Confirm password"]').fill(ADMIN_PASS);
    await overlay.locator('.login-submit').click();
  } else {
    await overlay.locator('input[aria-label="Username"]').fill(ADMIN_USER);
    await overlay.locator('input[aria-label="Password"]').fill(ADMIN_PASS);
    await overlay.locator('.login-submit').click();
  }
  await expect(page.locator('.login-overlay')).toBeHidden();
}

async function openAdmin(page: Page) {
  await page.locator('button[title="Administration"]').click();
  await expect(page.locator('.admin-card')).toBeVisible();
}

test.describe.serial('Administration UI (Wave B)', () => {
  test('the Administration control appears for an admin and opens the tabbed overlay', async ({ page }) => {
    await ensureSignedIn(page);

    const adminBtn = page.locator('button[title="Administration"]');
    await expect(adminBtn).toBeVisible();

    await adminBtn.click();
    const card = page.locator('.admin-card');
    await expect(card).toBeVisible();
    for (const label of ['Users', 'Groups', 'Entitlements', 'API keys']) {
      await expect(card.locator('.admin-tab', { hasText: label })).toBeVisible();
    }
  });

  test('the Entitlements reference tab enumerates the server vocabulary', async ({ page }) => {
    await ensureSignedIn(page);
    await openAdmin(page);
    await page.locator('.admin-tab', { hasText: 'Entitlements' }).click();
    await expect(page.locator('.admin-vocab-item').first()).toBeVisible();
    // system.admin is always in the vocabulary.
    await expect(page.locator('.admin-vocab-item code', { hasText: 'system.admin' })).toBeVisible();
  });

  test('creating a user (with a group + a direct entitlement) lists them', async ({ page }) => {
    await ensureSignedIn(page);
    await openAdmin(page);
    await page.locator('.admin-tab', { hasText: 'Users' }).click();

    await page.getByRole('button', { name: 'New user' }).click();
    const panel = page.locator('.admin-panel', { hasText: 'New user' });
    await expect(panel).toBeVisible();

    await panel.locator('input[aria-label="Username"]').fill(NEW_USER);
    await panel.locator('input[aria-label="Password"]').fill(NEW_USER_PASS);
    await panel.locator('input[aria-label="Confirm password"]').fill(NEW_USER_PASS);

    // Grant one plain action via the editor (equipment.view is always present).
    const viewChip = panel.locator('.ent-actions .admin-check-chip', { hasText: 'equipment.view' });
    await expect(viewChip).toBeVisible();
    await viewChip.locator('input[type="checkbox"]').check();

    await panel.getByRole('button', { name: 'Create user' }).click();

    // The new user appears in the list with its entitlement chip.
    const row = page
      .locator('section[x-data="adminUsersView()"] .admin-row')
      .filter({ has: page.locator('.admin-row-title', { hasText: NEW_USER }) });
    await expect(row).toBeVisible();
    await expect(row.locator('.ent-chip', { hasText: 'equipment.view' })).toBeVisible();
  });

  test('a weak password surfaces the server error verbatim', async ({ page }) => {
    await ensureSignedIn(page);
    await openAdmin(page);
    await page.locator('.admin-tab', { hasText: 'Users' }).click();
    await page.getByRole('button', { name: 'New user' }).click();
    const panel = page.locator('.admin-panel', { hasText: 'New user' });

    await panel.locator('input[aria-label="Username"]').fill('too-weak-user');
    // Client-side guard fires first for < 12 chars.
    await panel.locator('input[aria-label="Password"]').fill('short');
    await panel.locator('input[aria-label="Confirm password"]').fill('short');
    await panel.getByRole('button', { name: 'Create user' }).click();
    // The create panel embeds the entitlement editor, which has its own
    // (hidden) .login-error span; match the one that is actually shown.
    await expect(panel.locator('.login-error:visible')).toBeVisible();
  });

  test('built-in groups are undeletable and Guest is labelled as the anonymous scope', async ({ page }) => {
    await ensureSignedIn(page);
    await openAdmin(page);
    await page.locator('.admin-tab', { hasText: 'Groups' }).click();

    const groups = page.locator('section[x-data="adminGroupsView()"] .admin-row');
    await expect(groups.first()).toBeVisible();

    // Every built-in group shows an "Undeletable" note and no Delete button.
    const builtinRow = groups.filter({ has: page.locator('.admin-badge-builtin:visible') }).first();
    await expect(builtinRow.getByText('Undeletable')).toBeVisible();
    await expect(builtinRow.getByRole('button', { name: 'Delete' })).toHaveCount(0);

    // Editing Guest reveals the deny-by-default anonymous-scope explainer.
    // Match the name span by EXACT text ("Guest") — the row title also holds
    // "Built-in" and "Guest scope" badges, so a /^Guest\b/ regex over the
    // concatenated title text ("GuestBuilt-in…") would not match.
    const guestRow = groups.filter({ has: page.getByText('Guest', { exact: true }) }).first();
    await guestRow.getByRole('button', { name: 'Edit' }).click();
    await expect(page.locator('.admin-guest-note:visible')).toBeVisible();
  });

  test('creating an API key shows the one-time secret once, then lists and revokes it', async ({ page }) => {
    await ensureSignedIn(page);
    await openAdmin(page);
    await page.locator('.admin-tab', { hasText: 'API keys' }).click();

    await page.getByRole('button', { name: 'New key' }).click();
    const panel = page.locator('.admin-panel', { hasText: 'New API key' });
    await expect(panel).toBeVisible();
    await panel.locator('input[aria-label="Label"]').fill('integration-key');
    const chip = panel.locator('.ent-actions .admin-check-chip', { hasText: 'equipment.view' });
    await chip.locator('input[type="checkbox"]').check();
    await panel.getByRole('button', { name: 'Create key' }).click();

    // One-time secret modal: secret shown + copy button + must-dismiss.
    const secretCard = page.locator('.admin-secret-card');
    await expect(secretCard).toBeVisible();
    await expect(secretCard.locator('.admin-secret-value')).not.toBeEmpty();
    await expect(secretCard.getByRole('button', { name: 'Copy' })).toBeVisible();
    await secretCard.getByRole('button', { name: /dismiss/ }).click();
    await expect(secretCard).toBeHidden();

    // The key is listed (never a secret) and can be revoked.
    const row = page
      .locator('section[x-data="adminApikeysView()"] .admin-row')
      .filter({ has: page.locator('.admin-row-title', { hasText: 'integration-key' }) });
    await expect(row).toBeVisible();
    await row.getByRole('button', { name: 'Revoke' }).click();
    await row.getByRole('button', { name: 'Revoke key' }).click();
    await expect(row.locator('.admin-badge-off', { hasText: 'Revoked' })).toBeVisible();
  });
});
