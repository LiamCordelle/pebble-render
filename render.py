#!/usr/bin/env python3
"""Render a Pebble Time 2 (emery) watchface screenshot on your computer, without the emulator.

  python3 render.py PROJECT_DIR OUT.png [options]

Writes OUT.png, the same 200x228 RGBA PNG `pebble screenshot` saves (including its colour
correction), and OUT_3x.png, a nearest-neighbour upscale that's easier to inspect.

How: the real PebbleOS graphics, font, window and layer code is compiled for your computer
together with the project's C sources. For an ordinary app, its main() runs, init() builds
the windows and layers, and the top window is drawn with the firmware's own code, the
firmware's own fonts (extracted from the SDK's emulator image by setup.sh) and the app's
resources from its last `pebble build`. Renders take about a second, and any number can
run in parallel.

Run `pebble build` first: resources (fonts, images) and RESOURCE_ID_* come from the
project's build/ directory. Code changes don't need a rebuild.

Scene (ordinary apps):
  --time HH:MM[:SS]     the time on the watch (default 10:09:00)
  --date YYYY-MM-DD     the date (default 2026-10-17)
  --tz ZONE             the watch's time zone, as an IANA name such as Europe/London
                        (default UTC); matters for faces that show UTC or other zones
  --12h                 12-hour clock style (default 24-hour)
  --battery N           battery percentage (default 80)
  --charging            plugged in and charging
  --no-phone            phone disconnected
  --steps N             today's step count (default 0)
  --hr N                current heart rate (default 0)
  --run-ms N            let app timers run for N ms before capturing (animations)
  --no-tick             don't deliver the first tick (the tick handler isn't called)
  --message KEY=VALUE   deliver an AppMessage at start, as if the phone sent it; KEY is a
                        messageKeys name from package.json or a number; repeatable

Watchface Lab projects (src/c/lab_designs.h) use the lab's scenarios instead:
  --scenario N          0 demo, 1 stress, 2 empty
  --taps N              design index in multi-design lab apps
  --detail              the design's tap-for-detail view

Output:
  --scale N             also write OUT_<N>x.png (default 3; 1 to skip)
  --raw FILE            also write the raw 8-bit framebuffer
  --no-correction       skip the LCD colour correction (pebble screenshot --no-correction)
  -v                    print firmware log lines
"""
import argparse
import calendar
import fcntl
import json
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, 'hostgfx'))

import build  # noqa: E402  (hostgfx/build.py)
import screenshot  # noqa: E402  (hostgfx/screenshot.py)

FW_DIR = os.path.join(ROOT, 'fw')  # system_resources.pbpack + builtin_<id>.bin


def last_pebble_build(project):
    """Time of the last `pebble build`: pebble-tool reconfigures waf on every build."""
    build_dir = os.path.join(project, 'build')
    marks = [os.path.join(build_dir, f) for f in os.listdir(build_dir) if f.startswith('.lock-waf')]
    marks += [os.path.join(build_dir, 'c4che', '_cache.py'),
              os.path.join(build_dir, 'emery', 'app_resources.pbpack')]
    return max(os.path.getmtime(m) for m in marks if os.path.exists(m))


def stale_resources(project):
    """Why the last `pebble build`'s resources may not match the project, or None.

    Fonts, images and RESOURCE_ID_* come from the project's build/ directory, so resource
    changes need a `pebble build` first (code changes do not)."""
    built_at = last_pebble_build(project)
    paths = [os.path.join(project, 'package.json')]
    for base, _dirs, files in os.walk(os.path.join(project, 'resources')):
        paths += [os.path.join(base, f) for f in files]
    for p in paths:
        if os.path.exists(p) and os.path.getmtime(p) > built_at:
            return '%s changed after the last build' % os.path.relpath(p, project)
    return None


def check_firmware():
    """Warn when the SDK's emulator firmware is not the one the renderer was set up from."""
    try:
        info = json.load(open(os.path.join(FW_DIR, 'firmware.json')))
        st = os.stat(info['sdk_elf'])
    except (OSError, ValueError, KeyError):
        print('render: warning: cannot verify the emulator firmware version; re-run setup.sh',
              file=sys.stderr)
        return
    if (st.st_size, int(st.st_mtime)) != (info['sdk_elf_size'], info['sdk_elf_mtime']):
        print('render: warning: the SDK emulator firmware changed since setup (was %s); re-run '
              'setup.sh with PEBBLEOS_TAG set to the new firmware tag' % info['firmware_version'],
              file=sys.stderr)


def message_keys(project):
    """messageKeys name -> number, from the files `pebble build` generates."""
    keys = {}
    for path in [os.path.join(project, 'build', 'src', 'message_keys.auto.c'),
                 os.path.join(project, 'build', 'js', 'message_keys.json')]:
        if not os.path.exists(path):
            continue
        text = open(path).read()
        if path.endswith('.json'):
            keys.update({k: int(v) for k, v in json.loads(text).items()})
        else:
            keys.update({m.group(1): int(m.group(2))
                         for m in re.finditer(r'MESSAGE_KEY_(\w+)\s*=\s*(\d+)', text)})
    return keys


def resolve_messages(project, items):
    keys = message_keys(project)
    out = []
    for item in items:
        if '=' not in item:
            sys.exit('render: --message expects KEY=VALUE, got %r' % item)
        name, value = item.split('=', 1)
        if ';' in value:
            sys.exit('render: --message values cannot contain ";"')
        if name.isdigit():
            number = int(name)
        elif name in keys:
            number = keys[name]
        else:
            sys.exit('render: unknown message key %r (known: %s)' %
                     (name, ', '.join(sorted(keys)) or 'none; run `pebble build`'))
        out.append('%d=%s' % (number, value))
    return ';'.join(out)


def tz_spec(tz, epoch):
    """The watch's time zone the way the phone gives it to the firmware: the standard UTC
    offset, the daylight-saving window around `epoch` (UTC timestamps, 0 if none) and the
    zone's abbreviation at `epoch`, as "gmtoff,dst_start,dst_end,ABBR"."""
    from datetime import datetime, timedelta
    from zoneinfo import ZoneInfo
    zone = ZoneInfo(tz)

    def dst_on(t):
        return bool(datetime.fromtimestamp(t, zone).dst())

    now = datetime.fromtimestamp(epoch, zone)
    std = int((now.utcoffset() - (now.dst() or timedelta(0))).total_seconds())
    # Daylight-saving transitions within 400 days either side, found hourly, then to the second.
    transitions, t = [], epoch - 400 * 86400
    prev = dst_on(t)
    while t < epoch + 400 * 86400:
        cur = dst_on(t + 3600)
        if cur != prev:
            lo, hi = t, t + 3600
            while hi - lo > 1:
                mid = (lo + hi) // 2
                lo, hi = (lo, mid) if dst_on(mid) == cur else (mid, hi)
            transitions.append((hi, cur))
            prev = cur
        t += 3600
    windows, start = [], None
    for when, on in transitions:
        if on:
            start = when
        elif start is not None:
            windows.append((start, when))
            start = None
    window = next(((a, b) for a, b in windows if a <= epoch < b), None) or \
        next(((a, b) for a, b in windows if a > epoch), (0, 0))
    return '%d,%d,%d,%s' % (std, window[0], window[1], now.tzname()[:5])


def epoch_for(date, clock, tz):
    """Seconds since 1970 for a wall-clock date and time in time zone `tz`."""
    fmt = '%Y-%m-%d %H:%M:%S' if clock.count(':') == 2 else '%Y-%m-%d %H:%M'
    try:
        t = time.strptime('%s %s' % (date, clock), fmt)
    except ValueError:
        sys.exit('render: --date must be YYYY-MM-DD and --time HH:MM or HH:MM:SS')
    if tz == 'UTC':
        return calendar.timegm(t)
    saved = os.environ.get('TZ')
    os.environ['TZ'] = tz
    time.tzset()
    try:
        return int(time.mktime(t[:8] + (-1,)))
    finally:
        if saved is None:
            del os.environ['TZ']
        else:
            os.environ['TZ'] = saved
        time.tzset()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('project', help='Pebble project directory (after `pebble build`)')
    ap.add_argument('output', help='OUT.png')
    scene = ap.add_argument_group('scene (ordinary apps)')
    scene.add_argument('--time', default='10:09')
    scene.add_argument('--date', default='2026-10-17')
    scene.add_argument('--tz', default='UTC')
    scene.add_argument('--12h', dest='twelve', action='store_true')
    scene.add_argument('--battery', type=int, default=80)
    scene.add_argument('--charging', action='store_true')
    scene.add_argument('--no-phone', action='store_true')
    scene.add_argument('--steps', type=int, default=0)
    scene.add_argument('--hr', type=int, default=0)
    scene.add_argument('--run-ms', type=int, default=0)
    scene.add_argument('--no-tick', action='store_true')
    scene.add_argument('--message', action='append', default=[])
    lab = ap.add_argument_group('Watchface Lab projects')
    lab.add_argument('--scenario', type=int, default=int(os.environ.get('LAB_SCENARIO', '0')))
    lab.add_argument('--taps', type=int, default=0)
    lab.add_argument('--detail', action='store_true')
    out_group = ap.add_argument_group('output')
    out_group.add_argument('--scale', type=int, default=3)
    out_group.add_argument('--raw')
    out_group.add_argument('--no-correction', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    project = os.path.abspath(a.project)
    if not os.path.exists(os.path.join(project, 'package.json')):
        sys.exit('render: %s is not a Pebble project (no package.json)' % project)
    pack = os.path.join(project, 'build', 'emery', 'app_resources.pbpack')
    if not os.path.exists(pack):
        sys.exit('render: no %s - run `pebble build` first (emery must be a target platform)'
                 % pack)
    stale = stale_resources(project)
    if stale:
        sys.exit('render: %s; run `pebble build` first' % stale)
    if not os.path.exists(os.path.join(FW_DIR, 'system_resources.pbpack')):
        sys.exit('render: missing %s/system_resources.pbpack - run setup.sh first' % FW_DIR)
    check_firmware()

    mode = build.project_mode(project)
    if mode == 'lab':
        if a.scenario not in (0, 1, 2):
            sys.exit('render: scenario %d is not supported (0 demo, 1 stress, 2 empty)'
                     % a.scenario)
        scenario = a.scenario
    else:
        if a.detail:
            sys.exit('render: --detail is for Watchface Lab projects')
        scenario = 0

    started = time.time()
    os.makedirs(build.BUILD, exist_ok=True)
    try:
        # The shared firmware objects are checked/compiled under a machine-wide lock (a
        # no-op once built); each project then builds under its own lock, so renders of
        # different projects never wait for each other.
        with open(os.path.join(build.BUILD, '.fw.lock'), 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            fw_objs = build.fw_objects()
        app_dir = build.app_build_dir(project, scenario)
        os.makedirs(app_dir, exist_ok=True)
        with open(app_dir + '.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            exe = build.build_project(project, scenario, fw_objs)
    except build.BuildError as e:
        sys.exit('render: build failed\n%s' % e)

    out = os.path.abspath(a.output)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        raw_path = os.path.join(tmp, 'frame.raw')
        cmd = [exe, FW_DIR, pack, str(a.taps), raw_path]
        if mode == 'app':
            if a.tz != 'UTC' and not os.path.exists(os.path.join('/usr/share/zoneinfo', a.tz)):
                sys.exit('render: unknown time zone %r (use an IANA name such as Europe/London)'
                         % a.tz)
            epoch = epoch_for(a.date, a.time, a.tz)
            cmd += ['--epoch', str(epoch), '--tz-info', tz_spec(a.tz, epoch) if a.tz != 'UTC'
                    else '0,0,0,UTC', '--battery', str(a.battery),
                    '--steps', str(a.steps), '--hr', str(a.hr), '--run-ms', str(a.run_ms)]
            cmd += ['--12h'] if a.twelve else []
            cmd += ['--charging'] if a.charging else []
            cmd += ['--no-phone'] if a.no_phone else []
            cmd += ['--no-tick'] if a.no_tick else []
            if a.message:
                cmd += ['--message', resolve_messages(project, a.message)]
        if a.verbose:
            cmd.append('-v')
        if a.detail:
            cmd.append('--detail')
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                               env=dict(os.environ, TZ='UTC'))
        except subprocess.TimeoutExpired:
            sys.exit('render: the app did not finish within 120 s')
        if r.stderr:
            sys.stderr.write(r.stderr)
        if r.returncode == 6:
            sys.exit('render: this design has no detail view (LabDesign.draw_detail is NULL)')
        if r.returncode != 0:
            reason = ('crashed (signal %d)' % -r.returncode) if r.returncode < 0 else \
                     ('exited with %d' % r.returncode)
            sys.exit('render: the app %s. If it isn\'t an unsupported call reported above, '
                     'check it on the emulator: it may crash on the watch too.' % reason)
        raw = open(raw_path, 'rb').read()
    if a.raw:
        with open(a.raw, 'wb') as f:
            f.write(raw)

    img = screenshot.framebuffer_to_image(raw, correct=not a.no_correction)
    img.save(out)
    if a.scale > 1:
        root, ext = os.path.splitext(out)
        img.resize((img.width * a.scale, img.height * a.scale), 0).save(
            '%s_%dx%s' % (root, a.scale, ext))
    label = r.stdout.strip() if mode == 'lab' else os.path.basename(project)
    detail = ' detail' if a.detail else ''
    where = ('scenario %d' % scenario) if mode == 'lab' else '%s %s' % (a.date, a.time)
    print('ok %s (%dx%d, %s%s, %s, %.1fs)'
          % (out, img.width, img.height, label, detail, where, time.time() - started))


if __name__ == '__main__':
    main()
