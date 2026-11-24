"""Evaluation harness: runs a scip pipeline (the real C++ stages, via scip-cli,
lossless raw gray8 in and out) on the evaluation clips and measures it.

Data in $SCIP_EVAL_DIR (default build/enhance-eval), made by make_dataset.py
(12 clips x 48 frames, 640x512):
  x    pseudo-clean reference (multi-frame NL-means of the real frames)
  y    x + white Gaussian noise sigma 2.5 (new every frame) + measured fixed pattern
  y5   the same with sigma 5
  real the real frames (duplicates removed)

For a pipeline P:  s = P(x) (what P does to the scene), o = P(y) (what is seen),
o - s = the noise that reaches the output. Metrics on frames 12..47 of each clip
(step 3: 144 frames; temporal stages have warmed up).
"""
import hashlib, os, shutil, subprocess, sys, json
import numpy as np, cv2

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
HERE = os.environ.get('SCIP_EVAL_DIR', os.path.join(REPO, 'build', 'enhance-eval'))  # data and results
CLI = os.environ.get('SCIP_CLI', os.path.join(REPO, 'build', 'host-release', 'apps', 'scip-cli'))
W, H = 640, 512
NCLIP, LEN = 12, 48
METRIC = [c * LEN + i for c in range(NCLIP) for i in range(12, LEN, 3)]
cv2.setNumThreads(1)


def load(path):
    return np.memmap(path, np.uint8, 'r').reshape(-1, H, W)


def output_path(pipeline, inp):
    tag = hashlib.sha1((pipeline + '|' + inp).encode()).hexdigest()[:16]
    return os.path.join(HERE, 'out', f'{tag}_{inp}.raw')


def run(pipeline, inp):
    """Runs scip-cli on all_<inp>.raw; returns the output frames (memmap)."""
    src = os.path.join(HERE, f'all_{inp}.raw')
    out = output_path(pipeline, inp)
    if not os.path.exists(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        free = shutil.disk_usage(os.path.dirname(out)).free
        if free < 10 * 2**30:  # an output is ~190 MB; never fill the disk
            raise RuntimeError(f'only {free / 2**30:.1f} GB free for {out}')
        tmp = out + '.part'
        cmd = [CLI, '-w', '0', '-i',
               f'gst:filesrc location={src} ! rawvideoparse width={W} height={H} format=gray8 '
               'framerate=25/1 ! appsink name=scip_sink sync=false',
               '-p', pipeline, '-o', f'gst:appsrc name=scip_src ! filesink location={tmp}']
        env = dict(os.environ, OPENCV_FOR_THREADS_NUM='1')
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            raise RuntimeError(f'{pipeline}: {r.stderr[-500:]}')
        os.replace(tmp, out)
    a = load(out)
    if a.shape[0] != os.path.getsize(src) // (W * H):
        raise RuntimeError(f'{pipeline}: {a.shape[0]} frames out')
    return a


def grad(img):
    gx = cv2.Sobel(img, cv2.CV_32F, 1, 0, ksize=3) / 8
    gy = cv2.Sobel(img, cv2.CV_32F, 0, 1, ksize=3) / 8
    return np.sqrt(gx * gx + gy * gy)


def bandpass(img, sigma=2.0):
    return img - cv2.GaussianBlur(img, (0, 0), sigma)


def local_amp(img, k=7):
    return np.sqrt(np.maximum(cv2.boxFilter(img * img, -1, (k, k)), 0))


def local_std(img, k):
    m = cv2.boxFilter(img, -1, (k, k))
    return np.sqrt(np.maximum(cv2.boxFilter(img * img, -1, (k, k)) - m * m, 0))


def ssim_parts(a, b):
    """Mean SSIM and mean SSIM 'structure' term (contrast/brightness invariant)."""
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    C3 = C2 / 2
    g = lambda z: cv2.GaussianBlur(z, (11, 11), 1.5)
    ma, mb = g(a), g(b)
    va = np.maximum(g(a * a) - ma * ma, 0)
    vb = np.maximum(g(b * b) - mb * mb, 0)
    cov = g(a * b) - ma * mb
    ssim = ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2))
    struct = (cov + C3) / (np.sqrt(va * vb) + C3)
    return ssim[5:-5, 5:-5].mean(), struct[5:-5, 5:-5].mean()


def tone_match(src, ref):
    """Monotone global mapping of src that gives ref's histogram."""
    s_vals, s_idx, s_cnt = np.unique(src.ravel(), return_inverse=True, return_counts=True)
    r_vals, r_cnt = np.unique(ref.ravel(), return_counts=True)
    s_q = np.cumsum(s_cnt) / src.size
    r_q = np.cumsum(r_cnt) / ref.size
    return np.interp(s_q, r_q, r_vals)[s_idx].reshape(src.shape).astype(np.float32)


_REF = {}


def reference():
    """Masks and reference quantities from x, per metric frame (cached)."""
    if _REF:
        return _REF
    X, Y, Y5, R = load(f'{HERE}/all_x.raw'), load(f'{HERE}/all_y.raw'), load(f'{HERE}/all_y5.raw'), load(f'{HERE}/all_real.raw')
    frames = []
    for i in METRIC:
        x = X[i].astype(np.float32)
        gm = grad(cv2.GaussianBlur(x, (0, 0), 1.0))
        valid = (x > 5) & (x < 250)
        valid[:8] = valid[-8:] = False
        valid[:, :8] = valid[:, -8:] = False
        edge = (gm > np.percentile(gm, 90)) & valid
        hom = (gm < np.percentile(gm, 50)) & valid
        strong = gm > np.percentile(gm, 97)
        smooth = (gm < 0.4) & valid  # slow gradients (sky, walls): where banding shows
        band = cv2.dilate(strong.astype(np.uint8), np.ones((7, 7), np.uint8)).astype(bool) & valid
        frames.append(dict(x=x, edge=edge, hom=hom, band=band, valid=valid, smooth=smooth,
                           gx_edge=grad(x)[edge].mean(), A_x=local_amp(bandpass(x))[hom].mean(),
                           lc_x=local_std(cv2.GaussianBlur(x, (0, 0), 2), 31)[valid].mean()))
    _REF['frames'] = frames
    _REF['X'], _REF['Y'], _REF['Y5'], _REF['R'] = X, Y, Y5, R
    _REF['clip_in'] = {k: np.mean([(a[i] == 0).mean() + (a[i] == 255).mean() for i in METRIC])
                       for k, a in (('y', Y), ('y5', Y5), ('real', R))}
    return _REF


def frame_means(a):
    return np.array([a[i].mean() for i in range(a.shape[0])])


def evaluate(pipeline, inputs=tuple(os.environ.get('EVAL_INPUTS', 'y,real').split(',')), cleanup=True):
    ref = reference()
    S = run(pipeline, 'x')
    res = dict(pipeline=pipeline)
    xm = frame_means(ref['X'])
    per = {}
    sstats = []
    for j, i in enumerate(METRIC):
        f = ref['frames'][j]
        s = S[i].astype(np.float32)
        sstats.append(dict(
            edge_gain=grad(s)[f['edge']].mean() / f['gx_edge'],
            hom_detail=local_amp(bandpass(s))[f['hom']].mean() / f['A_x'],
            A_s=local_amp(bandpass(s))[f['hom']].mean(),
            local_contrast=local_std(cv2.GaussianBlur(s, (0, 0), 2), 31)[f['valid']].mean() / f['lc_x'],
            rms=s.std(),
            distortion=np.sqrt(((s - f['x']) ** 2)[f['valid']].mean()),
            halo=_halo(s, f),
            banding=_banding(s, f)))
    for k in sstats[0]:
        res[k] = float(np.mean([d[k] for d in sstats]))
    res['rms_x'] = float(np.mean([f['x'].std() for f in ref['frames']]))
    for inp in inputs:
        O = run(pipeline, inp)
        vals = []
        for j, i in enumerate(METRIC):
            f = ref['frames'][j]
            s = S[i].astype(np.float32)
            o = O[i].astype(np.float32)
            n = o - s
            ssim, struct = ssim_parts(o, f['x'])
            hist = np.bincount(O[i].ravel(), minlength=256) / o.size
            vals.append(dict(
                noise=n[f['hom']].std(),
                noise_bp=local_amp(bandpass(n))[f['hom']].mean(),
                edge_gain=grad(o)[f['edge']].mean() / f['gx_edge'],
                ssim=ssim, structure=struct,
                psnr=10 * np.log10(255 ** 2 / ((o - f['x']) ** 2)[f['valid']].mean()),
                clip=(hist[0] + hist[255]),
                entropy=-(hist[hist > 0] * np.log2(hist[hist > 0])).sum()))
        d = {k: float(np.mean([v[k] for v in vals])) for k in vals[0]}
        d['clip'] -= ref['clip_in'][inp]
        d['dnr'] = res['A_s'] / d['noise_bp']
        om = frame_means(O)
        dd = np.abs(np.diff(om) - np.diff(xm))
        dd = np.delete(dd, [c * LEN - 1 for c in range(1, NCLIP)])  # clip boundaries
        d['flicker'] = float(dd.mean())
        per[inp] = d
    res['by_input'] = per
    if cleanup:  # the outputs are ~190 MB each: keep none
        for inp in ('x',) + tuple(inputs):
            os.remove(output_path(pipeline, inp))
    return res


def _halo(s, f):
    """Over/undershoot (ringing, halos) near strong edges, in levels.

    q = guided filter of s with x as the guide (window 15): the best local
    linear map a*x + b, so global AND smooth local tone changes (CLAHE, local
    stretch) are explained by it, while a ring next to a monotone edge of x is
    not. Halo = how far s goes outside q's local (7x7) range."""
    x = f['x']
    k = (15, 15)
    box = lambda z: cv2.boxFilter(z, -1, k)
    mx, ms = box(x), box(s)
    vx = box(x * x) - mx * mx
    cxs = box(x * s) - mx * ms
    a = cxs / (vx + 10.0)
    b = ms - a * mx
    q = box(a) * x + box(b)
    lo = cv2.erode(q, np.ones((7, 7), np.uint8))
    hi = cv2.dilate(q, np.ones((7, 7), np.uint8))
    over = np.maximum(s - hi, 0) + np.maximum(lo - s, 0)
    return float(over[f['band']].mean())


def _banding(s, f):
    """False contours, in % of the smooth areas: where the reference changes
    by < 0.4 levels per pixel, even a 3x contrast gain gives steps of at most
    2 levels between neighbours; steps of 3 or more are bands (an 8-bit image
    stretched without interpolation)."""
    if not f['smooth'].any():
        return 0.0
    dx = np.zeros_like(s)
    dy = np.zeros_like(s)
    dx[:, :-1] = np.abs(np.diff(s, axis=1))
    dy[:-1, :] = np.abs(np.diff(s, axis=0))
    return float(100 * (np.maximum(dx, dy)[f['smooth']] >= 3).mean())


def input_baseline():
    """Metrics of the unprocessed inputs (pipeline 'none')."""
    return evaluate('none')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        print(json.dumps(evaluate(p), indent=1))
