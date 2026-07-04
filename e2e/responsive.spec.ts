import { test, expect, type Page } from '@playwright/test';

/**
 * Responsive layout matrix (Phase 7 of the responsive UI work).
 *
 * Asserts the reflow the app promises at each breakpoint:
 *   - the navigation PATTERN per width — bottom tab bar (< 640), hamburger
 *     drawer (640–1023), inline nav (>= 1024);
 *   - NO horizontal overflow on any view at any width;
 *   - RTL mirroring (dir=rtl) under an Arabic locale, still overflow-free;
 *   - a CONTRAST guard in dark mode (a heading's text colour must differ from
 *     its background — this would have caught the dark-mode black-on-black
 *     regression that shipped before this work).
 *
 * These structural assertions are OS-independent and always run. Full-page
 * VISUAL snapshots are opt-in (RESPONSIVE_SNAPSHOTS=1) because Playwright's
 * pixel baselines are per-platform: seed them on the target OS with
 *   RESPONSIVE_SNAPSHOTS=1 npx playwright test e2e/responsive.spec.ts --update-snapshots
 *
 * Runs against the default (unauthenticated) webServer + sample_session fixture;
 * the equipment-rich dashboard visuals live in the standalone
 * scripts/capture-responsive-screenshots.js harness (which can boot the OneTouch
 * fixture and inject history/schedules). Here the concern is structural
 * correctness + regression protection.
 */

const SNAPSHOTS = !!process.env.RESPONSIVE_SNAPSHOTS;

type NavPattern = 'tabbar' | 'hamburger' | 'inline';
const VIEWPORTS: { name: string; width: number; height: number; nav: NavPattern }[] = [
  { name: 'phone-se', width: 375, height: 667, nav: 'tabbar' },
  { name: 'phone', width: 390, height: 780, nav: 'tabbar' },
  { name: 'tablet-portrait', width: 820, height: 1180, nav: 'hamburger' },
  { name: 'tablet-landscape', width: 1180, height: 820, nav: 'inline' },
  { name: 'desktop', width: 1280, height: 900, nav: 'inline' },
];
const VIEWS = ['dashboard', 'detailed', 'trends', 'schedules', 'settings', 'diagnostics', 'about'];

async function horizontalOverflow(page: Page): Promise<number> {
  return page.evaluate(() => {
    const el = document.scrollingElement || document.documentElement;
    return el.scrollWidth - el.clientWidth;
  });
}

async function expectNavPattern(page: Page, pattern: NavPattern): Promise<void> {
  const tabbar = page.locator('.mobile-tab-bar');
  const hamburger = page.locator('nav.app-nav .nav-hamburger');
  const links = page.locator('nav.app-nav .nav-links');
  if (pattern === 'tabbar') {
    await expect(tabbar, 'phone: bottom tab bar visible').toBeVisible();
    await expect(links, 'phone: inline nav-links hidden').toBeHidden();
  } else if (pattern === 'hamburger') {
    await expect(hamburger, 'tablet: hamburger visible').toBeVisible();
    await expect(links, 'tablet: inline nav-links hidden').toBeHidden();
    await expect(tabbar, 'tablet: no bottom tab bar').toBeHidden();
  } else {
    await expect(links, 'desktop: inline nav-links visible').toBeVisible();
    await expect(hamburger, 'desktop: no hamburger').toBeHidden();
    await expect(tabbar, 'desktop: no bottom tab bar').toBeHidden();
  }
}

const mask = (page: Page) => ({ mask: [page.locator('.freshness-wrap')] });

for (const vp of VIEWPORTS) {
  test.describe(`${vp.name} (${vp.width}x${vp.height})`, () => {
    test.use({ viewport: { width: vp.width, height: vp.height } });

    for (const view of VIEWS) {
      test(`${view}: no horizontal overflow`, async ({ page }) => {
        await page.goto(`/#${view}`);
        await page.waitForTimeout(500);
        const overflow = await horizontalOverflow(page);
        expect(overflow, `${view} overflows horizontally by ${overflow}px at ${vp.width}px`).toBeLessThanOrEqual(1);
      });
    }

    test('navigation pattern matches the width', async ({ page }) => {
      await page.goto('/#dashboard');
      await page.waitForTimeout(400);
      await expectNavPattern(page, vp.nav);
    });

    test('dashboard visual snapshot', async ({ page }) => {
      test.skip(!SNAPSHOTS, 'set RESPONSIVE_SNAPSHOTS=1 (+ --update-snapshots to seed platform baselines)');
      await page.goto('/#dashboard');
      await page.waitForTimeout(800);
      await expect(page).toHaveScreenshot(`dashboard-${vp.name}.png`, { fullPage: true, maxDiffPixelRatio: 0.02, ...mask(page) });
    });
  });
}

test.describe('dark theme (phone)', () => {
  // colorScheme drives the theme store (it reads prefers-color-scheme when no
  // explicit choice is stored), which is more reliable than seeding localStorage.
  test.use({ viewport: { width: 390, height: 780 }, colorScheme: 'dark' });

  test('dashboard renders with legible headings (no black-on-black)', async ({ page }) => {
    await page.goto('/#dashboard');
    await page.waitForTimeout(700);
    // Contrast guard, format-agnostic: paint each colour onto a 1x1 canvas to
    // resolve oklch()/any format to RGB, then compute the WCAG contrast ratio of
    // a visible heading against the nearest opaque background. Black-on-black is
    // ~1.0; a legitimate dim label on a surface is well above the 1.6 floor.
    const ratio = await page.evaluate(() => {
      const toRgb = (c: string): number[] => {
        const cv = document.createElement('canvas'); cv.width = cv.height = 1;
        const ctx = cv.getContext('2d')!; ctx.fillStyle = '#000'; ctx.fillStyle = c;
        ctx.fillRect(0, 0, 1, 1); const d = ctx.getImageData(0, 0, 1, 1).data; return [d[0], d[1], d[2]];
      };
      const rl = (rgb: number[]) => {
        const f = (v: number) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * f(rgb[0]) + 0.7152 * f(rgb[1]) + 0.0722 * f(rgb[2]);
      };
      const el = [...document.querySelectorAll('.section-title, .summary-label')]
        .find((e) => { const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; });
      if (!el) return 99;
      let node: Element | null = el, bg = 'rgb(255,255,255)';
      while (node) {
        const b = getComputedStyle(node).backgroundColor;
        if (b && b !== 'rgba(0, 0, 0, 0)' && b !== 'transparent') { bg = b; break; }
        node = node.parentElement;
      }
      const L1 = rl(toRgb(getComputedStyle(el).color)), L2 = rl(toRgb(bg));
      const [hi, lo] = L1 > L2 ? [L1, L2] : [L2, L1];
      return (hi + 0.05) / (lo + 0.05);
    });
    expect(ratio, 'heading contrast ratio too low (black-on-black?)').toBeGreaterThan(1.6);
  });

  test('dashboard dark visual snapshot', async ({ page }) => {
    test.skip(!SNAPSHOTS, 'set RESPONSIVE_SNAPSHOTS=1 to seed baselines');
    await page.goto('/#dashboard');
    await page.waitForTimeout(800);
    await expect(page).toHaveScreenshot('dashboard-phone-dark.png', { fullPage: true, maxDiffPixelRatio: 0.02, ...mask(page) });
  });
});

test.describe('RTL / Arabic (phone)', () => {
  test.use({ viewport: { width: 390, height: 780 } });
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => localStorage.setItem('locale', 'ar'));
  });

  test('dashboard mirrors to RTL and stays overflow-free', async ({ page }) => {
    await page.goto('/#dashboard');
    await page.waitForTimeout(900);
    await expect(page.locator('html')).toHaveAttribute('dir', 'rtl');
    const overflow = await horizontalOverflow(page);
    expect(overflow, `RTL dashboard overflows by ${overflow}px`).toBeLessThanOrEqual(1);
  });

  test('dashboard RTL visual snapshot', async ({ page }) => {
    test.skip(!SNAPSHOTS, 'set RESPONSIVE_SNAPSHOTS=1 to seed baselines');
    await page.goto('/#dashboard');
    await page.waitForTimeout(900);
    await expect(page).toHaveScreenshot('dashboard-phone-rtl.png', { fullPage: true, maxDiffPixelRatio: 0.02, ...mask(page) });
  });
});
