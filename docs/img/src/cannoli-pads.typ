// The Echo Dot 5's USB test pads (devices/cannoli/README.md "Wiring"), traced from a photo of the board behind the
// base's rubber foot, as printed: "FLEX SIDE" at the top.  Coordinates are the photo's pixels (405 x 300 of it).
#import "theme.typ": *
#show: fig

// The board's own colours, the same in both themes (as the ring's).
#let pcb = rgb("#4b6b3a")
#let silk = rgb("#eef0e6")
#let gold = rgb("#dfa847")
#let gold-edge = rgb("#94701f")
#let hole = rgb("#141b12")
#let wire = (
  gnd: (rgb("#1b1f24"), rgb("#ffffff")),
  dm: (rgb("#ffffff"), rgb("#1b1f24")),
  dp: (rgb("#2da44e"), rgb("#ffffff")),
  vcc: (rgb("#d1242f"), rgb("#ffffff")),
)

#canvas(length: px, {
  import draw: *
  let S = 1.5
  let (ox, oy) = (0, 64)
  let Q(x, y) = P(ox + x * S, oy + y * S)
  let r(v) = v * S
  let silkline(..pts) = line(..pts.pos().map(p => Q(..p)), stroke: (paint: silk, thickness: 1.8pt, join: "miter"))
  let silktext(x, y, body, size: 8pt) = content(Q(x, y), text(size: size, weight: 700, fill: silk, body))
  // a cross-hatched pad: the copper shows through the mask as a 3 x 3 grid
  let hatch(x, y, c) = for d in (-5, 0, 5) {
    line(Q(x + d, y - 9), Q(x + d, y + 9), stroke: 0.8pt + c)
    line(Q(x - 9, y + d), Q(x + 9, y + d), stroke: 0.8pt + c)
  }

  content(P(405 * S / 2, 14), text(size: 12pt, weight: 700)[Echo Dot 5 (cannoli): USB test pads])
  content(P(405 * S / 2, 38), text(size: 9pt, fill: col.muted)[in the recess behind the base's rubber foot; the board
    as printed, #text(weight: 600)[FLEX SIDE] at the top])

  rect(Q(0, 0), Q(405, 300), radius: 16, fill: pcb, stroke: 1pt + col.stock-edge)

  // landmarks above the matrix
  for x in (50, 345) { rect(Q(x - 10, 14), Q(x + 10, 66), radius: r(10), fill: hole, stroke: 3pt + gold) }
  circle(Q(24, 100), radius: r(19), fill: gold, stroke: 1pt + gold-edge)
  circle(Q(108, 95), radius: r(19), fill: gold, stroke: 1pt + gold-edge)
  circle(Q(178, 42), radius: r(7), fill: hole, stroke: 2pt + gold)
  silktext(246, 66, [FLEX SIDE], size: 10pt)
  rect(Q(244, 90), Q(282, 105), radius: r(7), fill: pcb, stroke: 2.5pt + gold)
  circle(Q(178, 115), radius: r(16), fill: pcb, stroke: 4pt + gold)
  silktext(64, 128, [FL2])
  silktext(276, 130, [J9])
  silktext(52, 270, [R1])
  silktext(366, 248, [L5])

  // the two large plated screw holes either side
  for x in (62, 290) { circle(Q(x, 192), radius: r(37), fill: hole, stroke: 9pt + gold) }

  // silkscreen outline: open corners at the top (the round pad between them), a tab at the bottom
  silkline((99, 150), (99, 121), (156, 121))
  silkline((199, 121), (252, 121), (252, 155))
  silkline((97, 234), (97, 266), (160, 266), (160, 285), (190, 285), (190, 266), (252, 266), (252, 238))
  silkline((106, 238), (111, 250), (116, 238))

  // the nine pads; the centre one and the one right of it cross-hatched
  for (y, row) in ((155, (0, 0, 0)), (192, (0, 1, 1)), (230, (0, 0, 0))) {
    for (i, x) in (137, 176, 214).enumerate() {
      circle(Q(x, y), radius: r(12), fill: gold, stroke: 1pt + gold-edge)
      if row.at(i) == 1 { hatch(x, y, gold-edge) }
    }
  }
  // the three used, coloured as the USB wires they take
  let used(x, y, (fill, ink), name, hatched: false) = {
    circle(Q(x, y), radius: r(12.5), fill: fill, stroke: 1.5pt + rgb("#8c959f"))
    if hatched { hatch(x, y, rgb("#3d444d")) }
    content(Q(x, y), text(size: 8pt, weight: 800, fill: ink, name))
  }
  used(176, 192, wire.gnd, [GND], hatched: true)
  used(176, 230, wire.dm, [D−])
  used(137, 230, wire.dp, [D+])

  // the cable's wires, what each one goes to
  let ly = oy + 300 * S + 28
  let rows = (
    (wire.gnd, [GND], [the centre pad of the nine (cross-hatched, as the one right of it)]),
    (wire.dm, [D−], [the pad straight below GND]),
    (wire.dp, [D+], [the corner pad left of D−]),
    (wire.vcc, [+5 V], [not connected: insulate it. The Dot runs on its own 15 V adapter]),
  )
  for (i, ((fill, _), name, what)) in rows.enumerate() {
    let y = ly + i * 26
    rect(P(16, y - 5), P(64, y + 5), radius: 5, fill: fill, stroke: 1pt + col.line)
    if name == [+5 V] {
      line(P(70, y - 6), P(82, y + 6), stroke: 2pt + col.red)
      line(P(70, y + 6), P(82, y - 6), stroke: 2pt + col.red)
    }
    content(P(92, y), anchor: "west", text(size: 9.5pt)[#text(weight: 700, name) #h(4pt) #what])
  }
})
