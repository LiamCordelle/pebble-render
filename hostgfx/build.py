"""Build the host renderer: PebbleOS applib code compiled for the host, plus a project.

The firmware side (src/pebbleos at the tag of the firmware inside the SDK's emery QEMU
image) is compiled once into build/fw: the graphics stack, fonts and text, the UI layer
code (windows, layers, text and bitmap layers) and the dictionary code AppMessage uses.

Each project is compiled with the SDK's emery pebble.h, the same defines `pebble build`
uses, and SDK function names mapped to their firmware implementations
(gen/sdk_rename.h), then linked with the firmware objects into one small executable.

Two kinds of project:
- app: any Pebble app. Its main() runs for real; hostgfx/os_host.c stands in for the
  OS (window stack, event loop, timers, services) and renders the top window.
- lab: a Watchface Lab project (src/c/lab_designs.h). lab_glue.c calls the design's
  draw function directly, with the lab's demo/stress/empty scenario data.
"""
import concurrent.futures
import glob
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RENDERER = os.path.dirname(HERE)
PEBBLEOS = os.path.join(RENDERER, 'src', 'pebbleos')
FREERTOS = os.path.join(RENDERER, 'src', 'FreeRTOS-Kernel', 'FreeRTOS', 'Source')
TINYMT = os.path.join(RENDERER, 'src', 'TinyMT', 'tinymt')
GEN = os.path.join(HERE, 'gen')
BUILD = os.path.join(RENDERER, 'build')
FW_BUILD = os.path.join(BUILD, 'fw')
APP_BUILD = os.path.join(BUILD, 'apps')

def find_sdk():
    """The installed Pebble SDK: $PEBBLE_SDK, else the macOS or Linux default location."""
    candidates = [os.environ.get('PEBBLE_SDK', ''),
                  '~/Library/Application Support/Pebble SDK/SDKs/current',
                  '~/.pebble-sdk/SDKs/current']
    for c in candidates:
        if c and os.path.isdir(os.path.expanduser(c)):
            return os.path.expanduser(c)
    return os.path.expanduser(candidates[1])


SDK = find_sdk()
SDK_INCLUDE = os.path.join(SDK, 'sdk-core', 'pebble', 'emery', 'include')

CC = 'clang'

FW_SOURCES = [
    'src/fw/applib/applib_resource.c',
    'src/fw/applib/fonts/codepoint.c',
    'src/fw/applib/fonts/fonts.c',
    'src/fw/applib/graphics/8_bit/bitblt_private.c',
    'src/fw/applib/graphics/8_bit/framebuffer.c',
    'src/fw/applib/graphics/arabic_shaping.c',
    'src/fw/applib/graphics/bitblt.c',
    'src/fw/applib/graphics/framebuffer.c',
    'src/fw/applib/graphics/gbitmap.c',
    'src/fw/applib/graphics/gbitmap_png.c',
    'src/fw/applib/graphics/gbitmap_sequence.c',
    'src/fw/applib/graphics/gcolor_definitions.c',
    'src/fw/applib/graphics/gdraw_command.c',
    'src/fw/applib/graphics/gdraw_command_frame.c',
    'src/fw/applib/graphics/gdraw_command_image.c',
    'src/fw/applib/graphics/gdraw_command_list.c',
    'src/fw/applib/graphics/gdraw_command_sequence.c',
    'src/fw/applib/graphics/gpath.c',
    'src/fw/applib/graphics/gpath_builder.c',
    'src/fw/applib/graphics/graphics.c',
    'src/fw/applib/graphics/graphics_bitmap.c',
    'src/fw/applib/graphics/graphics_circle.c',
    'src/fw/applib/graphics/graphics_line.c',
    'src/fw/applib/graphics/graphics_private.c',
    'src/fw/applib/graphics/graphics_private_raw.c',
    'src/fw/applib/graphics/gtransform.c',
    'src/fw/applib/graphics/gtypes.c',
    'src/fw/applib/graphics/perimeter.c',
    'src/fw/applib/graphics/rtl_support.c',
    'src/fw/applib/graphics/text_layout.c',
    'src/fw/applib/graphics/text_render.c',
    'src/fw/applib/graphics/text_resources.c',
    'src/fw/applib/graphics/utf8.c',
    'src/fw/applib/i18n.c',
    'src/fw/applib/pbl_std/locale.c',
    'src/fw/applib/pbl_std/pbl_std.c',
    'src/fw/applib/pbl_std/strftime.c',
    'src/fw/applib/pbl_std/timelocal.c',
    'src/fw/applib/ui/bitmap_layer.c',
    'src/fw/applib/ui/layer.c',
    'src/fw/applib/ui/rotate_bitmap_layer.c',
    'src/fw/applib/ui/text_layer.c',
    'src/fw/applib/ui/text_layer_flow.c',
    'src/fw/applib/ui/window.c',
    'src/fw/util/dict.c',
    'src/fw/applib/vendor/tinflate/tinflate.c',
    'src/fw/applib/vendor/uPNG/upng.c',
    'src/fw/util/rand/rand.c',
    'src/fw/util/time/mktime.c',
    'src/fw/util/time/time.c',
    'lib/util/hash.c',
    'lib/util/iterator.c',
    'lib/util/keyed_circular_cache.c',
    'lib/util/math.c',
    'lib/util/math_fixed.c',
    'lib/util/trig.c',
]

# Host flags chosen to match the ARM build's semantics: unsigned char (ARM EABI), no
# floating-point contraction (the watch never fuses multiply-adds the same way), -O0 so
# signed arithmetic wraps like the generated Thumb code does.
COMMON_FLAGS = ['-O0', '-g', '-funsigned-char', '-ffp-contract=off', '-fno-common',
                '-w', '-D_FORTIFY_SOURCE=0']

FW_FLAGS = COMMON_FLAGS + [
    '-std=c11', '-DUNITTEST', '-D_TIME_H_',
    '-DPLATFORM_NAME="emery"', '-DCONFIG_SCREEN_COLOR_DEPTH_BITS=8',
    '-DCONFIG_PLATFORM_EMERY=1', '-DCONFIG_BOARD_OBELIX=1', '-DCONFIG_FLASH_GD25Q256E=1',
    '-DPLATFORM_DEFAULT=0', '-DMAX_FONT_GLYPH_SIZE=512', '-DCONFIG_LOG=1',
    # text_layout.c only enables screen-shaped text flow outside unit tests; keep the
    # firmware behaviour for graphics_text_attributes_enable_screen_text_flow().
    '-DUSE_DISPLAY_PERIMETER_ON_FONT_LAYOUT',
    '-include', os.path.join(PEBBLEOS, 'src/fw/board/displays/display_qemu_emery.h'),
    '-I', GEN,
    '-I', os.path.join(PEBBLEOS, 'src/fw/util/time'),
] + [x for d in [
    'tests/overrides/dummy_board', 'tests/overrides/default', 'tests/stubs', 'tests/fakes',
    'tests/test_includes', 'tests', 'include', 'subsys', 'src/fw',
    'src/fw/applib/vendor/tinflate', 'src/fw/applib/vendor/uPNG', 'third_party/freertos',
] for x in ('-I', os.path.join(PEBBLEOS, d))] + [
    '-I', os.path.join(FREERTOS, 'include'),
    '-I', os.path.join(FREERTOS, 'portable', 'GCC', 'ARM_CM3'),
    '-I', TINYMT,
]

# Exactly the DEFINES `pebble build` uses for emery (build/c4che/emery_cache.py).
APP_DEFINES = ['RELEASE', 'PBL_PLATFORM_EMERY', 'PBL_COLOR', 'PBL_RECT', 'PBL_MICROPHONE',
               'PBL_SMARTSTRAP', 'PBL_HEALTH', 'PBL_SMARTSTRAP_POWER', 'PBL_COMPASS',
               'PBL_TOUCH', 'PBL_RGB_BACKLIGHT', 'PBL_SPEAKER', 'PBL_DISPLAY_WIDTH=200',
               'PBL_DISPLAY_HEIGHT=228', 'PBL_SDK_3']

# libc functions the SDK exports unrenamed: the host versions behave identically, except
# rand/srand, which come from the firmware (TinyMT) via rand.c.
HOST_LIBC = {'strcmp', 'strncmp', 'strcpy', 'strncpy', 'strcat', 'strncat', 'strlen',
             'memset', 'memmove', 'atol', 'atoi', 'rand', 'srand'}


class BuildError(Exception):
    pass


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise BuildError(' '.join(cmd[:3]) + ' ...\n' + (r.stdout + r.stderr)[-6000:])
    return r


def _compile(src, obj, flags):
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    _run([CC] + flags + ['-c', src, '-o', obj])
    return obj


def _stale(obj, deps):
    if not os.path.exists(obj):
        return True
    t = os.path.getmtime(obj)
    return any(os.path.getmtime(d) > t for d in deps if os.path.exists(d))


def _check_inputs():
    missing = [p for p in (PEBBLEOS, FREERTOS, TINYMT, os.path.join(GEN, 'font_table.h'),
                           SDK_INCLUDE) if not os.path.exists(p)]
    if missing:
        raise BuildError('missing renderer inputs - run `sh setup.sh` once:\n  ' +
                         '\n  '.join(missing))


def fw_objects():
    """Compile the firmware graphics stack + host kernel shim (incremental)."""
    _check_inputs()
    jobs = []
    for rel in FW_SOURCES:
        jobs.append((os.path.join(PEBBLEOS, rel), os.path.join(FW_BUILD, rel[:-2] + '.o')))
    jobs.append((os.path.join(TINYMT, 'tinymt32.c'), os.path.join(FW_BUILD, 'tinymt32.o')))
    for host in ('fw_host.c', 'os_host.c'):
        jobs.append((os.path.join(HERE, host), os.path.join(FW_BUILD, host[:-2] + '.o')))
    header_deps = [os.path.join(HERE, 'build.py'), os.path.join(GEN, 'font_table.h'),
                   os.path.join(HERE, 'host_options.h')]
    todo = [(s, o) for s, o in jobs if _stale(o, [s] + header_deps)]
    if todo:
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            list(ex.map(lambda so: _compile(so[0], so[1], FW_FLAGS), todo))
    return [o for _, o in jobs]


def _defined_symbols(objs):
    out = _run(['nm', '-g', '--defined-only'] + objs).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            syms.add(parts[2].lstrip('_'))
    return syms


def _write_stubs(path, fw_syms):
    """Weak stubs for every SDK function the host build doesn't implement."""
    names = []
    for line in open(os.path.join(GEN, 'sdk_functions.txt')):
        _sdk_name, impl = line.split()
        if impl in fw_syms or impl in HOST_LIBC or impl in names:
            continue
        names.append(impl)
    with open(path, 'w') as f:
        f.write('#include <stdio.h>\n#include <stdlib.h>\n')
        f.write('static void unsupported(const char *name) {\n'
                '  fprintf(stderr, "render: SDK call %s() is not supported by the host '
                'renderer\\n", name);\n  exit(5);\n}\n')
        for n in names:
            f.write('__attribute__((weak)) void %s(void) { unsupported("%s"); }\n' % (n, n))


# Lab projects: lab_main.c is replaced by lab_glue.c; lab_weather.c (merged lab apps only)
# needs AppMessage and live data, which lab mode doesn't reproduce.
LAB_EXCLUDED_SOURCES = {'lab_main.c', 'lab_weather.c'}


def project_mode(project):
    """'lab' for Watchface Lab projects, 'app' for any other Pebble project."""
    return 'lab' if os.path.exists(os.path.join(project, 'src', 'c', 'lab_designs.h')) else 'app'


def project_sources(project, mode):
    srcs = sorted(glob.glob(os.path.join(project, 'src', 'c', '**', '*.c'), recursive=True))
    if mode == 'lab':
        return [s for s in srcs if os.path.basename(s) not in LAB_EXCLUDED_SOURCES]
    # Message keys (MESSAGE_KEY_*) are defined in a file `pebble build` generates.
    srcs += glob.glob(os.path.join(project, 'build', 'src', 'message_keys.auto.c'))
    return srcs


def app_build_dir(project, scenario):
    project = os.path.abspath(project)
    key = hashlib.sha1(('%s|%d' % (project, scenario)).encode()).hexdigest()[:12]
    return os.path.join(APP_BUILD, '%s-s%d-%s' % (os.path.basename(project), scenario, key))


def build_project(project, scenario=0, fw_objs=None):
    """Compile + link the project's renderer. Returns the executable path."""
    project = os.path.abspath(project)
    if fw_objs is None:
        fw_objs = fw_objects()
    mode = project_mode(project)
    srcs = project_sources(project, mode)
    resource_hdr = os.path.join(project, 'build', 'emery', 'src', 'resource_ids.auto.h')
    if not os.path.exists(resource_hdr):
        raise BuildError('no %s - run `pebble build` in the project first' % resource_hdr)

    out_dir = app_build_dir(project, scenario)
    os.makedirs(out_dir, exist_ok=True)

    if mode == 'lab':
        mode_defines = ['-DLAB_SCENARIO=%d' % scenario, '-DLAB_HOST_RENDER=1']
        glue = os.path.join(HERE, 'lab_glue.c')
    else:
        # The app's own main() runs inside the renderer, which has a main() of its own.
        mode_defines = ['-Dmain=pebble_app_main']
        glue = os.path.join(HERE, 'app_glue.c')
    flags = COMMON_FLAGS + ['-std=c99', '-D_TIME_H_', '-Dtime_t=long'] + \
        ['-D' + d for d in APP_DEFINES] + mode_defines + [
        '-include', os.path.join(HERE, 'app_prelude.h'),
        '-I', os.path.join(project, 'src', 'c'),
        '-I', os.path.join(project, 'build', 'emery'),
        '-I', os.path.join(project, 'build', 'include'),
        '-I', SDK_INCLUDE,
    ]
    deps = [resource_hdr, os.path.join(HERE, 'app_prelude.h'), os.path.join(GEN, 'sdk_rename.h'),
            os.path.join(HERE, 'build.py')] + \
        glob.glob(os.path.join(project, 'src', 'c', '**', '*.h'), recursive=True)
    # Object names keep the relative path, so same-named files in subfolders don't clash.
    jobs = [(s, os.path.join(out_dir, os.path.relpath(s, project).replace(os.sep, '__')[:-2] + '.o'))
            for s in srcs]
    jobs.append((glue, os.path.join(out_dir, os.path.basename(glue)[:-2] + '.o')))
    todo = [(s, o) for s, o in jobs if _stale(o, [s] + deps)]
    if todo:
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            list(ex.map(lambda so: _compile(so[0], so[1], flags), todo))
    app_objs = [o for _, o in jobs]

    exe = os.path.join(out_dir, 'render_bin')
    if _stale(exe, app_objs + fw_objs):
        stubs_c = os.path.join(out_dir, 'sdk_stubs.c')
        _write_stubs(stubs_c, _defined_symbols(fw_objs + app_objs))
        stubs_o = _compile(stubs_c, stubs_c[:-2] + '.o', ['-O0', '-w'])
        # Link to a temporary name and rename, so a render already running keeps its binary.
        tmp_exe = exe + '.tmp%d' % os.getpid()
        _run([CC, '-o', tmp_exe] + app_objs + fw_objs + [stubs_o, '-lm'])
        os.replace(tmp_exe, exe)
    return exe
