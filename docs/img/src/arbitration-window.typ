#import "theme.typ": *
#show: fig

// How long an Echo waits for the others' claims (arb.c): the window it learns from how late they came, a round that
// ends as soon as every claim is in, one that waits the window out, and a claim later than the window.
// One time axis for all rows: ms after this Echo heard the wake word.
#let X(ms) = 205 + ms * 1.0
#let rowlabel(y, title, sub) = draw.content(P(0, y), anchor: "west", block(width: 165 * px, {
  text(weight: 600, size: 10pt, title); linebreak(); text(size: 8.5pt, fill: col.muted, sub)
}))
#let dot(ms, y, c, r: 5) = draw.circle(P(X(ms), y), radius: r, fill: c, stroke: none)
#let decide(ms, y, c) = draw.line(P(X(ms), y - 15), P(X(ms), y + 15), stroke: (paint: c, thickness: 3pt, cap: "round"))
#let track(y) = draw.line(P(X(0), y), P(X(600), y), stroke: 1pt + col.stock-edge)

#canvas(length: px, {
  import draw: *

  // 1: the window, learned.  How late each claim of the other Echos came (0: before this Echo's own); 95 % of these
  // 38 came within 356 ms, plus 50 ms makes 406 (fake_ha_arbitration's numbers)
  let y1 = 52
  rowlabel(y1, [Learning the window], [how late the other Echos' claims came, the last 64])
  let late = (0, 0, 5, 12, 18, 25, 30, 34, 41, 47, 55, 62, 70, 85, 96, 104, 118, 126, 140, 152, 163, 171, 185, 197, 210,
              224, 233, 241, 256, 268, 279, 290, 303, 311, 322, 338, 356, 372)
  track(y1)
  for (i, ms) in late.enumerate() { dot(ms, y1 - 9 + calc.rem(i, 3) * 9, col.muted, r: 3.5) }
  rect(P(X(356), y1 - 22), P(X(406), y1 + 22), fill: col.accent-fill, stroke: none)
  line(P(X(356), y1 - 26), P(X(356), y1 + 22), stroke: (paint: col.line, thickness: 1.2pt, dash: "dashed"))
  line(P(X(406), y1 - 26), P(X(406), y1 + 22), stroke: 2pt + col.accent)
  label(X(356) - 5, y1 - 32, [95 % within 356 ms], anchor: "east")
  label(X(406) + 5, y1 - 32, [\+ 50 ms = the window, 406 ms], anchor: "west", c: col.accent-text)
  label(X(381), y1 + 32, [+50], c: col.accent-text, size: 8pt)

  // the learned window, carried down to the rounds that use it
  line(P(X(406), y1 + 22), P(X(406), 350), stroke: (paint: col.accent, thickness: 1pt, dash: "dashed"))

  // 2: everyone heard it: decided when the last claim is in, the rest of the window not needed
  let y2 = 140
  rowlabel(y2, [Every Echo heard you], [decides as soon as all claims are in])
  line(P(X(406), y2), P(X(600), y2), stroke: 1pt + col.stock-edge)      // not through the label in the window's rest
  rect(P(X(0), y2 - 11), P(X(95), y2 + 11), fill: col.accent-fill, stroke: 1pt + col.accent)
  rect(P(X(95), y2 - 11), P(X(406), y2 + 11), fill: none, stroke: (paint: col.stock-edge, thickness: 1pt, dash: "dashed"))
  label(X(250), y2, [rest of the window not needed], size: 8.5pt)
  dot(0, y2, col.accent)
  dot(30, y2, col.text); label(X(30), y2 - 27, [Living room])
  dot(95, y2, col.text); label(X(95), y2 - 27, [Hall])
  decide(95, y2, col.green)
  label(X(0), y2 + 26, [heard it])
  label(X(95) + 8, y2 + 26, [decides: every claim in], anchor: "west", c: col.green)

  // 3: one did not hear it: the window waited out
  let y3 = 222
  rowlabel(y3, [One Echo did not], [the hall Echo missed it: waits the window out])
  track(y3)
  rect(P(X(0), y3 - 11), P(X(406), y3 + 11), fill: col.accent-fill, stroke: 1pt + col.accent)
  dot(0, y3, col.accent)
  dot(120, y3, col.text); label(X(120), y3 - 27, [Living room])
  decide(406, y3, col.green)
  label(X(0), y3 + 26, [heard it])
  label(X(406) + 8, y3 + 26, [decides: window over], anchor: "west", c: col.green)

  // 4: a claim later than the window: missed this round, still sampled (until 1.5 s), so the window learns from it
  let y4 = 310
  rowlabel(y4, [A claim comes later], [missed this round, still measured for the window])
  track(y4)
  rect(P(X(0), y4 - 11), P(X(406), y4 + 11), fill: col.accent-fill, stroke: 1pt + col.accent)
  dot(0, y4, col.accent)
  dot(440, y4, col.amber); label(X(440), y4 - 27, [Living room, +440 ms: too late], c: col.amber)
  decide(406, y4, col.amber)
  label(X(0), y4 + 26, [heard it])
  label(X(406) - 8, y4 + 26, [decides without it], anchor: "east", c: col.amber)
  label(X(440) + 8, y4 + 26, [still measured], anchor: "west", c: col.amber)

  // the shared time axis
  let ya = 360
  line(P(X(0), ya), P(X(600), ya), stroke: 1.2pt + col.line)
  for ms in range(0, 601, step: 100) {
    line(P(X(ms), ya), P(X(ms), ya + 5), stroke: 1.2pt + col.line)
    label(X(ms), ya + 15, if ms == 0 [0] else [#ms ms], size: 8.5pt)
  }
  label(X(300), ya + 34, [time after this Echo heard the wake word, each dot a claim from another Echo], size: 9pt)
})
