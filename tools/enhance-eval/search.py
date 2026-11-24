"""Evaluates pipelines in parallel and ranks them (step 2 of the evaluation).

    python search.py FILE [FILE...]     evaluate the pipelines listed (one per line, # = comment)
    python search.py --rank [TEXT]      rank everything evaluated so far (optionally: containing TEXT)

Results are appended to $SCIP_EVAL_DIR/results.jsonl; a pipeline already there is
not evaluated again (delete its line after changing a stage's code).

Selection rule (fixed before the search, see README.md):
  E = geometric mean of edge_gain, hom_detail and local_contrast, on P(x)
  constraints: noise(y)  <= 0.8 * 2.75 = 2.20     visible noise reduction (input: 2.75)
               noise(real) <= 2.84                no more noise than the input, real video
               halo <= 0.25 levels, structure(y) >= 0.95,
               extra clipping <= 1 %, flicker(real) <= 0.6 levels,
               banding <= 7.3 % (the input's own value: no new false contours)
The banding limit was added after the search: looking at the frames showed
contour bands that no other metric caught (see docs/thermal-enhancement.md).
"""
import json
import os
import sys
from multiprocessing import Pool

import evalpipe

DB = os.path.join(evalpipe.HERE, 'results.jsonl')
LIMITS = dict(noise=2.20, noise_real=2.84, halo=0.25, structure=0.95, clip=0.010, flicker=0.6, banding=7.3)


def load_results():
    if not os.path.exists(DB):
        return {}
    rows = (json.loads(line) for line in open(DB))
    return {r['pipeline']: r for r in rows}


def _work(pipeline):
    try:
        return evalpipe.evaluate(pipeline)
    except Exception as e:  # noqa: BLE001 - report and go on with the others
        return dict(pipeline=pipeline, error=str(e))


def evaluate_all(pipelines, procs):
    have = load_results()
    todo = [p for p in dict.fromkeys(pipelines) if p not in have]
    with Pool(procs) as pool, open(DB, 'a') as f:
        for r in pool.imap_unordered(_work, todo):
            if 'error' in r:
                print('ERROR', r['pipeline'], r['error'], flush=True)
                continue
            f.write(json.dumps(r) + '\n')
            f.flush()
            print('done ', r['pipeline'], flush=True)


def score(r):
    return (r['edge_gain'] * r['hom_detail'] * r['local_contrast']) ** (1 / 3)


def passes(r):
    y, real = r['by_input']['y'], r['by_input']['real']
    return (y['noise'] <= LIMITS['noise'] and real['noise'] <= LIMITS['noise_real']
            and r['halo'] <= LIMITS['halo'] and y['structure'] >= LIMITS['structure']
            and y['clip'] <= LIMITS['clip'] and real['flicker'] <= LIMITS['flicker']
            and r['banding'] <= LIMITS['banding'])


def show(rows):
    print(f"{'E':>5} {'noise':>5} {'nReal':>5} {'edge':>5} {'homD':>5} {'lc':>5} {'dnr':>5} "
          f"{'halo':>5} {'band%':>5} {'struc':>5} {'clip%':>5} {'flick':>5}  pipeline")
    for r in rows:
        y, real = r['by_input']['y'], r['by_input']['real']
        print(f"{score(r):5.3f} {y['noise']:5.2f} {real['noise']:5.2f} {r['edge_gain']:5.2f} "
              f"{r['hom_detail']:5.2f} {r['local_contrast']:5.2f} {y['dnr']:5.2f} {r['halo']:5.2f} {r['banding']:5.2f} "
              f"{y['structure']:5.3f} {100 * y['clip']:5.2f} {real['flicker']:5.2f}  {r['pipeline']}")


def rank(text=None, top=25):
    rows = [r for r in load_results().values() if 'real' in r['by_input'] and (not text or text in r['pipeline'])]
    good = sorted((r for r in rows if passes(r)), key=score, reverse=True)
    print(f'{len(good)} of {len(rows)} pass the constraints')
    show(good[:top])


if __name__ == '__main__':
    if sys.argv[1] == '--rank':
        rank(sys.argv[2] if len(sys.argv) > 2 else None)
    else:
        lines = [l.strip() for name in sys.argv[1:] for l in open(name)]
        evaluate_all([l for l in lines if l and not l.startswith('#')], int(os.environ.get('PROCS', '8')))
        rank()
