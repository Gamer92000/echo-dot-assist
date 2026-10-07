#import "theme.typ": *
#show: fig

// One tile: an icon on the left, gestures and what they do on the right.
#let tile(x, y, w, h, title, rows, icon) = {
  draw.rect(P(x, y), P(x + w, y + h), radius: 14, fill: col.card, stroke: 1pt + col.stock-edge)
  icon(x + 52, y + h / 2)
  draw.content(P(x + 100, y + h / 2), anchor: "west", block(width: (w - 116) * px, {
    text(size: 12pt, weight: 700, title)
    v(2pt)
    set text(size: 9.5pt)
    grid(columns: (auto, 1fr), column-gutter: 8pt, row-gutter: 5pt,
      ..rows.map(((g, d)) => (text(weight: 600, fill: col.accent-text, g), d)).flatten())
  }))
}

#let knob(c, r: 30) = draw.circle(c, radius: r, fill: col.stock, stroke: 1.2pt + col.stock-edge)

#canvas(length: px, {
  import draw: *
  let (w, h, g) = (440, 160, 20)

  tile(0, 0, w, h, [Action #text(weight: 400, fill: col.muted)[(the dot)]], (
    ([Press], [talk without the wake word, pause music, stop an alarm, cancel a request]),
    ([Press], [approve a login waiting on the settings page or a phone]),
    ([Hold 10 s], [factory reset (the ring warns at 5 s)]),
  ), (x, y) => { knob(P(x, y)); circle(P(x, y), radius: 7, fill: col.text, stroke: none) })

  tile(w + g, 0, w, h, [Volume up, down], (
    ([Press], [10 % per step; the ring shows the level. On a Bluetooth speaker: its volume]),
    ([Both 2 s], [pair two Echos for wake word arbitration, only needed without Home Assistant]),
  ), (x, y) => {
    knob(P(x, y - 20), r: 20); knob(P(x, y + 22), r: 20)
    line(P(x - 8, y - 20), P(x + 8, y - 20), stroke: 2pt + col.text)
    line(P(x, y - 28), P(x, y - 12), stroke: 2pt + col.text)
    line(P(x - 8, y + 22), P(x + 8, y + 22), stroke: 2pt + col.text)
  })

  tile(0, h + g, w, h, [Microphone off], (
    ([Press], [hardware mute, as always: mics cut, ring red]),
    ([], [Home Assistant can mute, but only this button unmutes]),
  ), (x, y) => {
    knob(P(x, y))
    rect(P(x - 6, y - 16), P(x + 6, y + 4), radius: 6, fill: none, stroke: 2pt + col.text)
    arc(P(x, y - 2), start: 200deg, stop: 340deg, radius: 11, anchor: "origin", stroke: 2pt + col.text)
    line(P(x, y + 9), P(x, y + 15), stroke: 2pt + col.text)
    line(P(x - 7, y + 15), P(x + 7, y + 15), stroke: 2pt + col.text)
    line(P(x - 16, y - 16), P(x + 16, y + 16), stroke: 2.5pt + ring.red)
  })

  tile(w + g, h + g, w, h, [Your voice], (
    (["Alexa, …"], [to Home Assistant's Assist; the nearest Echo answers]),
    (["Alexa, stop"], [interrupts a reply]),
  ), (x, y) => {
    circle(P(x, y), radius: 28, fill: none, stroke: 7pt + ring.blue)
    arc(P(x, y), start: 50deg, stop: 130deg, radius: 28, anchor: "origin", stroke: 7pt + ring.cyan)
  })
})
