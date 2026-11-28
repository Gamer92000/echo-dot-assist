// The Echo 4's USB test pads (devices/laser/README.md "Wiring"), traced from a photo of the board, turned so that
// its printing reads upright.  Coordinates are the turned photo's pixels (760 x 790 of it).
#import "theme.typ": *
#show: fig

// The board's own colours, the same in both themes (as cannoli-pads.typ's).
#let pcb = rgb("#2f5a3a")
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
  let S = 0.62
  let (W, H) = (760, 790)
  let (ox, oy) = (0, 64)
  let Q(x, y) = P(ox + x * S, oy + y * S)
  let r(v) = v * S
  let silktext(x, y, body, size: 7.5pt) = content(Q(x, y), text(size: size, weight: 700, fill: silk, body))
  let square(x, y) = rect(Q(x - 11, y - 11), Q(x + 11, y + 11), fill: gold, stroke: 0.8pt + gold-edge)

  content(P(W * S / 2, 14), text(size: 12pt, weight: 700)[Echo 4 (laser): USB test pads])
  content(P(W * S / 2, 38), text(size: 9pt, fill: col.muted)[two columns of three large round pads, TM1 to TM6;
    the board turned so its printing reads upright])

  rect(Q(0, 0), Q(W, H), radius: 16, fill: pcb, stroke: 1pt + col.stock-edge)

  // landmarks above the pads: the two oval slots, the large hole, the screw at the far corner
  rect(Q(102, 15), Q(158, 45), radius: r(15), fill: hole, stroke: 3pt + silk)
  rect(Q(42, 219), Q(102, 245), radius: r(13), fill: hole, stroke: 3pt + silk)
  circle(Q(95, 357), radius: r(38), fill: hole, stroke: 4pt + rgb("#5d6b5a"))
  circle(Q(78, 752), radius: r(28), fill: rgb("#8c8f86"), stroke: 2pt + rgb("#5d6058"))

  // the small parts to the right of the pads
  square(100, 113)
  silktext(48, 112, [TM12])
  circle(Q(170, 115), radius: r(9), fill: gold, stroke: 0.8pt + gold-edge)
  silktext(170, 82, [TP5])
  square(237, 85)
  silktext(232, 140, [TM14])
  rect(Q(343, 74), Q(388, 134), radius: r(4), fill: rgb("#30363d"), stroke: 1pt + silk)
  silktext(365, 158, [L14])
  square(490, 197)
  silktext(440, 205, [TM48])
  silktext(548, 300, [CR28])

  // the row of small square test pads below them
  for (x, y, name, lx, ly) in (
    (530, 678, [TM25], 478, 690),
    (535, 752, [TM28], 600, 752),
    (617, 605, [TM24], 560, 605),
    (620, 678, [TM23], 690, 670),
    (640, 548, [TM8], 585, 535),
    (635, 475, [TM9], 580, 462),
    (683, 395, [TM11], 625, 405),
    (627, 337, [TM10], 690, 315),
    (712, 545, none, 0, 0),
    (705, 615, none, 0, 0),
  ) {
    square(x, y)
    if name != none { silktext(lx, ly, name) }
  }

  // the six large round pads, each with its printed name
  let pads = (
    (347, 305, [TM1], 270, 305),
    (347, 412, [TM6], 270, 412),
    (347, 520, [TM5], 270, 520),
    (458, 302, [TM2], 458, 250),
    (458, 410, [TM3], 458, 358),
    (458, 518, [TM4], 458, 466),
  )
  for (x, y, name, lx, ly) in pads {
    circle(Q(x, y), radius: r(32), fill: gold, stroke: 1pt + gold-edge)
    silktext(lx, ly, name, size: 9pt)
  }
  // the three used, coloured as the USB wires they take
  let used(x, y, (fill, ink), name) = {
    circle(Q(x, y), radius: r(33), fill: fill, stroke: 1.5pt + rgb("#8c959f"))
    content(Q(x, y), text(size: 8.5pt, weight: 800, fill: ink, name))
  }
  used(347, 412, wire.gnd, [GND])
  used(347, 305, wire.dp, [D+])
  used(458, 302, wire.dm, [D−])

  // the cable's wires, what each one goes to
  let ly = oy + H * S + 28
  let rows = (
    (wire.gnd, [GND], [TM6, the middle pad of the column TM1, TM6, TM5]),
    (wire.dp, [D+], [TM1, the pad above GND]),
    (wire.dm, [D−], [TM2, the pad right of D+]),
    (wire.vcc, [+5 V], [not connected: insulate it. The Echo runs on its own adapter]),
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
