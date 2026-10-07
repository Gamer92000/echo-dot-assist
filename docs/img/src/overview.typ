#import "theme.typ": *
#show: fig

#canvas(length: px, {
  import draw: *

  // legend
  rect(P(20, 14), P(36, 30), radius: 4, fill: col.stock, stroke: 1pt + col.stock-edge)
  label(44, 22, [Amazon's, kept as it is], anchor: "west", size: 9.5pt)
  rect(P(210, 14), P(226, 30), radius: 4, fill: col.accent-fill, stroke: 1.4pt + col.accent)
  label(234, 22, [this project], anchor: "west", size: 9.5pt)

  // the Echo
  rect(P(20, 50), P(560, 360), radius: 24, stroke: (paint: col.line, thickness: 1.2pt, dash: "dashed"))
  content(P(44, 76), anchor: "west", text(size: 13pt, weight: 700)[Your Echo])

  node(44, 100, 236, 64, [Microphones, speaker], sub: [the Echo's own hardware])
  node(44, 188, 236, 64, [Audio front end], sub: [echo cancelling, beamforming])
  node(44, 276, 236, 64, [Wake word engine], sub: [Amazon's, or microWakeWord])
  arrow((162, 164), (162, 187))
  arrow((162, 252), (162, 275))

  rect(P(320, 100), P(536, 340), radius: 14, fill: col.accent-fill, stroke: 1.6pt + col.accent)
  content(P(340, 112), anchor: "north-west", block(width: 180 * px, {
    text(size: 15pt, weight: 700, fill: col.accent-text)[hassmic]
    linebreak()
    text(size: 9pt, fill: col.muted)[in place of the Alexa client]
    v(6pt)
    set text(size: 10pt)
    set list(spacing: 7pt, marker: text(fill: col.accent)[•])
    list[voice satellite][media player][Bluetooth audio, proxy][buttons, light ring][settings page][signed updates]
  }))
  arrow((280, 220), (319, 220))
  label(299, 210, [audio], size: 7.5pt)
  arrow((280, 308), (319, 308))
  label(299, 298, [wake], size: 7.5pt)

  // Amazon, behind the firewall
  // a brick wall, staggered rows
  for (i, y) in (46, 62, 78, 94, 110).enumerate() {
    let xs = if calc.odd(i) { ((596, 606), (606, 626), (626, 636)) } else { ((596, 616), (616, 636)) }
    for (a, b) in xs { rect(P(a, y), P(b, y + 16), fill: col.red-fill, stroke: 0.9pt + col.red) }
  }
  label(616, 140, [firewall], c: col.red)
  line(P(560, 86), P(594, 86), stroke: (paint: col.red, thickness: 1.6pt, dash: "dashed"))
  for (x, y, r) in ((700, 92, 24), (734, 72, 30), (776, 78, 27), (808, 96, 20), (752, 102, 22)) {
    circle(P(x, y), radius: r, fill: col.stock, stroke: none)
  }
  content(P(754, 88), anchor: "center", align(center, {
    text(size: 10.5pt, weight: 600, fill: col.muted)[Amazon cloud]
    linebreak()
    text(size: 8.5pt, fill: col.red)[never reached]
  }))
  // no-entry sign between wall and cloud
  circle(P(660, 86), radius: 10, fill: col.card, stroke: 2pt + col.red)
  line(P(653, 79), P(667, 93), stroke: 2pt + col.red)

  // what hassmic talks to
  node(640, 146, 240, 60, [Home Assistant], sub: [Assist, timers, entities], kind: "card")
  node(640, 218, 240, 60, [Music Assistant], sub: [multiroom, Sendspin], kind: "card")
  node(640, 290, 240, 60, [Phones, speakers], sub: [Bluetooth in and out], kind: "card")
  for y in (176, 248, 320) { arrow((538, y), (638, y), c: col.accent, both: true, w: 1.6pt) }
})
