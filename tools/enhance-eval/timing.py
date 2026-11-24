"""Per-stage time on this PC, one core (taskset) and one OpenCV thread, from scip-cli's report.

calibrate(): the README's A53 times (i.MX 8M Plus, one core, 1280x720 gray) of stages this
work did not change, against the same stages here -> A53/PC ratio per stage."""
import subprocess, re, os, sys, statistics
CLI = os.environ.get('SCIP_CLI', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'host-release', 'apps', 'scip-cli'))
VIDEO = os.environ.get('SCIP_EVAL_VIDEO', '')
A53_720P_GRAY = {'levels black=10 white=240': 1.3, 'equalize': 1.9, 'autolevels': 5.8, 'clahe': 9.2,
                 'retinex': 28, 'blur ksize=5': 1.9, 'median ksize=3': 3.1, 'bilateral': 20, 'guided': 17,
                 'temporal_mean': 5.5, 'temporal_median frames=3': 5.8, 'sharpen': 7.2, 'unsharp': 6.9, 'detail': 18}
def measure(pipeline, inp=VIDEO, n=600, core=22, extra=()):
    cmd = ['taskset', '-c', str(core), CLI, '-i', inp, '-p', pipeline, '-n', str(n), '-w', '20', *extra]
    out = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, OPENCV_FOR_THREADS_NUM='1')).stdout
    stages = {}
    for line in out.splitlines():
        m = re.match(r'\s{4}(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line)
        if m: stages.setdefault(m.group(1), []).append(float(m.group(2)))
        m = re.match(r'\s+pipeline total\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)', line)
        if m: total = float(m.group(1)); p95 = float(m.group(4))
    return stages, total, p95
def calibrate():
    ratios = {}
    for p, a53 in A53_720P_GRAY.items():
        _, t, _ = measure(p, 'pattern:1280x720:gray', n=200)
        ratios[p] = a53 / t
        print(f'  {p:28s} PC {t:7.3f} ms   A53 {a53:5.1f} ms   ratio {a53/t:5.1f}')
    r = sorted(ratios.values())
    print(f'  ratio median {statistics.median(r):.1f}, range {r[0]:.1f}..{r[-1]:.1f}')
    return ratios
if __name__ == '__main__':
    if sys.argv[1] == 'calibrate': calibrate()
    else:
        for p in sys.argv[1:]:
            st, t, p95 = measure(p, extra=('--gray-input',))
            print(f'{t:7.3f} ms (p95 {p95:6.3f})  {p}'); [print(f'     {k:16s} {v[0]:7.3f}') for k, v in st.items()]
