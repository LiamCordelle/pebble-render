#!/usr/bin/env python3
"""Check pebble-render against the Pebble emulator, pixel for pixel.

  python3 tests/parity.py PROJECT_DIR [PROJECT_DIR ...] [--battery 100] [--tz ZONE]

For each project this:
  1. copies it to a temporary directory under a fixed test UUID (switching the emulator
     between different apps can hang it, while upgrading one app in place is reliable),
  2. builds it, installs it on the emery emulator and takes a screenshot,
  3. renders the same moment with render.py and compares every pixel.

Faces that draw seconds, or seed rand() from the clock, depend on the exact second, so the
render is retried for each second between the install and the screenshot. One identical
frame proves the match. Faces that animate with timers, or draw random elements on every
redraw, can't be compared this way.

Needs the Pebble SDK with its emulator. The emulator's battery reads 100% by default;
pass --battery if yours differs.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
RENDER = os.path.join(os.path.dirname(HERE), 'render.py')
TEST_UUID = 'D0C5E1E4-77C0-4C55-9E5C-0BE4E0A11E57'
DIALOG_GREY = (171, 171, 171)  # firmware dialog background after colour correction


def pebble(*args, timeout=120):
    return subprocess.run(['pebble', *args], capture_output=True, text=True, timeout=timeout)


def local_zone():
    try:
        path = os.path.realpath('/etc/localtime')
        return path.split('zoneinfo/', 1)[1]
    except (OSError, IndexError):
        return 'UTC'


def prepare(project, tmp):
    """Copy the project with the test UUID and build it. Returns the copy's path."""
    copy = os.path.join(tmp, os.path.basename(os.path.abspath(project)))
    shutil.copytree(project, copy, ignore=shutil.ignore_patterns('build', '.git', 'node_modules'))
    if os.path.isdir(os.path.join(project, 'node_modules')):
        os.symlink(os.path.abspath(os.path.join(project, 'node_modules')),
                   os.path.join(copy, 'node_modules'))
    pkg = os.path.join(copy, 'package.json')
    data = json.load(open(pkg))
    data['pebble']['uuid'] = TEST_UUID
    data['pebble']['targetPlatforms'] = ['emery']
    json.dump(data, open(pkg, 'w'), indent=2)
    r = subprocess.run(['pebble', 'build'], cwd=copy, capture_output=True, text=True)
    if r.returncode:
        sys.exit('pebble build failed for %s:\n%s' % (project, (r.stdout + r.stderr)[-3000:]))
    return copy


def is_dialog(png):
    im = Image.open(png).convert('RGB')
    corners = [im.getpixel(p) for p in [(2, 2), (197, 2), (2, 225), (197, 225)]]
    return all(c == DIALOG_GREY for c in corners)


def shoot(project, out, wait):
    """Install on the emulator and screenshot. Returns (install_start, shot_end) times."""
    for attempt in range(3):
        pebble('emu-button', 'click', 'back', '--emulator', 'emery')
        started = time.time()
        r = subprocess.run(['pebble', 'install', '--emulator', 'emery'], cwd=project,
                           capture_output=True, text=True, timeout=180)
        if r.returncode:
            continue
        time.sleep(wait)
        r = pebble('screenshot', '--no-open', '--emulator', 'emery', out)
        ended = time.time()
        if r.returncode == 0 and os.path.exists(out) and not is_dialog(out):
            return started, ended
    sys.exit('could not get a clean emulator screenshot of %s' % project)


def compare(a_png, b_png):
    a = Image.open(a_png).convert('RGB')
    b = Image.open(b_png).convert('RGB')
    diff = ImageChops.difference(a, b)
    if not diff.getbbox():
        return 0, diff
    return sum(1 for p in diff.convert('L').getdata() if p), diff


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('projects', nargs='+')
    ap.add_argument('--battery', type=int, default=100)
    ap.add_argument('--tz', default=local_zone(), help='the emulator\'s zone (default: yours)')
    ap.add_argument('--wait', type=float, default=3.0, help='seconds between install and shot')
    ap.add_argument('--out', default='parity-out', help='where screenshots and diffs go')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for project in a.projects:
            name = os.path.basename(os.path.abspath(project))
            copy = prepare(project, tmp)
            emu_png = os.path.join(a.out, name + '-emulator.png')
            # The first install may be a switch from whatever app was running, which the
            # emulator sometimes ignores; a second install is an in-place upgrade.
            for attempt in range(2):
                started, ended = shoot(copy, emu_png, a.wait)
                best = None
                for t in range(int(started), int(ended) + 1):
                    stamp = time.localtime(t)
                    host_png = os.path.join(a.out, name + '-render.png')
                    subprocess.run([sys.executable, RENDER, copy, host_png, '--scale', '1',
                                    '--date', time.strftime('%Y-%m-%d', stamp),
                                    '--time', time.strftime('%H:%M:%S', stamp),
                                    '--tz', a.tz, '--battery', str(a.battery)],
                                   capture_output=True, text=True, check=True)
                    n, diff = compare(emu_png, host_png)
                    if best is None or n < best[0]:
                        best = (n, time.strftime('%H:%M:%S', stamp), diff)
                    if n == 0:
                        break
                if best[0] < 200 * 228 // 4:
                    break
            n, when, diff = best
            if n == 0:
                print('%-24s identical (at %s)' % (name, when))
            else:
                failures += 1
                diff_png = os.path.join(a.out, name + '-diff.png')
                diff.point(lambda v: 255 if v else 0).save(diff_png)
                print('%-24s %d pixels differ (best at %s); see %s' % (name, n, when, diff_png))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
