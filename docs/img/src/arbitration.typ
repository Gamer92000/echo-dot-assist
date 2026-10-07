#import "theme.typ": *
#show: fig

// Wake word arbitration (arb.c): every Echo that heard the word scores it, the best one answers.
#let echo(x, y, on) = {
  draw.circle(P(x, y), radius: 22, fill: col.stock, stroke: 1pt + col.stock-edge)
  if on {
    draw.circle(P(x, y), radius: 22, fill: none, stroke: 6pt + ring.blue)
    draw.arc(P(x, y), start: 120deg, stop: 180deg, radius: 22, anchor: "origin", stroke: (paint: ring.cyan, thickness: 6pt, cap: "butt"))
  } else {
    draw.circle(P(x, y), radius: 22, fill: none, stroke: 6pt + ring.off)
  }
}
// how clearly it stood out of the room's noise, as a meter
#let meter(x, y, level) = for i in range(5) {
  let h = 6 + i * 4
  draw.rect(P(x - 22 + i * 10, y), P(x - 15 + i * 10, y - h), radius: 1.5,
    fill: if i < level { col.accent } else { col.stock-edge }, stroke: none)
}

#canvas(length: px, {
  import draw: *
  let rooms = ((0, [Kitchen], 1, false), (280, [Living room], 5, true), (560, [Hall], 2, false))

  for (x, name, level, on) in rooms {
    rect(P(x, 0), P(x + 260, 190), radius: 14, fill: col.card, stroke: 1pt + col.stock-edge)
    label(x + 18, 22, text(weight: 600, name), anchor: "west", size: 10pt)
    let ex = x + 180
    echo(ex, 100, on)
    meter(ex, 158, level)
    label(ex, 176, if on [answers] else [stays quiet], c: if on { col.accent-text } else { col.muted }, size: 9pt)
    line(P(ex, 190), P(ex, 236), stroke: (paint: col.line, thickness: 1.2pt, dash: "dashed"))
  }

  // the speaker, in the living room
  let (hx, hy) = (340, 96)
  circle(P(hx, hy - 22), radius: 11, fill: col.muted, stroke: none)
  rect(P(hx - 16, hy - 6), P(hx + 16, hy + 40), radius: (north: 14, south: 3), fill: col.muted, stroke: none)
  rect(P(hx + 14, hy - 66), P(hx + 104, hy - 40), radius: 8, fill: col.accent-fill, stroke: 1pt + col.accent)
  line(P(hx + 20, hy - 40), P(hx + 12, hy - 30), P(hx + 30, hy - 40), stroke: 1pt + col.accent, fill: col.accent-fill)
  label(hx + 59, hy - 53, [Alexa, …], c: col.accent-text, size: 9.5pt)

  line(P(20, 236), P(800, 236), stroke: 2pt + col.line)
  label(410, 254, [your network: the Echos compare in 0.2 s; the one that heard it clearest answers], size: 9pt)
})
