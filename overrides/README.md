# Docs theme — Aqualink Automate brand skin

This directory is the MkDocs Material `custom_dir` (wired via `theme.custom_dir`
in `mkdocs.yml`). It layers the web-UI's visual design on top of stock Material
for MkDocs — **no fork, no pinned Material version bump**. Everything degrades to
plain Material if a file here is removed.

## What's here

| Path | Role |
|------|------|
| `main.html` | Extends Material's `base.html`. Injects the landing **hero** on the homepage only (`page.is_homepage`) and adds brand `theme-color` meta. |
| `partials/hero-graphic.svg` | Self-contained pool/spa/water-drop illustration for the hero. Inherits the `--aa-*` palette vars from the page, so it recolours with the light/dark toggle. |
| `../docs/stylesheets/aqualink.css` | The skin: self-hosted brand fonts, the custom OKLCH colour schemes, header/admonition polish, and hero styles. Linked via `extra_css`. |
| `../docs/assets/fonts/*.woff2` | Vendored Bricolage Grotesque + Hanken Grotesk + Noto Sans Arabic/Hebrew (copied from `assets/web/vendor/fonts/`). `theme.font: false` disables Google Fonts. |
| `../docs/assets/brand/favicon.svg` | The water-drop mark (copied from `assets/web/favicon.svg`); used as both `logo` and `favicon`. |

## How the colours work

`aqualink.css` does **not** define new Material colour schemes from scratch — it
retints the built-in `slate` (dark, the default) and `default` (light) schemes by
overriding the `--md-*` custom properties. `palette.primary`/`accent` are set to
`custom` so Material doesn't fight the overrides. The values are copied verbatim
from the web UI's `:root[data-theme=...]` tokens in
[`assets/web/styles/app.css`](../assets/web/styles/app.css) — **that file is the
source of truth.** When the app palette changes, update the `--aa-*` blocks here
to match.

Fonts use the same trick: with `theme.font: false`, Material builds body/code
font-family from the `--md-text-font` / `--md-code-font` primitives, so those are
set to the brand faces rather than overriding `--md-*-font-family`.

## Social cards

Open Graph / Twitter preview images are auto-generated per page by Material's
`social` plugin (`mkdocs.yml` → `plugins.social`), styled dark (`#0f172a`) with
Hanken Grotesk. The plugin is gated with `enabled: !ENV [CI, false]`:

- **CI** (GitHub Actions sets `CI=true`) → cards render. `docs.yml` installs
  `mkdocs-material[imaging]` + the Cairo system libs the imaging backend needs.
- **Local** (no `CI` env) → the plugin is disabled, so `mkdocs serve` / `mkdocs
  build --strict` work without Cairo installed. To preview cards locally, install
  the imaging deps and run with `CI=true mkdocs build`.

## Preview locally

```bash
pip install "mkdocs-material>=9.7"   # add [imaging] only to preview social cards
mkdocs serve          # http://127.0.0.1:8000/aqualink-automate/
mkdocs build --strict # what CI (.github/workflows/docs.yml) runs
```

The published site (built by `docs.yml` → `gh-pages`) is unaffected structurally:
this is a presentation layer only, and it shares `gh-pages` with the signed
APT/DNF repos exactly as before (disjoint paths under `site/`).
