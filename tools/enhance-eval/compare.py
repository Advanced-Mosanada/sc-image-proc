"""Per-clip comparison (step 3 of the evaluation).

    python compare.py FILE      FILE: pipelines, one per line; the 3rd is the reference

Every metric per clip (scene), its mean with a bootstrap 95 % interval over the
12 clips, and the paired difference of each pipeline after the 3rd against the
3rd, with the number of clips where it is better. A difference whose interval
excludes 0 is consistent across scenes, not an average pulled by one scene.
"""
import sys

import numpy as np

import evalpipe as ev
rng = np.random.default_rng(0)
pipes = [l.strip() for l in open(sys.argv[1]) if l.strip() and not l.startswith('#')]
ref = ev.reference()
res = {}
for p in pipes:
    S = ev.run(p, 'x'); Y = ev.run(p, 'y'); Y5 = ev.run(p, 'y5'); R = ev.run(p, 'real')
    per = {k: [[] for _ in range(ev.NCLIP)] for k in ('E', 'noise', 'noise5', 'nreal', 'halo', 'banding', 'clip')}
    for j, i in enumerate(ev.METRIC):
        c = i // ev.LEN; f = ref['frames'][j]
        s = S[i].astype(np.float32)
        eg = ev.grad(s)[f['edge']].mean() / f['gx_edge']
        hd = ev.local_amp(ev.bandpass(s))[f['hom']].mean() / f['A_x']
        lc = ev.local_std(ev.cv2.GaussianBlur(s, (0, 0), 2), 31)[f['valid']].mean() / f['lc_x']
        per['E'][c].append((eg * hd * lc) ** (1 / 3))
        per['noise'][c].append((Y[i].astype(np.float32) - s)[f['hom']].std())
        per['noise5'][c].append((Y5[i].astype(np.float32) - s)[f['hom']].std())
        per['nreal'][c].append((R[i].astype(np.float32) - s)[f['hom']].std())
        per['halo'][c].append(ev._halo(s, f))
        per['banding'][c].append(ev._banding(s, f))
        o = Y[i]; per['clip'][c].append(100 * (((o == 0) | (o == 255)).mean() - ((ev.reference()['Y'][i] == 0) | (ev.reference()['Y'][i] == 255)).mean()))
    res[p] = {k: np.array([np.mean(v) for v in vs]) for k, vs in per.items()}
    for inp in ('x', 'y', 'y5', 'real'):
        ev.os.remove(ev.output_path(p, inp))
def ci(a):
    b = [rng.choice(a, len(a)).mean() for _ in range(4000)]
    return a.mean(), np.percentile(b, 2.5), np.percentile(b, 97.5)
for p in pipes:
    print(p)
    for k, v in res[p].items():
        m, lo, hi = ci(v); print(f'   {k:7s} {m:6.3f}  [{lo:6.3f}, {hi:6.3f}]   per clip min {v.min():.2f} max {v.max():.2f}')
base = pipes[2]
print('\npaired differences vs', base[:60])
for p in pipes[3:]:
    print(' ', p[:110])
    for k in ('E', 'noise', 'noise5', 'nreal', 'halo', 'banding'):
        d = res[p][k] - res[base][k]; m, lo, hi = ci(d)
        print(f'     d{k:6s} {m:+.3f} [{lo:+.3f}, {hi:+.3f}]  better in {int(((d > 0) if k == "E" else (d < 0)).sum())}/12 clips')
