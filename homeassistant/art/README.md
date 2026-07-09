# Add-on art sources

Source SVGs for the Home Assistant add-on's `icon.png` (256×256) and `logo.png`
(640×200). This folder has no `config.yaml`, so the Supervisor ignores it — it is not
an add-on.

The mark is the app's own favicon motif (`assets/web/favicon.svg`): a sky-blue water
drop (`#38bdf8 → #0284c7`) with white ripples on a slate tile (`#1e293b → #0f172a`), so
it reads on any Home Assistant theme.

## Regenerate the PNGs

The rendered PNGs live in the **stable** add-on folder; the edge channel gets them via
`scripts/gen-homeassistant-edge-addon.ps1` (it copies all files verbatim). Rasterise with
[`@resvg/resvg-js`](https://github.com/yisibl/resvg-js) (pure Rust, no native deps):

```bash
npm install @resvg/resvg-js
node -e '
  const { Resvg } = require("@resvg/resvg-js"); const fs = require("fs");
  const out = "../../aqualink-automate";
  for (const [svg, png, w] of [["icon.svg","icon.png",256],["logo.svg","logo.png",640]]) {
    const r = new Resvg(fs.readFileSync(svg), {
      fitTo: { mode: "width", value: w },
      font: { loadSystemFonts: true },      // resolves the Segoe UI / sans wordmark
      background: "rgba(0,0,0,0)",
    });
    fs.writeFileSync(out + "/" + png, r.render().asPng());
  }
'
```

Then re-run the edge generator and commit both folders' PNGs.
