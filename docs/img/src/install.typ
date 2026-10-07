#import "theme.typ": *
#show: fig

// scripts/setup.sh's steps (devices/<codename>/setup.sh STEPS), grouped.
#let steps = ([Tools], [Downloads], [USB access], [USB cable], [Unlock], [Stock firmware], [Root],
  [Build], [Lock down, #linebreak() join Wi-Fi], [Install], [Wake words #linebreak() #text(fill: col.muted)[optional]])
#let phases = (
  (1, 3, "card", [On your PC], [tools, firmware, unlock files]),
  (4, 7, "warn", [Into the Echo], [wipes it · skipped if rooted]),
  (8, 8, "new", [Build], [or GitHub's]),
  (9, 11, "good", [Onto your network], [the Echo names itself]),
)
#let sx(i) = 54 + (i - 1) * 86

#canvas(length: px, {
  import draw: *
  let cy = 100

  for (a, b, kind, title, note) in phases {
    let (fill, edge, tcol) = kinds.at(kind)
    rect(P(sx(a) - 40, 0), P(sx(b) + 40, 62), radius: 10, fill: fill, stroke: 1pt + edge)
    content(P((sx(a) + sx(b)) / 2, 31), anchor: "center", align(center, {
      text(size: 10.5pt, weight: 700, fill: if kind == "card" { col.text } else { tcol }, title)
      linebreak()
      text(size: 8.5pt, fill: col.muted, note)
    }))
  }

  line(P(sx(1), cy), P(sx(11) + 70, cy), stroke: 2pt + col.stock-edge)
  for (i, name) in steps.enumerate(start: 1) {
    let kind = phases.find(p => p.at(0) <= i and i <= p.at(1)).at(2)
    let (fill, edge, tcol) = kinds.at(kind)
    circle(P(sx(i), cy), radius: 17, fill: col.card, stroke: 2pt + edge)
    content(P(sx(i), cy), text(size: 10pt, weight: 700, fill: if kind == "card" { col.text } else { tcol }, str(i)))
    content(P(sx(i), cy + 26), anchor: "north", block(width: 80 * px, align(center, text(size: 8.5pt, name))))
  }

  // the end: Home Assistant finds it
  let ex = sx(11) + 76
  rect(P(ex, cy - 30), P(ex + 190, cy + 30), radius: 12, fill: col.accent-fill, stroke: 1.6pt + col.accent)
  content(P(ex + 95, cy), anchor: "center", align(center, {
    text(size: 10.5pt, weight: 700, fill: col.accent-text)[Add in Home Assistant]
    linebreak()
    text(size: 8.5pt, fill: col.muted)[it is discovered]
  }))
})
