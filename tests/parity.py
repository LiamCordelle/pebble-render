#!/usr/bin/env python3
"""Check pebble-render against the Pebble emulator, pixel for pixel.

  python3 tests/parity.py PROJECT_DIR [PROJECT_DIR ...] [--battery 100] [--tz ZONE]
                          [--press BUTTON[:MS]] [--wait MS] [--message KEY=VALUE] ...

For each project this:
  1. copies it to a temporary directory under a fixed test UUID (switching the emulator
     between different apps can hang it, while upgrading one app in place is reliable),
  2. builds it, installs it on the emery emulator, carries out the actions (button presses
     with `pebble emu-button`, AppMessages with `pebble send-app-message`) and takes a
     screenshot,
  3. renders the same moment with render.py, given the same actions, and compares every pixel.

Faces that draw seconds, or seed rand() from the clock, depend on the exact second, so the
render is retried for each second between the install and the screenshot. One identical
frame proves the match. Faces that animate with timers, or draw random elements on every
redraw, can't be compared this way.

Needs the Pebble SDK with its emulator. The emulator's battery reads 100% by default;
pass --battery if yours differs.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, 'render.py')
sys.path.insert(0, ROOT)
import render  # noqa: E402  (render.py: the action flags and their timing)
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


def emulator_actions(project, actions):
    """Carry out render.py's --press, --wait and --message actions on the emulator."""
    keys = render.message_keys(project)
    messages = []

    def send_messages():
        if not messages:
            return
        ints, strings = [], []
        for item in messages:
            name, value = item.split('=', 1)
            key = name if name.isdigit() else str(keys[name])
            (ints if re.fullmatch(r'-?\d+', value) else strings).append('%s=%s' % (key, value))
        cmd = ['send-app-message', '--emulator', 'emery']
        cmd += (['--int'] + ints if ints else []) + (['--string'] + strings if strings else [])
        pebble(*cmd)
        messages.clear()

    for kind, value in actions:
        if kind == 'message':
            messages.append(value)
            continue
        send_messages()
        if kind == 'press':
            name, _, hold = value.partition(':')
            pebble('emu-button', 'click', name, '--emulator', 'emery', '--duration',
                   hold or str(render.PRESS_HOLD_MS))
            time.sleep(render.PRESS_SETTLE_MS / 1000)
        else:
            time.sleep(value / 1000)
    send_messages()


def actions_ms(actions):
    """Simulated time the actions take in render.py."""
    total = 0
    for kind, value in actions:
        if kind == 'press':
            hold = value.partition(':')[2]
            total += (int(hold) if hold else render.PRESS_HOLD_MS) + render.PRESS_SETTLE_MS
        elif kind == 'wait':
            total += value
    return total


def action_flags(actions):
    return [x for kind, value in actions for x in ('--' + kind, str(value))]


def shoot(project, out, settle, actions):
    """Install on the emulator, act, and screenshot. Returns (install_start, shot_end) times."""
    for attempt in range(3):
        pebble('emu-button', 'click', 'back', '--emulator', 'emery')
        started = time.time()
        r = subprocess.run(['pebble', 'install', '--emulator', 'emery'], cwd=project,
                           capture_output=True, text=True, timeout=180)
        if r.returncode:
            continue
        time.sleep(settle)
        emulator_actions(project, actions)
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
    histogram = diff.convert('L').histogram()
    return sum(histogram) - histogram[0], diff


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('projects', nargs='+')
    ap.add_argument('--battery', type=int, default=100)
    ap.add_argument('--tz', default=local_zone(), help='the emulator\'s zone (default: yours)')
    ap.add_argument('--settle', type=float, default=3.0,
                    help='seconds between install and the actions (or the shot)')
    ap.add_argument('--out', default='parity-out', help='where screenshots and diffs go')
    ap.add_argument('--name', help='name for the output files (default: the project folder)')
    ap.add_argument('--press', action=render.Action)
    ap.add_argument('--wait', action=render.Action, type=int)
    ap.add_argument('--message', action=render.Action)
    ap.set_defaults(actions=[])
    a = ap.parse_args()
    run_s = math.ceil(actions_ms(a.actions) / 1000)
    os.makedirs(a.out, exist_ok=True)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for project in a.projects:
            name = a.name or os.path.basename(os.path.abspath(project))
            copy = prepare(project, tmp)
            emu_png = os.path.join(a.out, name + '-emulator.png')
            # The first install may be a switch from whatever app was running, which the
            # emulator sometimes ignores; a second install is an in-place upgrade.
            for attempt in range(2):
                started, ended = shoot(copy, emu_png, a.settle, a.actions)
                best = None
                # The render's actions take run_s of simulated time; start it so that it ends
                # somewhere between the install and the screenshot.
                for t in range(int(started) - run_s, int(ended) - run_s + 1):
                    stamp = time.localtime(t)
                    host_png = os.path.join(a.out, name + '-render.png')
                    subprocess.run([sys.executable, RENDER, copy, host_png, '--scale', '1',
                                    '--date', time.strftime('%Y-%m-%d', stamp),
                                    '--time', time.strftime('%H:%M:%S', stamp),
                                    '--tz', a.tz, '--battery', str(a.battery)]
                                   + action_flags(a.actions),
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
