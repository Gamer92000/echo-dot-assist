#import "theme.typ": *
#show: fig

// docs/WINDOWS.md: what a Windows PC needs once, before scripts/setup.sh (scripts/lib/setup.sh need_tools on
// WINDOWS; devices/biscuit/setup.sh step_tools: adb, python3, fastboot, uvx; firebreak's README for the drivers).

#let tools = (
  ([Git for Windows], [*Git Bash*: the window the setup runs in],
    "card", [PowerShell], "winget install Git.Git"),
  ([Python], [the setup signs your updates with it],
    "card", [python.org], [installer: tick #text(weight: 700)["Add python.exe to PATH"]]),
  ([Android platform-tools], [*adb* and *fastboot*: talk to the Echo over USB],
    "card", [PowerShell], "winget install Google.PlatformTools"),
  ([uv], [downloads and runs *firebreak*, the unlock],
    "card", [PowerShell], "winget install astral-sh.uv"),
  ([Two USB drivers], [let fastboot see the Echo; without them the ring stays blue],
    "warn", [PowerShell #linebreak() as administrator], [the commands in section 2, item 5 (from firebreak's README)]),
)

#canvas(length: px, {
  import draw: *
  let (rh, g, w) = (62, 12, 880)
  for (i, (name, what, kind, where, how)) in tools.enumerate() {
    let y = i * (rh + g)
    let (fill, edge, tcol) = kinds.at(kind)
    rect(P(0, y), P(w, y + rh), radius: 12, fill: col.card, stroke: 1pt + col.stock-edge)
    circle(P(32, y + rh / 2), radius: 15, fill: col.accent-fill, stroke: 1.6pt + col.accent)
    content(P(32, y + rh / 2), text(size: 11pt, weight: 700, fill: col.accent-text, str(i + 1)))
    content(P(60, y + rh / 2), anchor: "west", block(width: 330 * px, {
      text(size: 11.5pt, weight: 700, name)
      linebreak()
      text(size: 9pt, fill: col.muted, what)
    }))
    // where it is typed or clicked
    rect(P(404, y + 12), P(520, y + rh - 12), radius: 8, fill: fill, stroke: 1pt + edge)
    content(P(462, y + rh / 2), anchor: "center", align(center,
      text(size: 8.5pt, weight: 600, fill: if kind == "card" { col.text } else { tcol }, where)))
    // what to type there
    content(P(536, y + rh / 2), anchor: "west", block(width: 330 * px,
      if type(how) == str { text(size: 9.5pt, raw(how)) } else { text(size: 9pt, how) }))
  }
  let y = tools.len() * (rh + g) + 4
  rect(P(0, y), P(w, y + 44), radius: 12, fill: col.accent-fill, stroke: 1.4pt + col.accent)
  content(P(w / 2, y + 22), anchor: "center", text(size: 10pt, fill: col.accent-text)[
    #text(weight: 700)[Then close every PowerShell and Git Bash window and open Git Bash anew:] it only sees new tools at its start.])
})
