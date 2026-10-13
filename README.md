# Docs theme — Aqualink Automate brand skin

This directory is the `custom_dir` (wired via `theme.custom_dir` in
`mkdocs.yml`) for the docs site, built by [Zensical](https://zensical.org) —
the Rust-core successor to MkDocs + Material that reads this same config
(`theme.variant: classic` keeps it on Material's look). It layers the web-UI's
visual design on top of the stock theme — **no fork, no pinned theme version
bump**. Everything degrades to the plain theme if a file here is removed.

## What's here

| Path | Role |
|------|------|
| `main.html` | Extends `base.html`. Injects the landing **hero** on the homepage only and adds brand `theme-color` meta. Homepage detection falls back to `nav.homepage` because Zensical's classic variant leaves `page.is_homepage` falsy — see the comment in the file. |
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

## Preview locally

```bash
pip install zensical
zensical serve          # http://127.0.0.1:8000/
zensical build --strict # what CI (.github/workflows/docs.yml) runs
```

Excluded docs (`design/`, `refactoring/`, `alwin32/`, …) still get built —
Zensical doesn't honour `exclude_docs` — but stay unlinked from `nav`. Run
`python scripts/zensical-prune-excluded.py` after the build to see the site
exactly as it publishes.

The published site (built by `docs.yml` → `gh-pages`) is unaffected structurally:
this is a presentation layer only, and it shares `gh-pages` with the signed
APT/DNF repos exactly as before (disjoint paths under `site/`).
