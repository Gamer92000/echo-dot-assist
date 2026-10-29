"""Cuts one wake word's aligner out of an MFA acoustic model (src/hassmic/wakealign.h).  Runs in the Montreal Forced
Aligner's environment (kalpy reads the model, `mfa g2p` spells unknown words); tools/wakealign/build.sh calls it.

  extract.py <acoustic model dir> <dictionary> <g2p model or -> <out.npz> <word> [<word> ...]

The model dir is MFA's model zip unpacked (final.alimdl, tree, lda.mat, meta.json).  The phrase's pronunciations: each
word's in the dictionary (all of them, the likeliest 3), else the G2P model's 2 best; a phrase is its words' phones
run together (at most 4 combinations, the likeliest).  Each pronunciation is a path of its own in the graph.  Out: for
each node of the graph its HMM states, each with its pdf (from MFA's decision tree by the triphone around it) and its
arcs with the trained log probabilities; the pdfs' GMMs; the LDA matrix; the feature settings; the dictionary's
silence probabilities of each pronunciation and of "[bracketed]" (spn).  kalpy's ContextDependency.Compute drops the
pdf it computes (binding of a pointer argument), so the tree is read from its text form (copy-tree) and walked here."""
import itertools, json, os, subprocess, sys, tempfile
import numpy as np
from _kalpy.matrix import FloatMatrix
from kalpy.gmm.utils import read_gmm_model
from kalpy.utils import read_kaldi_object

mdir, dict_path, g2p, out, words = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], [w.lower() for w in sys.argv[5:]]
meta = json.load(open(f"{mdir}/meta.json"))
if "phone_mapping" in meta: pm = dict(meta["phone_mapping"])
else:                                   # MFA 2.0's (portuguese_mfa) list the phones only: sil 1, spn 2, then sorted, as 3.x
    pm = {"sil": 1, "spn": 2, **{q: i + 3 for i, q in enumerate(sorted(meta["phones"]))}}
pm["<eps>"] = 0
DEFAULT = [1.0, meta.get("silence_probability", 0.21), 1.0, 1.0]


def entries(w):
    """[(pronunciation probability, silence after, corrections for silence / no silence before), phones]"""
    out = []
    for l in open(dict_path, encoding="utf-8"):
        f = l.rstrip("\n").split("\t")
        if f[0] == w and len(f) >= 6: out.append(([float(v) for v in f[1:5]], f[5].split()))
    out.sort(key=lambda e: -e[0][0])
    return out[:3]


def spell(w):
    """the G2P model's 2 best for a word the dictionary lacks"""
    if g2p == "-": raise SystemExit(f"{w}: not in {dict_path}, and no G2P model")
    with tempfile.TemporaryDirectory() as d:
        open(f"{d}/in.txt", "w").write(w + "\n")
        subprocess.run(["mfa", "g2p", f"{d}/in.txt", g2p, f"{d}/out.txt", "--num_pronunciations", "2"], check=True, capture_output=True)
        prons = [l.rstrip("\n").split("\t")[1].split() for l in open(f"{d}/out.txt", encoding="utf-8") if "\t" in l]
    return [(list(DEFAULT), p) for p in prons if all(q in pm for q in p)]


per_word = [entries(w) or spell(w) for w in words]
variants = []
for combo in itertools.product(*per_word):
    phones = [q for _, p in combo for q in p]
    prob = float(np.prod([e[0][0] for e in combo]))
    variants.append(dict(phones=phones, probs=[prob, combo[-1][0][1], combo[0][0][2], combo[0][0][3]]))
variants.sort(key=lambda v: -v["probs"][0]); variants = variants[:4]
seen = set(); variants = [v for v in variants if not (tuple(v["phones"]) in seen or seen.add(tuple(v["phones"])))]
assert variants and all(len(v["phones"]) >= 2 for v in variants), variants
bprobs = (entries("[bracketed]") or [([1.0, 0.5, 1.0, 1.0], None)])[0][0]   # japanese_mfa has no [bracketed]
tm, am = read_gmm_model(f"{mdir}/final.alimdl")
topo = tm.GetTopo()
assert tm.NumPhones() == max(pm.values()), (tm.NumPhones(), max(pm.values()))
tree_txt = f"{mdir}/tree.txt"
if not os.path.exists(tree_txt):
    subprocess.run(["copy-tree", "--binary=false", f"{mdir}/tree", tree_txt], check=True, capture_output=True)


def parse_tree(path):
    """Kaldi's EventMap in text: CE answer | SE key [ set ] { yes no } | TE key n ( map ... ) | NULL"""
    t = open(path).read()
    for c in "[]{}()": t = t.replace(c, f" {c} ")
    tok = t.split(); assert tok[:4] == ["ContextDependency", "3", "1", "ToPdf"], tok[:4]
    pos = [4]
    def nxt(): pos[0] += 1; return tok[pos[0] - 1]
    def node():
        t = nxt()
        if t == "NULL": return None
        if t == "CE": return ("CE", int(nxt()))
        if t == "SE":
            key = int(nxt()); assert nxt() == "["; yes = set()
            while (u := nxt()) != "]": yes.add(int(u))
            assert nxt() == "{"; a = node(); b = node(); assert nxt() == "}"
            return ("SE", key, yes, a, b)
        if t == "TE":
            key = int(nxt()); n = int(nxt()); assert nxt() == "("
            tab = [node() for _ in range(n)]; assert nxt() == ")"
            return ("TE", key, tab)
        raise ValueError(t)
    return node()


TREE = parse_tree(tree_txt)


def lookup(ev, m=TREE):                 # keys: -1 pdf class, 0 left phone, 1 the phone, 2 right phone
    while m[0] != "CE": m = (m[3] if ev[m[1]] in m[2] else m[4]) if m[0] == "SE" else m[2][ev[m[1]]]
    return m[1]


def phone_hmm(ph, left, right):
    """[(pdf, [(dest state, -1 = leaves the phone; log prob), ...]) per emitting state]"""
    p = pm[ph]; ent = topo.TopologyForPhone(p)
    pdfs = [lookup({-1: ent[s].forward_pdf_class, 0: pm[left], 1: p, 2: pm[right]}) for s in range(len(ent) - 1)]
    states = []
    for s in range(len(ent) - 1):
        ts = tm.TupleToTransitionState(p, s, pdfs[s], pdfs[s])
        arcs = [(tr[0] if tr[0] < len(ent) - 1 else -1, tm.GetTransitionLogProb(tm.PairToTransitionId(ts, i)))
                for i, tr in enumerate(ent[s].transitions)]
        states.append((pdfs[s], arcs))
    return states


# the graph's nodes (name, phone, left, right): per pronunciation k its first phone after silence or at the start
# ("<eps>"), its last before silence or straight into spn (Kaldi picks each triphone by the phone actually next to it);
# silence and spn are context independent in MFA's trees, one node each
nodes = [("sil_start", "sil", "<eps>", variants[0]["phones"][0])]
for k, v in enumerate(variants):
    P = v["phones"]
    nodes += [(f"{k}:w0_sil", P[0], "sil", P[1]), (f"{k}:w0_start", P[0], "<eps>", P[1])]
    nodes += [(f"{k}:w{i}", P[i], P[i - 1], P[i + 1]) for i in range(1, len(P) - 1)]
    nodes += [(f"{k}:wl_sil", P[-1], P[-2], "sil"), (f"{k}:wl_spn", P[-1], P[-2], "spn")]
nodes += [("sil_mid", "sil", variants[0]["phones"][-1], "spn"), ("spn", "spn", "sil", "sil"), ("sil_end", "sil", "spn", "<eps>")]
hmms = [phone_hmm(ph, l, r) for _, ph, l, r in nodes]
used = sorted({pdf for h in hmms for pdf, _ in h})
g = {p: am.GetPdf(p) for p in used}
lda = read_kaldi_object(FloatMatrix, f"{mdir}/lda.mat").numpy()
np.savez(out, word=" ".join(words), variants=json.dumps(variants), chain=np.array([n[0] for n in nodes]),
         hmms=json.dumps([[[int(pdf), [[int(d), float(lp)] for d, lp in arcs]] for pdf, arcs in h] for h in hmms]),
         pdfs=np.array(used), ngauss=np.array([len(g[p].gconsts().numpy()) for p in used]),
         gconsts=np.concatenate([g[p].gconsts().numpy() for p in used]),
         means_invvars=np.concatenate([g[p].means_invvars().numpy() for p in used]),
         inv_vars=np.concatenate([g[p].inv_vars().numpy() for p in used]),
         lda=lda, features=json.dumps(meta["features"]), bracketed_probs=np.array(bprobs))
print(f"{' '.join(words)}: " + " | ".join(" ".join(v["phones"]) for v in variants) +
      f"; {sum(len(h) for h in hmms)} states, {len(used)} pdfs, {sum(len(g[p].gconsts().numpy()) for p in used)} gaussians")
