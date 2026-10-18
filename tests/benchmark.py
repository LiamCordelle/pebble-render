#!/usr/bin/env python3
"""Time one design iteration with the emulator against pebble-render.

  python3 tests/benchmark.py PROJECT_DIR [--runs 5] [--skip-cold]

Emulator loop (what you do today after each change):
  pebble build, then pebble install --emulator emery, a short settle, pebble screenshot.
  Measured cold (emulator not running; it boots on install) and warm (already running).

pebble-render loop:
  first render of a project (compiles its sources), a render after a code change (no
  `pebble build` needed), a render where only the scene changes (nothing compiles), and ten
  scenes rendered in parallel (the emulator runs one app at a time).

Run it with nothing else using the emulator. The cold measurement stops the emulator first.
"""
import argparse
import concurrent.futures
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, 'render.py')
SETTLE = 1.5  # seconds for the newly installed face to draw before the screenshot


def run(cmd, cwd=None):
    t = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('failed: %s\n%s' % (' '.join(cmd), (r.stdout + r.stderr)[-2000:]))
    return time.perf_counter() - t


def emulator_iteration(project, out):
    build = run(['pebble', 'build'], cwd=project)
    install = run(['pebble', 'install', '--emulator', 'emery'], cwd=project)
    time.sleep(SETTLE)
    shot = run(['pebble', 'screenshot', '--no-open', '--emulator', 'emery', out])
    return build, install, shot


def render(project, out, *extra):
    return run([sys.executable, RENDER, project, out, '--scale', '1', *extra])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('project')
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--skip-cold', action='store_true', help="don't stop the emulator")
    a = ap.parse_args()
    project = os.path.abspath(a.project)
    sources = [os.path.join(b, f) for b, _d, fs in os.walk(os.path.join(project, 'src', 'c'))
               for f in fs if f.endswith('.c')]
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'shot.png')
        rows = []

        # Emulator
        if not a.skip_cold:
            subprocess.run(['pebble', 'kill'], capture_output=True)
            time.sleep(2)
            b, i, s = emulator_iteration(project, out)
            rows.append(('Emulator, cold (boots on install)', b + i + SETTLE + s,
                         'build %.1f s, install %.1f s, settle %.1f s, screenshot %.1f s'
                         % (b, i, SETTLE, s)))
        warm = [emulator_iteration(project, out) for _ in range(a.runs)]
        totals = [b + i + SETTLE + s for b, i, s in warm]
        rows.append(('Emulator, warm', statistics.median(totals),
                     'median of %d: build %.1f s, install %.1f s, settle %.1f s, screenshot %.1f s'
                     % (a.runs, statistics.median(w[0] for w in warm),
                        statistics.median(w[1] for w in warm), SETTLE,
                        statistics.median(w[2] for w in warm))))

        # pebble-render
        cache = os.path.join(ROOT, 'build', 'apps')
        for d in os.listdir(cache) if os.path.isdir(cache) else []:
            if d.startswith(os.path.basename(project) + '-'):
                shutil.rmtree(os.path.join(cache, d), ignore_errors=True)
        rows.append(('pebble-render, first render of the project', render(project, out),
                     'compiles the project\'s sources and links'))
        changed = []
        for _ in range(a.runs):
            os.utime(sources[0])
            changed.append(render(project, out))
        rows.append(('pebble-render, after a code change', statistics.median(changed),
                     'median of %d: recompiles the changed file; no pebble build' % a.runs))
        scene = [render(project, out, '--time', '%02d:%02d' % (8 + k, 5 * k))
                 for k in range(a.runs)]
        rows.append(('pebble-render, scene change only', statistics.median(scene),
                     'median of %d: new --time, nothing compiles' % a.runs))
        t = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as ex:
            list(ex.map(lambda k: render(project, os.path.join(tmp, 'p%d.png' % k),
                                         '--time', '%02d:00' % k), range(10)))
        rows.append(('pebble-render, 10 scenes in parallel', time.perf_counter() - t,
                     'wall-clock time for all ten'))

    width = max(len(r[0]) for r in rows)
    print('\n%s (%s)\n' % (os.path.basename(project), time.strftime('%Y-%m-%d')))
    for name, secs, note in rows:
        print('%-*s  %6.2f s   %s' % (width, name, secs, note))


if __name__ == '__main__':
    main()
