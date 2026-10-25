#!/usr/bin/env python3
"""Which members a wake word round waits for (arb.c "Rivals"), compared on a synthetic house.  Scores in dB (arb.c's /100).
tools/arbsim.py [seeds]: per estimator, averaged over the seeds: the time to the answer, how often the best Echo did
not answer, how often two did.  Not a measurement: the house, the noise and the doors are guesses; for the order of
the estimators and for tuning arb.c's constants, which should be redone once real claims (both scores of one wake
word) are in.
House: rooms on a line, one Echo each (two in the big living room); a talker stands at a spot of a room (hotspots),
the score at each Echo = level - 20 log10(dist) - walls between (each door open 6 dB, shut 20 dB) + N(0, sd).
Detection: logistic around 12 dB.  Doors toggle at random.  From each Echo's view, for every wake word it detects:
which members it waits for, its due (window 400 ms if a waited member did not claim, else the latest waited claim's
lateness), each Echo deciding at its due or when a better claim or another's "answers" reaches it (each 50-350 ms
on the way): the time to the first answer, whether the best Echo answered, whether two did."""
import math, random, sys, statistics as st

R = random.Random(1)
ROOMS = [(0, 5), (5, 12), (12, 16), (16, 22), (22, 26)]          # x ranges, m; 12-16 a hall
ECHOS = [(2, 1), (7, 1), (10.5, 2.5), (14, 1), (19, 2), (24, 1)]   # (x, y): 6 Echos, two in room 1
ROOM_OF = lambda x: next(i for i, (a, b) in enumerate(ROOMS) if a <= x < b)
SPOTS = [(R.uniform(a + .5, b - .5), R.uniform(0, 4)) for a, b in ROOMS for _ in range(2)]
WINDOW, MARGIN, SD = 400, 3.0, 2.0

def scores(t, doors):
    out = []
    for ex, ey in ECHOS:
        d = max(0.5, math.hypot(t[0] - ex, t[1] - ey))
        a, b = sorted((ROOM_OF(t[0]), ROOM_OF(ex)))
        walls = sum(6 if doors[w] else 20 for w in range(a, b))
        out.append(t[2] - 20 * math.log10(d) - walls + R.gauss(0, SD))
    return out

def detected(s): return R.random() < 1 / (1 + math.exp(-(s - 12) / 1.5))

class Always:
    def waits(self, m, s): return True
    def take(self, m, s, d): pass

class Mask:                                         # first take: no claim in last 4 of 16, < 1/8
    def __init__(self): self.h = {}
    def waits(self, m, s):
        h = self.h.get(m, [])
        return len(h) < 6 or any(h[-4:]) or sum(h) * 8 >= len(h)
    def take(self, m, s, d): self.h.setdefault(m, []).append(d is not None); self.h[m] = self.h[m][-16:]

class KNN:                                          # what arb.c does now
    def __init__(self, keep=32, k=8): self.o, self.keep, self.k = {}, keep, k
    def waits(self, m, s):
        o = self.o.get(m, [])
        if len(o) < 6: return True
        near = sorted(range(len(o)), key=lambda i: (abs(o[i][0] - s), -i))[:self.k]
        return any(o[i][1] is not None and o[i][1] > -MARGIN for i in near)
    def take(self, m, s, d): self.o.setdefault(m, []).append((s, d)); self.o[m] = self.o[m][-self.keep:]

class Kernel:                                       # arb.c's: kernel in score x decay in time, prior toward waiting
    def __init__(self, h=4.0, lam=0.99, p=0.15, prior=(0.5, 2), boost=3): self.o, self.h, self.lam, self.p, self.prior, self.boost = {}, h, lam, p, prior, boost
    def waits(self, m, s):
        num, den = self.prior
        for age, (os_, d) in enumerate(reversed(self.o.get(m, []))):
            w = math.exp(-abs(os_ - s) / self.h) * self.lam ** age
            if d is not None and d > -MARGIN: num += w * self.boost; den += w * self.boost
            else: den += w
        return num / den > self.p
    def take(self, m, s, d): self.o.setdefault(m, []).append((s, d)); self.o[m] = self.o[m][-64:]

class EWMA:                                         # one running mean/var of their - ours (no claim: ours - 15)
    def __init__(self, a=0.1): self.a, self.st = a, {}
    def waits(self, m, s):
        n, mu, var = self.st.get(m, (0, 0, 100))
        return n < 6 or mu + 2 * math.sqrt(var) > -MARGIN
    def take(self, m, s, d):
        x = d if d is not None else -15
        n, mu, var = self.st.get(m, (0, 0, 100))
        mu2 = mu + self.a * (x - mu); var = (1 - self.a) * (var + self.a * (x - mu) ** 2)
        self.st[m] = (n + 1, mu2, var)

class RLS:                                          # theirs ~ a + b * ours, forgetting 0.97, no claim: censored at 12 dB
    def __init__(self, lam=0.97): self.lam, self.st = lam, {}
    def waits(self, m, s):
        st_ = self.st.get(m)
        if not st_ or st_[3] < 6: return True
        th, P, r2, n = st_
        pred = th[0] + th[1] * s
        return pred + 2 * math.sqrt(r2) > s - MARGIN
    def take(self, m, s, d):
        y = s + d if d is not None else min(s, 12) - 3
        th, P, r2, n = self.st.get(m, ([0, 1], [[100, 0], [0, 100]], 25, 0))
        x = [1, s]; Px = [P[0][0] * x[0] + P[0][1] * x[1], P[1][0] * x[0] + P[1][1] * x[1]]
        den = self.lam + x[0] * Px[0] + x[1] * Px[1]; k = [Px[0] / den, Px[1] / den]
        e = y - (th[0] + th[1] * s); th = [th[0] + k[0] * e, th[1] + k[1] * e]
        P = [[(P[i][j] - k[i] * Px[j]) / self.lam for j in range(2)] for i in range(2)]
        r2 = self.lam * r2 + (1 - self.lam) * e * e
        self.st[m] = (th, P, r2, n + 1)

def run(make, events=6000):
    """mean time to the answer (ms), % wake words the best Echo did not answer, % two or more answered"""
    doors = [True] * (len(ROOMS) - 1)
    est = [make() for _ in ECHOS]
    lat_, wrong, double, n = [], 0, 0, 0
    for ev in range(events):
        if R.random() < 0.01: w = R.randrange(len(doors)); doors[w] = not doors[w]
        sp = R.choice(SPOTS); t = (min(25.9, max(0.1, sp[0] + R.gauss(0, .5))), sp[1] + R.gauss(0, .5), R.gauss(70, 4))
        sc = scores(t, doors); det = [detected(x) for x in sc]
        heard = [i for i in range(len(ECHOS)) if det[i]]
        if not heard: continue
        n += 1
        claim = {(a, b): R.uniform(50, 350) for a in heard for b in range(len(ECHOS)) if a != b}     # a's claim reaches b
        ans = {(a, b): R.uniform(50, 350) for a in heard for b in range(len(ECHOS)) if a != b}       # a's "answers"
        due = {}
        for e in heard:
            ORACLE["beat"] = lambda m: det[m] and sc[m] > sc[e]
            wt = [m for m in range(len(ECHOS)) if m != e and est[e].waits(m, sc[e])]
            due[e] = 30 if not wt else WINDOW if any(m not in heard for m in wt) else max(30, max(claim[(m, e)] for m in wt))
        won, when, left = [], {}, set(heard)
        while left:                                 # the next Echo to decide: at its due, or when it is beaten
            best = None
            for e in left:
                beats = [claim[(m, e)] for m in heard if m != e and sc[m] > sc[e]] + [when[m] + ans[(m, e)] for m in won]
                at = min([due[e]] + [b for b in beats if b < due[e]])
                lost = any(b <= at for b in beats)
                if best is None or at < best[1]: best = (e, at, lost)
            e, at, lost = best; left.discard(e)
            if not lost: won.append(e); when[e] = at
        lat_.append(min(when.values()))
        wrong += max(heard, key=lambda i: sc[i]) not in won
        double += len(won) > 1
        for e in heard:                             # what each kept: a claim against its own; both answered = close
            for m in range(len(ECHOS)):
                if m == e: continue
                d = sc[m] - sc[e] if m in heard else None
                if d is not None and e in won and m in won: d = max(d, 0)
                est[e].take(m, sc[e], d)
    return st.mean(lat_), 100 * wrong / n, 100 * double / n


class Never:
    def waits(self, m, s): return False
    def take(self, m, s, d): pass

ORACLE = {}
class Oracle:                                       # knows the round: waits only for members that claim and beat it
    def waits(self, m, s): return ORACLE["beat"](m)
    def take(self, m, s, d): pass

ROWS = [("always wait (before)", Always), ("never wait", Never), ("oracle", Oracle), ("no claim in 4 of 16", Mask),
        ("mean difference", EWMA), ("line, censored", RLS), ("nearest 8 of 32", KNN), ("nearest 16 of 96", lambda: KNN(96, 16)),
        ("kernel, close x1, p 0.12", lambda: Kernel(p=0.12, boost=1)), ("kernel (arb.c)", Kernel),
        ("kernel, p 0.2", lambda: Kernel(p=0.2))]
if __name__ == "__main__":
    seeds = [int(x) for x in sys.argv[1:]] or [7, 8, 9]
    for name, mk in ROWS:
        a = []
        for seed in seeds: R.seed(seed); a.append(run(mk))
        print(f"{name:26s} answer after {sum(x[0] for x in a) / len(a):4.0f} ms  not the best Echo {sum(x[1] for x in a) / len(a):5.2f} %"
              f"  two answered {sum(x[2] for x in a) / len(a):5.2f} %")
