#import "theme.typ": *
#show: fig

// Every update, pushed or online (ota.c, update.c, root's main.sh ota_watch, main.c's self test).
#canvas(length: px, {
  import draw: *

  node(0, 0, 220, 64, [From your PC], sub: [`scripts/ota-push.sh`, signed with your key], kind: "card", size: 10.5pt)
  node(0, 96, 220, 64, [From Home Assistant], sub: [update entity, GitHub release, release key], kind: "card", size: 10.5pt)

  node(270, 46, 170, 68, [Signature checked], sub: [by root, before unpacking], kind: "new", size: 10.5pt)
  node(490, 46, 170, 68, [New version starts], sub: [old one kept aside], kind: "new", size: 10.5pt)
  node(710, 34, 190, 92, [Self test], sub: [within 30 s: wake word engine loaded, ports open, 1 s of mic audio], kind: "new", size: 10.5pt)

  node(1000, 0, 200, 64, [Kept for good], sub: [the new copy to fall back to], kind: "good", size: 10.5pt)
  node(1000, 96, 200, 64, [Falls back], sub: [to the last copy that worked], kind: "bad", size: 10.5pt)
  node(270, 156, 170, 50, [Refused], sub: [nothing installed], kind: "bad", size: 10.5pt)

  arrow((222, 32), (268, 70))
  arrow((222, 128), (268, 90))
  arrow((442, 80), (488, 80))
  arrow((662, 80), (708, 80))
  arrow((902, 66), (998, 32), c: col.green)
  arrow((902, 94), (998, 128), c: col.red)
  arrow((355, 116), (355, 154), c: col.red)
  label(944, 34, [passes], c: col.green, size: 8.5pt)
  label(944, 126, [fails], c: col.red, size: 8.5pt)
  label(362, 135, [bad signature], c: col.red, size: 8.5pt, anchor: "west")
})
