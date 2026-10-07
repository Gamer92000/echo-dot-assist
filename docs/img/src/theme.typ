// Shared look of the README figures. tools/figures.sh renders every figure twice,
// `--input theme=light` and `--input theme=dark`; the README picks one per GitHub theme
// with <picture>. Colours follow GitHub's Primer palette so the figures sit on its pages.
#import "@preview/cetz:0.5.0": canvas, draw

#let dark = sys.inputs.at("theme", default: "light") == "dark"
#let pick(l, d) = if dark { rgb(d) } else { rgb(l) }

#let col = (
  text: pick("#1f2328", "#e6edf3"),
  muted: pick("#59636e", "#9198a1"),
  line: pick("#8c959f", "#656c76"),
  card: pick("#ffffff", "#151b23"),
  stock: pick("#eef1f4", "#262c36"),
  stock-edge: pick("#c4ccd5", "#3d444d"),
  accent: pick("#0969da", "#4493f8"),
  accent-fill: pick("#ddf4ff", "#0c2d53"),
  accent-text: pick("#0550ae", "#79c0ff"),
  red: pick("#cf222e", "#f85149"),
  red-fill: pick("#ffebe9", "#3c1618"),
  green: pick("#1a7f37", "#3fb950"),
  green-fill: pick("#dafbe1", "#12261e"),
  amber: pick("#9a6700", "#d29922"),
  amber-fill: pick("#fff8c5", "#2e2410"),
)

// The light ring's own colours (stock Alexa's animations), the same in both themes.
#let ring = (
  blue: rgb("#1f4fff"),
  cyan: rgb("#00d5ff"),
  red: rgb("#ff2d2d"),
  orange: rgb("#ff8a00"),
  purple: rgb("#8a3dff"),
  off: pick("#d0d7de", "#30363d"),
)

#let fig(body) = {
  set page(width: auto, height: auto, margin: 8pt, fill: none)
  set text(font: ("Noto Sans", "DejaVu Sans"), size: 10pt, fill: col.text)
  set par(leading: 0.5em)
  body
}

// Canvas in "pixels" (y grows downwards, as in the SVG they replace): 1 unit = 0.75pt.
#let px = 0.75pt
#let P(x, y) = (x, -y)

#let kinds = (
  stock: (col.stock, col.stock-edge, col.text),
  new: (col.accent-fill, col.accent, col.accent-text),
  card: (col.card, col.stock-edge, col.text),
  good: (col.green-fill, col.green, col.green),
  bad: (col.red-fill, col.red, col.red),
  warn: (col.amber-fill, col.amber, col.amber),
)

// A rounded box with a bold title and an optional muted line under it.
#let node(x, y, w, h, title, sub: none, kind: "stock", size: 11.5pt, pad: 14, align-x: left) = {
  let (fill, edge, tcol) = kinds.at(kind)
  draw.rect(P(x, y), P(x + w, y + h), radius: 10, fill: fill, stroke: (if kind == "stock" or kind == "card" { 1pt } else { 1.4pt }) + edge)
  let body = block(width: (w - 2 * pad) * px, align(align-x, {
    text(weight: 600, size: size, fill: if kind == "stock" or kind == "card" { col.text } else { tcol }, title)
    if sub != none { linebreak(); text(size: 9pt, fill: col.muted, sub) }
  }))
  draw.content(P(x + w / 2, y + h / 2), anchor: "center", body)
}

#let arrow(a, b, c: none, both: false, w: 1.3pt, dash: none) = {
  let c = if c == none { col.line } else { c }
  let m = (fill: c, scale: 0.9)
  draw.line(P(..a), P(..b), stroke: (paint: c, thickness: w, dash: dash),
    mark: if both { (start: "stealth", end: "stealth", ..m) } else { (end: "stealth", ..m) })
}

#let label(x, y, body, anchor: "center", c: none, size: 9pt) = draw.content(P(x, y), anchor: anchor,
  text(size: size, fill: if c == none { col.muted } else { c }, body))
