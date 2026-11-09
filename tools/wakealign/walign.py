"""The wake word aligner in numpy (src/hassmic/wakealign.h): the reference wakealign.c follows, and the writer of its
.wa files.  On 220 OmniVoice requests (2026-10-29) it gives MFA's word end in 71 of 77 of one set, within 40 ms in
all; wakealign.c gives its end on all 220, to the sample.

  walign.py write <word.npz> <out.wa> <shared.was>   (extract.py's npz), compact()ed; its GMMs into shared.was
  walign.py end <word.npz> <raw>                     the word's end in a 16 kHz raw file (the window of wakealign_end)"""
import json, os, struct, sys
import numpy as np

R = 16000


class Model:
    def __init__(self, path):
        z = np.load(path, allow_pickle=False)
        self.word = str(z["word"]); self.chain = [str(c) for c in z["chain"]]
        self.hmms = json.loads(str(z["hmms"])); self.pdfs = [int(p) for p in z["pdfs"]]; self.ngauss = z["ngauss"]
        self.gconsts, self.mi, self.iv = z["gconsts"], z["means_invvars"], z["inv_vars"]
        self.lda = z["lda"]; self.feat = json.loads(str(z["features"]))
        self.variants = json.loads(str(z["variants"])); self.bracketed_probs = [float(v) for v in z["bracketed_probs"]]
        self.off = np.concatenate([[0], np.cumsum(self.ngauss)]).astype(int)
        self.pdf_index = {p: i for i, p in enumerate(self.pdfs)}


# ------------------------------------------------------------------ Kaldi's MFCC (feature-mfcc.cc, feature-window.cc, mel-computations.cc)
def mel(f): return 1127.0 * np.log(1.0 + f / 700.0)


def mel_banks(nbins, low, high, nfft=512):
    nb = nfft // 2; ml, mh = mel(low), mel(high); d = (mh - ml) / (nbins + 1)
    banks = np.zeros((nbins, nb + 1))
    for b in range(nbins):
        left, center, right = ml + b * d, ml + (b + 1) * d, ml + (b + 2) * d
        for i in range(nb):
            m = mel(R / nfft * i)
            if left < m < right: banks[b, i] = (m - left) / (center - left) if m <= center else (right - m) / (right - center)
    return banks


def dct_matrix(ncep, nbins):
    M = np.zeros((ncep, nbins)); M[0] = np.sqrt(1.0 / nbins)
    for k in range(1, ncep): M[k] = np.sqrt(2.0 / nbins) * np.cos(np.pi / nbins * (np.arange(nbins) + 0.5) * k)
    return M


def mfcc(x, f):
    """x: samples (int16 scale), frames centred every shift with snip_edges false (the signal reflected at its ends)"""
    shift, length = f["frame_shift"] * R // 1000, f["frame_length"] * R // 1000
    n = (len(x) + shift // 2) // shift; N = len(x)
    idx = np.arange(n)[:, None] * shift + shift // 2 - length // 2 + np.arange(length)[None, :]
    idx = np.where(idx < 0, -idx - 1, idx); idx = np.where(idx >= N, 2 * N - 1 - idx, idx)
    fr = x[idx].astype(np.float64); fr = fr - fr.mean(axis=1, keepdims=True)
    pe = f["preemphasis_coefficient"]
    fr = np.concatenate([fr[:, :1] - pe * fr[:, :1], fr[:, 1:] - pe * fr[:, :-1]], axis=1)
    win = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(length) / (length - 1))) ** 0.85
    spec = np.abs(np.fft.rfft(fr * win, 512)) ** 2
    fb = np.log(np.maximum(spec @ mel_banks(f["num_mel_bins"], f["low_frequency"], f["high_frequency"]).T, np.finfo(np.float32).eps))
    c = fb @ dct_matrix(f["num_coefficients"], f["num_mel_bins"]).T
    L = f["cepstral_lifter"]
    return c * (1 + 0.5 * L * np.sin(np.pi * np.arange(f["num_coefficients"]) / L))


def features(m, x):
    """MFCC, the mean over the window taken out, 7 frames spliced (edges repeated), the model's LDA"""
    c = mfcc(x, m.feat); c = c - c.mean(axis=0); n = len(c)
    pad = np.concatenate([np.repeat(c[:1], 3, 0), c, np.repeat(c[-1:], 3, 0)])
    return np.concatenate([pad[i:i + n] for i in range(7)], axis=1) @ m.lda.T


def loglikes(m, feats):
    g = m.gconsts[None, :] + feats @ m.mi.T - 0.5 * (feats ** 2) @ m.iv.T
    out = np.empty((len(feats), len(m.pdfs)))
    for i in range(len(m.pdfs)):
        s = g[:, m.off[i]:m.off[i + 1]]; mx = s.max(axis=1); out[:, i] = mx + np.log(np.exp(s - mx[:, None]).sum(axis=1))
    return out


# ------------------------------------------------------------------ the graph (MFA's lexicon with silence probabilities) and Viterbi
def build(m, ascale=1 / 12, sls=0.1, init_sil=0.21, fsc=0.99, fnsc=0.05):
    """[sil] word ([sil] spn | spn) [sil], one path per pronunciation, with the dictionary's silence and pronunciation
    probabilities, MFA's align options (acoustic scale 1/12, self loops 0.1) and Kaldi's way with them (AddSelfLoops,
    GetTransitionLogProbIgnoringSelfLoops).  Returns states [(pdf index, [(next state or -1 = may end there, log
    weight)])], entries, the states of the word's last phones, the acoustic scale"""
    L = np.log; names = m.chain; idx = {n: i for i, n in enumerate(names)}
    _, bs, bfs, bfn = m.bracketed_probs
    succ = {"sil_start": [], "sil_mid": [("spn", 0.0)], "spn": [("sil_end", L(bs) + L(fsc)), (None, L(1 - bs) + L(fnsc))],
            "sil_end": [(None, 0.0)]}
    entry_nodes = [("sil_start", L(init_sil))]; lastn = []
    for k, v in enumerate(m.variants):
        pp, ws, wfs, wfn = v["probs"]; pre = f"{k}:"
        mid = sorted((n for n in names if n.startswith(pre) and n[len(pre):][0] == "w" and n[len(pre) + 1:].isdigit() and n != pre + "w0"),
                     key=lambda n: int(n[len(pre) + 1:]))
        last = [(pre + "wl_sil", 0.0), (pre + "wl_spn", 0.0)]; lastn += [pre + "wl_sil", pre + "wl_spn"]
        succ["sil_start"].append((pre + "w0_sil", L(wfs) + L(pp)))
        succ[pre + "w0_sil"] = succ[pre + "w0_start"] = [(mid[0], 0.0)] if mid else last
        for i, n in enumerate(mid): succ[n] = [(mid[i + 1], 0.0)] if i + 1 < len(mid) else last
        succ[pre + "wl_sil"] = [("sil_mid", L(ws) + L(bfs))]; succ[pre + "wl_spn"] = [("spn", L(1 - ws) + L(bfn))]
        entry_nodes.append((pre + "w0_start", L(1 - init_sil) + L(wfn) + L(pp)))
    used = [n for n in names if n in succ]                  # nodes of other graphs (tools' experiments) left out
    if used[0] != "sil_start" or len(m.hmms[idx["sil_start"]]) != 5:
        raise ValueError("the graph must open on sil_start's 5 states: wakealign.c takes silence's pdfs from them")
    base, states = {}, []
    for n in used: base[n] = len(states); states += [None] * len(m.hmms[idx[n]])
    for n in used:
        for s, (pdf, arcs) in enumerate(m.hmms[idx[n]]):
            self_lp = next((lp for d, lp in arcs if d == s), None)
            ns = np.log1p(-np.exp(self_lp)) if self_lp is not None else 0.0
            nx = []
            for d, lp in arcs:
                w = sls * lp if d == s else lp - ns + sls * ns
                if d >= 0: nx.append((base[n] + d, w))
                else: nx += [(base[t] if t else -1, w + ww) for t, ww in succ[n]]
            states[base[n] + s] = (m.pdf_index[pdf], nx)
    entry = [(base[n], w) for n, w in entry_nodes]
    lastst = [s for n in lastn for s in range(base[n], base[n] + len(m.hmms[idx[n]]))]
    return states, entry, lastst, ascale


def viterbi(m, ll, graph):
    """the best path through graph for the frames' log likelihoods: a state per frame, [] if none ends"""
    states, entry, lastst, ascale = graph; S, T = len(states), len(ll)
    W = np.full((S, S), -np.inf); final = np.full(S, -np.inf); pdf = np.array([p for p, _ in states])
    for s, (_, nx) in enumerate(states):
        for d, w in nx:
            if d >= 0: W[s, d] = max(W[s, d], w)
            else: final[s] = max(final[s], w)
    delta = np.full(S, -np.inf)
    for s, w in entry: delta[s] = max(delta[s], w + ascale * ll[0, pdf[s]])
    back = np.zeros((T, S), dtype=np.int32)
    for t in range(1, T):
        cand = delta[:, None] + W; back[t] = np.argmax(cand, axis=0)
        delta = cand[back[t], np.arange(S)] + ascale * ll[t, pdf]
    s = int(np.argmax(delta + final))
    if not np.isfinite(delta[s] + final[s]): return []
    path = [s]
    for t in range(T - 1, 0, -1): s = int(back[t, s]); path.append(s)
    return path[::-1]


def transformed(feats, xform):
    """W [x; 1] per frame (fMLLR), or the features as they are"""
    return feats if xform is None else np.hstack([feats, np.ones((len(feats), 1))]) @ xform.T


def word_end(m, x, graph=None, xform=None, adapt=None):
    """the sample (in x) where the word ends, or -1.  xform: the household's fMLLR transform (D x D+1) or None;
    adapt: an Adapt whose statistics get this window's (on the features as they are, the posteriors on xform's)"""
    graph = graph or build(m); feats = features(m, x); fx = transformed(feats, xform)
    path = viterbi(m, loglikes(m, fx), graph)
    if adapt is not None and path: adapt.add(m, feats, fx, path, graph)
    lastst = set(graph[2]); shift = m.feat["frame_shift"] * R // 1000
    tw = next((t for t in range(len(path) - 1, -1, -1) if path[t] in lastst), -1)
    if tw < 0: return -1
    # a pause after the word (5 frames of silence's states and more): the replay from its middle (wakealign.c)
    states = graph[0]; silp = {states[s][0] for s in range(5)}; t = tw + 1
    while t < len(path) and states[path[t]][0] in silp: t += 1
    if t - tw - 1 >= 5: return min(len(x), (tw + 1 + (t - tw - 1) // 2) * shift)
    return min(len(x), pause_after(x, (tw + 1) * shift, shift))


def pause_after(x, at, shift, floor_db=6.0, frames=8, look=20, into=30):
    """wakealign.c pause_after: a pause at the room's floor (10th percentile of the window's 10 ms levels) within look
    frames after at: the replay from its middle, at most into frames into it"""
    nf = len(x) // shift; k = (at + shift - 1) // shift
    if nf < 10 or k >= nf: return at
    f = x[:nf * shift].astype(np.float64).reshape(nf, shift)
    db = np.array([10 * np.log10(sum(float(v) * float(v) for v in row) / shift + 1) for row in f])
    s = np.sort(db); pos = (nf - 1) * 0.1; i = int(pos); fl = s[i] + (pos - i) * (s[min(i + 1, nf - 1)] - s[i])
    t = k
    while t < nf and t < k + look:
        if db[t] >= fl + floor_db: t += 1; continue
        u = t
        while u < nf and db[u] < fl + floor_db: u += 1
        if u - t >= frames: return (t + min((u - t) // 2, into)) * shift
        t = u
    return at


class Adapt:
    """fMLLR of one household in one model's feature space (Gales 1998; Kaldi's fmllr-diag-gmm.cc): K, G_i, beta from
    the Gaussian posteriors along each request's path, older requests counting less (FORGET a request), the transform
    re-estimated from them once MIN requests are in.  On the 220 German requests, each estimated from the other
    requests of the same voice: 158 -> 166 (5 of them), 169 (20)"""
    FORGET, MIN, ITERS, POST_MIN = 0.98, 5, 10, 1e-3

    def __init__(self, dim=40):
        self.D = dim; self.k = np.zeros((dim, dim + 1)); self.g = np.zeros((dim, dim + 1, dim + 1)); self.beta = 0.0
        self.requests = 0; self.xform = None

    def add(self, m, feats, fx, path, graph):
        self.k *= self.FORGET; self.g *= self.FORGET; self.beta *= self.FORGET
        states = graph[0]
        for t, s in enumerate(path):
            p = states[s][0]; a, b = m.off[p], m.off[p + 1]; x = fx[t]
            ll = m.gconsts[a:b] + m.mi[a:b] @ x - 0.5 * m.iv[a:b] @ (x * x); post = np.exp(ll - ll.max()); post /= post.sum()
            xi = np.append(feats[t], 1.0); xx = np.outer(xi, xi)
            for gi in np.nonzero(post > self.POST_MIN)[0]:
                g = post[gi]
                self.k += g * m.mi[a + gi][:, None] * xi[None, :]
                self.g += g * m.iv[a + gi][:, None, None] * xx[None, :, :]
                self.beta += g
        self.requests += 1

    def estimate(self):
        """the transform from the statistics (row by row, ITERS rounds from the last one), None below MIN requests"""
        if self.requests < self.MIN: self.xform = None; return None
        D = self.D; W = np.hstack([np.eye(D), np.zeros((D, 1))]) if self.xform is None else self.xform.astype(np.float64).copy()
        ginv = [np.linalg.inv(self.g[i] + 1e-6 * np.eye(D + 1)) for i in range(D)]
        for _ in range(self.ITERS):
            for i in range(D):
                c = np.append(np.linalg.inv(W[:, :D]).T[i], 0.0)      # the cofactor row, up to det(A), which cancels
                e1 = c @ ginv[i] @ c; e2 = c @ ginv[i] @ self.k[i]
                alpha = (-e2 + np.sqrt(e2 * e2 + 4 * e1 * self.beta)) / (2 * e1)
                W[i] = (alpha * c + self.k[i]) @ ginv[i]
        self.xform = W.astype(np.float32).astype(np.float64)
        return self.xform


# ------------------------------------------------------------------ the .wa file wakealign.c reads
def compact(m, nmax=16):
    """the model as written: at most nmax Gaussians per pdf (the heaviest, renormalised), means x precisions and
    precisions in float16.  On the 220 German requests (wakealign.h): all of them in float32 158, float16 158, at
    most 24 159, 16 158, 12 153, 8 152; 16 in float16 is a fifth of the size (~80 kB a word) and of the work"""
    import copy
    c = copy.copy(m); D = m.mi.shape[1]
    # gconst = log w - (D log 2 pi - sum log iv) / 2 - sum mu^2 iv / 2, mu = mi / iv
    logw = m.gconsts + 0.5 * (D * np.log(2 * np.pi) - np.log(m.iv).sum(axis=1)) + 0.5 * ((m.mi ** 2) / m.iv).sum(axis=1)
    gc, mi, iv, ng = [], [], [], []
    for i in range(len(m.pdfs)):
        a, b = m.off[i], m.off[i + 1]; keep = np.argsort(-logw[a:b], kind="stable")[:nmax] + a
        lw = logw[keep]; norm = lw.max() + np.log(np.exp(lw - lw.max()).sum())
        gc.append(m.gconsts[keep] - norm); mi.append(m.mi[keep]); iv.append(m.iv[keep]); ng.append(len(keep))
    c.gconsts = np.concatenate(gc).astype("<f4").astype(np.float64)
    c.mi = np.concatenate(mi).astype("<f2").astype(np.float64); c.iv = np.concatenate(iv).astype("<f2").astype(np.float64)
    c.ngauss = np.array(ng); c.off = np.concatenate([[0], np.cumsum(c.ngauss)]).astype(int)
    return c


def fnv32(b):
    """FNV-1a, 32 bits: a word file's check of each GMM it takes from the shared file"""
    h = 2166136261
    for c in b: h = ((h ^ c) * 16777619) & 0xffffffff
    return h


def read_shared(path):
    """a .was file: (header, LDA, dim, [GMM blocks]); header and LDA as written, for comparing"""
    d = open(path, "rb").read()
    if d[:4] != b"HMWS" or struct.unpack_from("<I", d, 4)[0] != 1: raise ValueError(f"{path}: not a shared aligner file")
    head = d[8:48]; rows, cols = struct.unpack_from("<2I", d, 48); e = 56 + rows * cols * 4; lda = d[48:e]
    n, dim = struct.unpack_from("<2I", d, e); idx = struct.unpack_from(f"<{2 * n}I", d, e + 8)
    blocks = [d[idx[2 * k + 1]:idx[2 * k + 1] + idx[2 * k] * (4 + 4 * dim)] for k in range(n)]
    return head, lda, dim, blocks


def write_shared(path, head, lda, dim, blocks):
    """little-endian: "HMWS" 1; the feature settings; the LDA; the number of GMMs and their dimension; per GMM its
    Gaussians and where it starts; the GMMs (per Gaussian gconst in float32, then means x precisions and precisions in
    float16).  Written whole and renamed into place"""
    top = 48 + len(lda) + 8 + 8 * len(blocks); idx = []; at = top
    for b in blocks: idx += [len(b) // (4 + 4 * dim), at]; at += len(b)
    out = b"HMWS" + struct.pack("<I", 1) + head + lda + struct.pack("<2I", len(blocks), dim) + struct.pack(f"<{len(idx)}I", *idx) + b"".join(blocks)
    open(path + ".tmp", "wb").write(out); os.replace(path + ".tmp", path)


def write_word(path, shared, head, lda, dim, blocks, tail):
    """a word's .wa beside shared: its GMMs (blocks) go into shared (made, or added to: what other words of the same
    acoustic model have there keeps its place; the same GMM is kept once), the word file names them by their place
    and check.  tail: the graph as write_wa lays it out.  Returns the word file's size"""
    if os.path.exists(shared):
        h, l, d, have = read_shared(shared)
        if (h, l, d) != (head, lda, dim): raise ValueError(f"{shared}: another acoustic model's (features or LDA differ)")
    else: have = []
    at = {b: k for k, b in enumerate(have)}; refs = []
    for b in blocks:
        if b not in at: at[b] = len(have); have.append(b)
        refs += [at[b], fnv32(b)]
    write_shared(shared, head, lda, dim, have)
    name = os.path.basename(shared).encode()
    out = b"HMWA" + struct.pack("<2I", 3, len(name)) + name + struct.pack("<I", len(blocks)) + struct.pack(f"<{len(refs)}I", *refs) + tail
    open(path, "wb").write(out)
    return len(out)


def write_wa(m, path, shared):
    """little-endian: "HMWA" 3; the shared file's name (next to it); the GMMs the word takes from it (place, check);
    the graph's states (with their GMM as an index into that list) and arcs, entries, finals, the states of the word's
    last phones; the acoustic scale.  States 0-4 are the leading silence's (build checks): wakealign.c knows every
    silence state by their GMMs.  The feature settings, the LDA and the GMMs are in shared (write_shared), one file
    for every word of one acoustic model: they need the same silence, spn and phones (6.6 MB as one file a word, of
    which 1.6 MB in copies).  m: compact()'s"""
    states, entry, lastst, ascale = build(m); f = m.feat
    head = struct.pack("<6I4f", f["num_coefficients"], f["num_mel_bins"], f["frame_length"] * R // 1000, f["frame_shift"] * R // 1000,
                       3, 3, f["low_frequency"], f["high_frequency"], f["preemphasis_coefficient"], f["cepstral_lifter"])
    lda = struct.pack("<2I", *m.lda.shape) + m.lda.astype("<f4").tobytes()
    keep = sorted({pdf for pdf, _ in states}); remap = {p: i for i, p in enumerate(keep)}; blocks = []
    for i in keep:
        a, b = m.off[i], m.off[i + 1]
        blocks.append(m.gconsts[a:b].astype("<f4").tobytes() + m.mi[a:b].astype("<f2").tobytes() + m.iv[a:b].astype("<f2").tobytes())
    final = np.full(len(states), -np.inf); tail = bytearray(struct.pack("<I", len(states)))
    for s, (pdf, nx) in enumerate(states):
        arcs = [(d, w) for d, w in nx if d >= 0]
        for d, w in nx:
            if d < 0: final[s] = max(final[s], w)
        tail += struct.pack("<2I", remap[pdf], len(arcs)) + b"".join(struct.pack("<if", d, w) for d, w in arcs)
    tail += struct.pack("<I", len(entry)) + b"".join(struct.pack("<If", s, w) for s, w in entry)
    tail += final.astype("<f4").tobytes()
    tail += struct.pack("<I", len(lastst)) + struct.pack(f"<{len(lastst)}I", *lastst) + struct.pack("<f", ascale)
    return write_word(path, shared, head, lda, m.mi.shape[1], blocks, bytes(tail))


if __name__ == "__main__":
    if sys.argv[1] == "write":
        print(f"{sys.argv[3]}: {write_wa(compact(Model(sys.argv[2])), sys.argv[3], sys.argv[4])} bytes")
    elif sys.argv[1] == "end":
        print(word_end(compact(Model(sys.argv[2])), np.fromfile(sys.argv[3], "<i2")))
