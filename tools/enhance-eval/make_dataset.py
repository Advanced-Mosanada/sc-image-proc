"""Builds the evaluation data from a thermal video (step 1 of the evaluation).

    python make_dataset.py VIDEO [WORKDIR]      (WORKDIR default: build/enhance-eval)

1. Noise of the video: Immerkaer's estimator on flat areas (temporal noise), and
   the fixed-pattern noise (FPN): the high-pass of the frames averaged over the
   whole video, where the scene (the camera moves) averages out and the
   pattern fixed to the sensor stays.
2. NCLIPS clips spread evenly over the video, each LEN consecutive *unique*
   frames (exact repeats from a frame-rate conversion removed), i.e. what the
   camera delivered.
3. Per clip:
     real   the real frames
     dup    the same span as the video plays it, repeats included
     x      pseudo-clean reference: multi-frame NL-means (5 frames, patches matched
            across time, so motion does not blur it), h = 3
     y      x + white Gaussian noise sigma 2.5, new every frame, + the measured FPN
            (fixed over time): the measured noise of the video
     y5     the same with sigma 5 (a noisier camera / scene)
   written as raw gray8 640x512 files all_<name>.raw (clips one after another).
"""
import os
import sys

import cv2
import numpy as np

NCLIPS, LEN, PAD = 12, 48, 2
SIGMAS = {'y': 2.5, 'y5': 5.0}
SEED = 1234


def gray(frame):
    return frame[..., 1].copy() if frame.ndim == 3 else frame  # the video is gray: B = G = R


def measure_noise(video):
    """Temporal noise (flat areas) and the fixed pattern, from every 7th unique frame."""
    cap = cv2.VideoCapture(video)
    kernel = np.array([[1, -2, 1], [-2, 4, -2], [1, -2, 1]], np.float32)
    acc = None
    n = 0
    sigmas = []
    prev = None
    i = 0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = gray(f).astype(np.float32)
        i += 1
        if prev is not None and np.array_equal(g, prev):
            continue
        prev = g
        if i % 7:
            continue
        hp = g - cv2.GaussianBlur(g, (0, 0), 2)
        acc = hp.astype(np.float64) if acc is None else acc + hp  # float64: reproducible sums
        n += 1
        s = cv2.GaussianBlur(g, (0, 0), 1.5)
        m = np.hypot(cv2.Sobel(s, cv2.CV_32F, 1, 0), cv2.Sobel(s, cv2.CV_32F, 0, 1))
        flat = (m < np.percentile(m, 20)) & (g > 8) & (g < 247)
        r = cv2.filter2D(g, -1, kernel)
        sigmas.append(np.sqrt(np.pi / 2) / 6 * np.abs(r[flat]).mean())
    fpn = acc / n
    print(f'noise: {n} frames, temporal sigma (flat areas) p10/p50/p90 = '
          f'{np.percentile(sigmas, 10):.2f} / {np.percentile(sigmas, 50):.2f} / {np.percentile(sigmas, 90):.2f}; '
          f'fixed pattern sigma = {fpn.std():.2f}')
    return fpn


def main():
    video = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 else 'build/enhance-eval'
    os.makedirs(work, exist_ok=True)
    fpn = measure_noise(video).astype(np.float32)
    np.save(os.path.join(work, 'fpn.npy'), fpn)
    fpn -= cv2.GaussianBlur(fpn, (0, 0), 8)  # only the fine pattern; the rest is scene residue

    cap = cv2.VideoCapture(video)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    starts = [int(200 + k * (total - 400) / NCLIPS) for k in range(NCLIPS)]
    rng = np.random.default_rng(SEED)
    files = {k: open(os.path.join(work, f'all_{k}.raw'), 'wb') for k in ('x', 'y', 'y5', 'real', 'dup')}
    dup_map = []
    for k, s in enumerate(starts):
        cap.set(cv2.CAP_PROP_POS_FRAMES, s)
        uniq, dup, prev = [], [], None
        while len(uniq) < LEN + 2 * PAD:
            ok, f = cap.read()
            if not ok:
                raise RuntimeError('video ended inside a clip')
            g = gray(f)
            if PAD <= len(uniq) < LEN + PAD:
                dup.append(g)
            if prev is not None and np.array_equal(g, prev):
                continue
            uniq.append(g)
            prev = g
        real = np.array(uniq[PAD:LEN + PAD])
        x = np.array([cv2.fastNlMeansDenoisingMulti(uniq, i, 5, None, 3, 7, 21) for i in range(PAD, LEN + PAD)])
        for name, sigma in SIGMAS.items():
            noise = rng.normal(0, sigma, x.shape).astype(np.float32) + fpn[None]
            files[name].write(np.clip(np.rint(x.astype(np.float32) + noise), 0, 255).astype(np.uint8).tobytes())
        files['x'].write(x.tobytes())
        files['real'].write(real.tobytes())
        files['dup'].write(np.array(dup).tobytes())
        for d in dup:  # which unique frame each played frame is
            same = [i for i in range(LEN) if np.array_equal(real[i], d)]
            dup_map.append(k * LEN + same[0] if same else -1)
        print(f'clip {k:2d} from frame {s:5d}: real - x std {(real.astype(float) - x).std():.2f}')
    for f in files.values():
        f.close()
    np.save(os.path.join(work, 'dup_map.npy'), np.array(dup_map))


if __name__ == '__main__':
    main()
