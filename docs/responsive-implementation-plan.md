# Responsive Web UI — Implementation Plan

_Aqualink Automate · drafted 2026-07-04 · decisions locked_

## Goal & guardrails
Translate the locked responsive mockup into the shipping Alpine.js/CSS app so every view reflows for phone, tablet (portrait + landscape) and desktop, in light/dark and LTR/RTL — **without regressing desktop** and **with automated coverage at every size**.

**Non-goals:** no native apps; no new HTTP/WebSocket API (presentation only → `assets/web/api/swagger.yaml` untouched); no rebuild of dark mode / RTL / i18n (reuse what ships).

## Locked decisions
1. **Equipment = tap-tiles everywhere, including desktop** (icon + name, fills when on; no switch, no On/Off text). Desktop drops its current switch-style controls → one control model across all sizes.
2. **Mobile alerts = bottom sheet** (the bell opens a sheet from the bottom).
3. **Visual-regression snapshots for all 8 locales** × viewport × theme (in addition to the missing-key / pseudo-locale checks that already cover all 8).
4. **Merge each phase into local `develop`** (no per-phase GitHub PRs); desktop stays shippable throughout; **tests written per-phase, not at the end**.

## Mechanism
- **Viewport media queries** (not container queries): the app is full-viewport, and we need `(orientation: landscape)` which is viewport-based.
- Breakpoints: `< 640` phone · `640–1023` tablet · `≥ 1024` desktop; plus `(min-width: 768px) and (orientation: landscape)` for the iPad-landscape dashboard.
- New components use **logical properties** (`inset-inline`, `margin-inline-start`, `text-align: start`) and **design tokens only** — never hardcoded colors. Themed roots must set `color: var(--text)` (root cause of the dark-mode black-on-black bug).

## What already exists (reuse, don't rebuild)
- Dark mode: `$store.theme.toggle()` / `$store.theme.isDark`, `:root[data-theme=...]` OKLCH tokens.
- RTL + i18n: `dir`, `scripts/i18n.js`, `i18n/*.js` (8 locales, ~769 keys), CI `i18n-catalogs` job, `e2e/i18n.spec.ts` (missing-key + pseudo-locale scan).
- Modals/overlays: login via `$store.auth.showLogin`; admin/account via `admin:open` / `account:open` CustomEvents; alerts dropdown in nav.
- Test harness: `e2e/*` Playwright against replay fixtures; `scripts/capture-doc-screenshots.js` boots the real binary against the same fixtures.

## Phases (each merged to local develop)

### Phase 0 — Foundations
- Files: `styles/app.css` (documented breakpoint block), **new** `scripts/stores/layout-store.js`, `index.html` (script include + store registration).
- Add `layout` store: reactive `isPhone/isTablet/isDesktop/isLandscape` via `matchMedia` listeners + `mobileNavOpen` + `activeSheet` (null | 'more' | 'login' | 'account' | 'admin' | 'alerts' | 'schedule' | 'device').
- Done: store reactive to resize/orientation; no visual change.

### Phase 1 — Navigation shell
- Files: `index.html` (nav), `styles/app.css` + `components.css`, `layout-store.js`, all 8 `i18n/*.js`.
- Keep top nav ≥1024. Add **bottom tab bar** `<640` (Dashboard · Trends · Schedules · More) with `env(safe-area-inset-bottom)`. Add **hamburger + drawer** 640–1023. Build the **More sheet** (overflow routes Detailed/Settings/About/Diagnostics + theme toggle + account/admin/sign-in, reusing existing controls/events).
- Done: correct nav per width; ESC/outside close; RTL mirrors; nav-pattern e2e assertions pass.

### Phase 2 — Dashboard reflow (hero)
- Files: `views/dashboard-view.js`, `index.html` dashboard block, `components/pool-graphic.js`, `equipment-button.js`, `chemistry-gauge.js`, `chlorinator-control.js`.
- Compact status header (pump/heater/chlorinator icons) vs desktop summary strip. **Tap-tiles everywhere** (rework `equipment-button.js`). **Heater card with integrated setpoints**; delete standalone setpoints; circulation standalone. **Consolidated chemistry + chlorinator card** on phone/tablet; circular gauges on desktop. Section reorder via CSS `order` at phone breakpoint.
- Done: matches mockup at each width; snapshots.

### Phase 3 — Remaining views
- Files: `trends-view.js`, `schedules-view.js`, `settings-view.js`, `detailed-view.js`, `diagnostics-view.js`, `about-view.js`.
- Trends → **consolidated series list** (toggle + current + min/max/avg, 1/2/3-up). Schedules rows horizontal→stacked. Settings 2-col→1-col ≤ iPad. Detailed grouped table. Diagnostics grids reflow + health card stacks. About version meta + lang grid.
- Done: each view reflows; snapshots.

### Phase 4 — Modals as responsive sheets
- Files: auth overlay in `index.html`, `account-view.js`, `admin-view.js` + `admin-*-view.js`, `schedules-view.js` (editor), `device-card.js`, alerts markup, `components.css`.
- Shared sheet pattern: full-screen sheet `<640` → centered dialog `≥640`. Apply to login/setup, account, admin, schedule editor, device detail. **Admin section switcher = 2-col grid on phone**. **Alerts = bottom sheet on phone.** Preserve focus-trap + ESC.
- Done: modals are sheets on phone; a11y intact; snapshots.

### Phase 5 — iPad landscape orientation layout
- Files: `styles/app.css` (scoped `@media … orientation:landscape`), `dashboard-view.js`.
- Dashboard-only grid-areas: status + chemistry paired / equipment action-bar / heater + circulation. Scoped so other views/orientations are unaffected.
- Done: landscape iPad shows action-bar layout; portrait unchanged.

### Phase 6 — i18n completeness
- Files: `i18n/en.js` + all 7 locales; run `scripts/check-i18n-keys.ps1`.
- Every new string (tab labels, More, drawer, sheet titles) in all 8 locales, mirrored in RTL.
- Done: `i18n-catalogs` job green; pseudo-locale scan clean.

### Phase 7 — Testing matrix (first-class)
- Files: **new** `e2e/responsive.spec.ts`, extend `e2e/i18n.spec.ts`, CI e2e job.
- Matrix: viewport {375, 390, 820, 1180, 1280, 1440} × theme {light, dark} × dir {ltr, rtl} × locale {all 8}.
- Per cell assert: correct nav-pattern (tab bar <640 / hamburger 640–1023 / inline ≥1024), grid column counts, modal-as-sheet vs dialog, **no horizontal overflow** (`scrollWidth ≤ clientWidth`), **`toHaveScreenshot`** visual regression, and a **contrast guard** (computed text color ≠ background on key headings).
- Extend the i18n missing-key + pseudo-locale checks to run at **every viewport** (today effectively desktop-only).
- Deterministic replay fixture + fixed clock + masked live/timestamp regions.

### Phase 8 — Docs & screenshots
- Files: `scripts/capture-doc-screenshots.js`, `docs/assets/*.png`, `docs/usage-and-api.md`, `docs/i18n.md`, `README.md`.
- Add mobile/tablet captures; regenerate embedded PNGs; document responsive nav/breakpoints (CLAUDE.md doc-accuracy rule).

## Sequencing
`0 → 1 → (2 ‖ 3) → 4 → 5 → 6 → 7 → 8`. Phase 7's harness is scaffolded during Phase 1 and grows with each phase.

## Risks & mitigations
- Desktop regression → snapshot baselines captured before changes + "desktop unchanged" assertions.
- Hardcoded colors → tokens + logical props only; contrast guard in CI.
- Snapshot flakiness → replay fixtures + fixed clock + masked live regions.
- Scope creep → phase gates (one merge to develop each).

## Rollout
Branch `feat/responsive-ui`; each phase committed and fast-forwarded into local `develop` (race-safe `update-ref` CAS); new e2e matrix in the test suite; docs updated alongside.
