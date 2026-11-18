#import "theme.typ": *
#show: fig

// docs/WINDOWS.md: scripts/setup.sh's steps for an Echo Dot 2 under Git Bash (devices/biscuit/setup.sh STEPS), with
// the Echo unlocked by hand already (scripts/setup.sh stops at the start on Windows without it on adb), and what each
// asks of you there.  udev is marked done on Windows; the unlock step only checks (twrp_or_unlocked); the root step is
// boot-root by hand, as the XDA thread says (root_thread).

// kind, title, what happens / what you do
#let steps = (
  ("auto", [Tools on this PC], [checks the tools; says which are missing]),
  ("you", [Downloads], [save the firmware into #raw("Downloads"): it picks it up from there]),
  ("skip", [USB access for the unlock], [skipped on Windows]),
  ("auto", [Micro-USB cable], [on USB already since the unlock; checked]),
  ("auto", [Unlock, boot TWRP], [done by hand before, as the XDA thread says; only checked, TWRP started]),
  ("auto", [Stock firmware into both slots], [wipes the Echo, writes the firmware into both slots]),
  ("you", [Root], [install #raw("boot-root.zip") by hand, as the XDA thread says; then Enter: it checks]),
  ("auto", [Build], [downloads GitHub's build of your copy and checks its signature]),
  ("you", [Lock down, join Wi-Fi], [type your Wi-Fi's name and password]),
  ("you", [Install], [press Enter at "Settings to start with"; the Echo restarts]),
)
#let look = (
  you: ("new", [you]),
  "auto": ("card", [by itself]),
  skip: ("stock", [skipped]),
  danger: ("bad", [you · careful]),
)

#canvas(length: px, {
  import draw: *
  let (rh, w, top) = (46, 880, 40)

  // legend
  let lx = 0
  for k in ("you", "auto", "skip") {
    let (kind, name) = look.at(k)
    let (fill, edge, tcol) = kinds.at(kind)
    rect(P(lx, 6), P(lx + 16, 22), radius: 4, fill: fill, stroke: 1.2pt + edge)
    content(P(lx + 22, 14), anchor: "west", text(size: 9.5pt, name))
    lx += 120
  }

  line(P(24, top + rh / 2), P(24, top + (steps.len() - 0.5) * rh + 30), stroke: 2pt + col.stock-edge)
  for (i, (k, title, what)) in steps.enumerate() {
    let y = top + i * rh
    let (kind, _) = look.at(k)
    let (fill, edge, tcol) = kinds.at(kind)
    let strong = kind == "new" or kind == "bad"
    if strong {
      rect(P(48, y + 4), P(w, y + rh - 4), radius: 10, fill: fill, stroke: 1.2pt + edge)
    }
    circle(P(24, y + rh / 2), radius: 15, fill: if strong { fill } else { col.card }, stroke: 2pt + edge)
    content(P(24, y + rh / 2), text(size: 10pt, weight: 700, fill: if kind == "card" or kind == "stock" { col.muted } else { tcol }, str(i + 1)))
    content(P(62, y + rh / 2), anchor: "west", block(width: 230 * px,
      text(size: 10.5pt, weight: if strong { 700 } else { 600 }, fill: if kind == "stock" { col.muted } else if strong { tcol } else { col.text }, title)))
    content(P(300, y + rh / 2), anchor: "west", block(width: 570 * px,
      text(size: 9.5pt, fill: if strong { col.text } else { col.muted }, what)))
  }

  // the end: Home Assistant finds it
  let y = top + steps.len() * rh + 14
  rect(P(48, y), P(w, y + 52), radius: 12, fill: col.green-fill, stroke: 1.6pt + col.green)
  circle(P(24, y + 26), radius: 15, fill: col.green-fill, stroke: 2pt + col.green)
  content(P(24, y + 26), text(size: 12pt, weight: 700, fill: col.green)[✓])
  content(P(62, y + 26), anchor: "west", block(width: 800 * px, {
    text(size: 10.5pt, weight: 700, fill: col.green)[Add it in Home Assistant]
    h(10pt)
    text(size: 9.5pt)[Settings → Devices & services: it shows up as discovered ("Echo Dot 2 …" and six letters and digits). Add, pick an Assist pipeline, say "Alexa".]
  }))
})
