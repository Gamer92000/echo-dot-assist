#import "theme.typ": *
#show: fig

// docs/WINDOWS.md: the fastboot gesture on an Echo Dot 2 as firebreak 0.8.0 asks for it (devices.py: "Unplug the
// USB cable, press and hold the action button (the one with a dot), plug the cable back in, and let go when the
// light ring turns green"), and what the ring says meanwhile (firebreak's README "Notes", devices/biscuit/README.md).
// No drawing of the Echo itself: the button and ring as buttons.typ and ring.typ draw them, so the README's figures
// and this one speak one language.

#let knob(c, r: 30) = draw.circle(c, radius: r, fill: col.stock, stroke: 1.2pt + col.stock-edge)
#let action(x, y, held: false) = {
  knob(P(x, y))
  draw.circle(P(x, y), radius: 7, fill: col.text, stroke: none)
  if held { draw.circle(P(x, y), radius: 38, fill: none, stroke: (paint: col.accent, thickness: 2.5pt, dash: "dashed")) }
}
// a micro-USB plug on its cable, pointing left; `gap` px short of the socket at x
#let plug(x, y, gap: 0) = {
  import draw: *
  rect(P(x - 12, y - 8), P(x + 2, y + 8), radius: 2, fill: none, stroke: 1.6pt + col.text)   // the socket
  rect(P(x + gap, y - 5), P(x + gap + 14, y + 5), radius: 1.5, fill: col.line, stroke: none)  // metal tip
  rect(P(x + gap + 14, y - 9), P(x + gap + 36, y + 9), radius: 3, fill: col.text, stroke: none)
  line(P(x + gap + 36, y), P(x + gap + 66, y), stroke: 4pt + col.text)
}
#let ringed(x, y, paint, r: 30) = draw.circle(P(x, y), radius: r, fill: none, stroke: 8pt + paint)
#let rainbow = range(12).map(i => color.hsv(i * 30deg, 85%, 100%))
#let green = rgb("#2ad15a")

#canvas(length: px, {
  import draw: *
  let cw = 220
  let cy = 80

  let scenes = (
    ([Unplug the USB cable], x => plug(x - 30, cy, gap: 26)),
    ([Press and hold the action button: the one with the dot], x => action(x, cy, held: true)),
    ([Keep holding it and plug the cable back in], x => { action(x - 44, cy, held: true); plug(x + 10, cy) }),
    ([Let go when the light ring turns green], x => ringed(x, cy, green)),
  )
  for (i, (what, show-it)) in scenes.enumerate() {
    let x = i * cw + cw / 2
    show-it(x)
    circle(P(i * cw + 16, 16), radius: 13, fill: col.accent-fill, stroke: 1.6pt + col.accent)
    content(P(i * cw + 16, 16), text(size: 10pt, weight: 700, fill: col.accent-text, str(i + 1)))
    content(P(x, 136), anchor: "north", block(width: (cw - 24) * px, align(center, text(size: 10pt, weight: 600, what))))
    if i < 3 { arrow((i * cw + cw - 12, cy), (i * cw + cw + 12, cy)) }
  }

  // what the ring says
  let top = 204
  line(P(0, top - 14), P(4 * cw, top - 14), stroke: 1pt + col.stock-edge)
  content(P(0, top + 4), anchor: "west", text(size: 11.5pt, weight: 700)[What the light ring tells you])
  let signs = (
    (green, "good", [Green], [fastboot: firebreak takes over]),
    (rainbow, "good", [Rainbow], [unlocked before: fine, too]),
    (ring.blue, "bad", [Stays solid blue], [no data on USB: drivers missing, a charge-only cable or a bad port]),
    (rgb("#ffffff"), "warn", [White], [TWRP: firebreak at work. #text(weight: 700)[Do not unplug]]),
  )
  for (i, (c, kind, name, what)) in signs.enumerate() {
    let x = i * cw
    let (cx, cy) = (x + 30, top + 60)
    let (fill, edge, tcol) = kinds.at(kind)
    if type(c) == array {
      for (j, h) in c.enumerate() {
        arc(P(cx, cy), start: j * 30deg, stop: (j + 1) * 30deg, radius: 22, anchor: "origin", stroke: (paint: h, thickness: 7pt, cap: "butt"))
      }
    } else {
      ringed(cx, cy, c, r: 22)
      // white on a light page: an outline so it shows
      if c == rgb("#ffffff") {
        circle(P(cx, cy), radius: 26, fill: none, stroke: 0.8pt + col.stock-edge)
        circle(P(cx, cy), radius: 18, fill: none, stroke: 0.8pt + col.stock-edge)
      }
    }
    content(P(x + 64, cy), anchor: "west", block(width: (cw - 72) * px, {
      text(size: 10.5pt, weight: 700, fill: tcol, name)
      linebreak()
      text(size: 9pt, fill: col.muted, what)
    }))
  }
})
