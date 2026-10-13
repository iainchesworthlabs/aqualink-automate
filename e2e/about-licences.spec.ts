import { test, expect } from '@playwright/test';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

/**
 * About page — licence line, copyright line and the third-party "Licences"
 * section.
 *
 * Everything shown comes from one data file, assets/web/scripts/config/
 * third-party-notices.js (window.AquaNotices), so these tests read the same
 * object the page renders rather than restating it. What they pin down:
 *
 *   - the licence line names the licence and LICENSE.txt and links to the source;
 *   - the copyright line is the data file's;
 *   - every listed component is rendered with its licence text one click away;
 *   - nothing vendored ships without a notice: every file under
 *     assets/web/vendor (and every font family declared in fonts.css) must be
 *     claimed by an entry's `files`, so vendoring a library or font without
 *     adding it to the data file fails here instead of shipping unattributed.
 */

const WEB = join(__dirname, '..', 'assets', 'web');

interface Component {
  kind: 'font' | 'script' | 'icon';
  name: string;
  version?: string;
  licence: string;
  copyright: string[];
  notes?: string[];
  url: string;
  files: string[];
}
interface Notices {
  app: { licence: string; licenceFile: string; copyright: string };
  licences: Record<string, { name: string; url: string; text: string }>;
  components: Component[];
}

async function openAbout(page: import('@playwright/test').Page): Promise<Notices> {
  await page.goto('/#about');
  await expect(page.locator('.section-title', { hasText: 'Software Information' })).toBeVisible();
  return page.evaluate(() => JSON.parse(JSON.stringify((window as any).AquaNotices)));
}

/** All files under a directory, as paths relative to assets/web with `/` separators. */
function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((entry) => {
    const full = join(dir, entry);
    return statSync(full).isDirectory() ? walk(full) : [relative(WEB, full).split('\\').join('/')];
  });
}

const escapeRe = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/** `*` matches within one path segment; everything else is literal. */
function matches(pattern: string, file: string): boolean {
  return new RegExp('^' + pattern.split('*').map(escapeRe).join('[^/]*') + '$').test(file);
}

test.describe('About — licence and copyright', () => {
  test('licence line names the licence and LICENSE.txt and links to the source', async ({ page }) => {
    const { app } = await openAbout(page);

    expect(app.licence).toBe('GNU General Public License v3.0');
    expect(app.licenceFile).toBe('LICENSE.txt');

    const grid = page.locator('.info-grid').first();
    const licenceRow = grid.locator('dt', { hasText: /^Licence$/ }).locator('xpath=following-sibling::dd[1]');
    await expect(licenceRow).toContainText(`${app.licence} (${app.licenceFile})`);

    // The source link is the build's own homepage (CMake HOMEPAGE_URL), the same
    // value the Homepage row shows — one source of truth, so they cannot drift.
    const source = licenceRow.locator('a', { hasText: 'Source code' });
    await expect(source).toBeVisible();
    const homepage = grid.locator('dt', { hasText: /^Homepage$/ }).locator('xpath=following-sibling::dd[1]//a');
    await expect(source).toHaveAttribute('href', /^https?:\/\//);
    await expect(source).toHaveAttribute('href', (await homepage.getAttribute('href')) as string);
    await expect(source).toHaveAttribute('rel', /noopener/);
  });

  test('copyright line is shown', async ({ page }) => {
    const { app } = await openAbout(page);
    expect(app.copyright).toMatch(/^Copyright © \d{4} \S/);

    const row = page
      .locator('.info-grid')
      .first()
      .locator('dt', { hasText: /^Copyright$/ })
      .locator('xpath=following-sibling::dd[1]');
    await expect(row).toHaveText(app.copyright);
  });
});

test.describe('About — third-party licences', () => {
  test('every listed component is rendered with its copyright line and licence link', async ({ page }) => {
    const { components, licences } = await openAbout(page);
    expect(components.length).toBeGreaterThan(0);

    const section = page.locator('.about-notice');
    await expect(section).toHaveCount(components.length);

    for (const c of components) {
      const item = section.filter({ has: page.locator('.about-notice-name', { hasText: new RegExp(`^${escapeRe(c.name)}$`) }) });
      await expect(item, `component "${c.name}" rendered`).toHaveCount(1);
      for (const line of c.copyright) await expect(item).toContainText(line);
      for (const note of c.notes ?? []) await expect(item).toContainText(note);
      if (c.version) await expect(item.locator('code')).toHaveText(c.version);
      await expect(item.locator('a', { hasText: licences[c.licence].name })).toHaveAttribute('href', licences[c.licence].url);
    }
  });

  test('licence texts are one click away and are the full text', async ({ page }) => {
    const { licences } = await openAbout(page);

    const ofl = licences['OFL-1.1'].text;
    expect(ofl).toContain('SIL OPEN FONT LICENSE Version 1.1');
    expect(ofl).toContain('PERMISSION & CONDITIONS');
    expect(ofl).toContain('DISCLAIMER');
    const mit = licences['MIT'].text;
    expect(mit).toContain('Permission is hereby granted, free of charge');
    expect(mit).toContain('The above copyright notice and this permission notice shall be included');

    for (const lic of Object.values(licences)) {
      const toggle = page.locator('.about-licence-toggle', { hasText: lic.name });
      await expect(toggle).toHaveAttribute('aria-expanded', 'false');
      const pre = toggle.locator('xpath=following-sibling::pre');
      await expect(pre).toBeHidden();
      await toggle.click();
      await expect(toggle).toHaveAttribute('aria-expanded', 'true');
      await expect(pre).toBeVisible();
      await expect(pre).toContainText(lic.text.split('\n')[0]);
      await toggle.click();
      await expect(pre).toBeHidden();
    }
  });

  test('every vendored file and every declared font family has a notice', async ({ page }) => {
    const { components } = await openAbout(page);
    const patterns = components.flatMap((c) => c.files);

    // 1. Every file shipped under vendor/ (fonts.css is our own declaration file).
    const vendored = walk(join(WEB, 'vendor')).filter((f) => !f.endsWith('/fonts.css'));
    expect(vendored.length, 'found vendored files').toBeGreaterThan(0);
    const unclaimed = vendored.filter((f) => !patterns.some((p) => matches(p, f)));
    expect(unclaimed, 'vendored files with no entry in scripts/config/third-party-notices.js').toEqual([]);

    // 2. Every pattern claims something (a stale entry is as wrong as a missing one).
    const all = walk(WEB);
    const stale = patterns.filter((p) => !all.some((f) => matches(p, f)));
    expect(stale, 'notice entries whose files no longer exist').toEqual([]);

    // 3. Every font-family declared in fonts.css is named by a font entry.
    const css = readFileSync(join(WEB, 'vendor', 'fonts', 'fonts.css'), 'utf8');
    const families = [...new Set([...css.matchAll(/font-family:\s*'([^']+)'/g)].map((m) => m[1]))];
    expect(families.length, 'font families declared').toBeGreaterThan(0);
    const fontNames = components.filter((c) => c.kind === 'font').map((c) => c.name);
    expect(families.filter((f) => !fontNames.includes(f)), 'declared font families with no notice').toEqual([]);
  });
});
