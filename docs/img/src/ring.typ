#import "theme.typ": *
#show: fig

// A ring from (start, stop, colour) segments, angles counter-clockwise from the right.
#let segs(c, parts, r: 30, w: 8pt) = for (a, b, paint) in parts {
  draw.arc(c, start: a * 1deg, stop: b * 1deg, radius: r, anchor: "origin", stroke: (paint: paint, thickness: w, cap: "butt"))
}
#let even(n, colours) = range(n).map(i => (i * 360 / n, (i + 1) * 360 / n, colours.at(calc.rem(i, colours.len()))))

#let states = (
  ([Listening], [it heard the wake word], even(1, (ring.blue,)) + ((55, 125, ring.cyan),)),
  ([Thinking], [Home Assistant is on it], even(12, (ring.blue, ring.cyan))),
  ([Speaking], [a reply plays (pulses)], even(1, (ring.cyan,))),
  ([Muted], [Microphone off pressed], even(1, (ring.red,))),
  ([Not set up], [until Home Assistant adds it], even(8, (ring.orange, ring.off))),
  ([Do not disturb], [flashes when switched on], even(1, (ring.purple,))),
  ([Bluetooth pairing], [phones can pair (chaser)], even(1, (ring.off,)) + ((30, 100, ring.blue),)),
  ([Identify], [which Echo is this? 10 s], even(12, range(12).map(i => color.hsv(i * 30deg, 85%, 100%)))),
)

#canvas(length: px, {
  import draw: *
  let (cw, ch) = (220, 150)
  for (i, (name, sub, parts)) in states.enumerate() {
    let x = calc.rem(i, 4) * cw + cw / 2
    let y = calc.div-euclid(i, 4) * ch + 44
    segs(P(x, y), parts)
    content(P(x, y + 50), anchor: "north", align(center, {
      text(size: 11pt, weight: 700, name)
      linebreak()
      text(size: 9pt, fill: col.muted, sub)
    }))
  }
})
