#!/usr/bin/env python3
"""Extract the firmware data the host renderer needs from the installed Pebble SDK.

  python3 -I hostgfx/setup_fw.py   (setup.sh runs this)

Reads (never modifies) the SDK's Emery QEMU images:
  sdk-core/pebble/emery/qemu/qemu_spi_flash.bin.bz2   system resource pack (fonts...)
  sdk-core/pebble/emery/qemu/emery_sdk_debug.elf       firmware ELF with debug info

Writes (all local, git-ignored, never redistributed):
  fw/system_resources.pbpack      the system pack exactly as stored in SPI flash
  fw/builtin_<id>.bin             resources compiled into the firmware (fallback font)
  hostgfx/gen/font_table.h        font key -> (resource id, extension id), from the ELF
  hostgfx/gen/resource/resource_ids.auto.h   the firmware's ResourceId enum
  hostgfx/gen/sdk_rename.h        SDK function name -> firmware implementation name
"""
import bz2
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RENDERER = os.path.dirname(HERE)
FW_OUT = os.path.join(RENDERER, 'fw')
GEN = os.path.join(HERE, 'gen')
PEBBLEOS = os.path.join(RENDERER, 'src', 'pebbleos')

sys.path.insert(0, HERE)
from build import SDK  # noqa: E402  (same lookup: $PEBBLE_SDK, then the default locations)
QEMU_DIR = os.path.join(SDK, 'sdk-core', 'pebble', 'emery', 'qemu')
ELF = os.path.join(QEMU_DIR, 'emery_sdk_debug.elf')
SPI_BZ2 = os.path.join(QEMU_DIR, 'qemu_spi_flash.bin.bz2')
GDB = os.path.join(SDK, 'toolchain', 'arm-none-eabi', 'bin', 'arm-none-eabi-gdb')

SYSTEM_STORE_ENTRIES = 768      # MAX_RESOURCES_FOR_SYSTEM_STORE
MANIFEST = 12                   # num_resources, crc, timestamp


def gdb(*commands):
    args = [GDB, '-batch', '-nx']
    for c in commands:
        args += ['-ex', c]
    out = subprocess.run(args + [ELF], capture_output=True, text=True, timeout=300)
    return out.stdout


def gdb_script(text):
    path = os.path.join(GEN, '_tmp.gdb')
    with open(path, 'w') as f:
        f.write('set pagination off\n' + text)
    try:
        out = subprocess.run([GDB, '-batch', '-nx', '-x', path, ELF],
                             capture_output=True, text=True, timeout=300)
    finally:
        os.remove(path)
    return out.stdout


def extract_system_pack():
    flash = bz2.decompress(open(SPI_BZ2, 'rb').read())
    # Locate the pack via the GOTHIC_09 table entry would be fragile; instead scan for a
    # manifest whose table is sorted, contiguous and fits in flash.
    for base in range(0, len(flash) - 0x4000, 0x1000):
        n, _crc, _ts = struct.unpack_from('<III', flash, base)
        if not 100 < n <= SYSTEM_STORE_ENTRIES:
            continue
        ents = [struct.unpack_from('<IIII', flash, base + MANIFEST + 16 * i) for i in range(n)]
        # Resource ids run 1..n; identical resources may share data, so offsets need not be
        # contiguous or ordered.
        if [e[0] for e in ents] != list(range(1, n + 1)) or ents[0][1] != 0:
            continue
        data0 = base + MANIFEST + SYSTEM_STORE_ENTRIES * 16
        end = data0 + max(e[1] + e[2] for e in ents)
        if end > len(flash):
            continue
        pack = flash[base:end]
        with open(os.path.join(FW_OUT, 'system_resources.pbpack'), 'wb') as f:
            f.write(pack)
        print('system pack: flash offset 0x%x, %d resources, %d bytes' % (base, n, len(pack)))
        return
    sys.exit('system resource pack not found in SPI flash image')


def extract_builtins():
    out = gdb_script('''
set $i = 0
while $i < g_num_builtin_resources
  printf "B %d 0x%x %d\\n", (int)g_builtin_resources[$i].resource_id, (unsigned)g_builtin_resources[$i].address, (int)g_builtin_resources[$i].num_bytes
  set $i = $i + 1
end
''')
    elf = open(ELF, 'rb').read()
    # Map virtual addresses to file offsets with the program headers.
    e_phoff, = struct.unpack_from('<I', elf, 0x1c)
    e_phentsize, e_phnum = struct.unpack_from('<HH', elf, 0x2a)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _pa, p_filesz = struct.unpack_from(
            '<IIIII', elf, e_phoff + i * e_phentsize)
        if p_type == 1 and p_filesz:
            segs.append((p_vaddr, p_offset, p_filesz))
    count = 0
    for line in out.splitlines():
        if not line.startswith('B '):
            continue
        _, rid, addr, size = line.split()
        rid, addr, size = int(rid), int(addr, 16), int(size)
        for vaddr, off, filesz in segs:
            if vaddr <= addr and addr + size <= vaddr + filesz:
                data = elf[off + addr - vaddr: off + addr - vaddr + size]
                with open(os.path.join(FW_OUT, 'builtin_%d.bin' % rid), 'wb') as f:
                    f.write(data)
                count += 1
                break
    print('builtin resources:', count)


def gen_font_table():
    out = gdb_script('''
set $n = sizeof(s_font_resource_keys)/sizeof(s_font_resource_keys[0])
set $i = 0
while $i < $n
  printf "F %s %d %d\\n", s_font_resource_keys[$i].key_name, (int)s_font_resource_keys[$i].resource_id, (int)s_font_resource_keys[$i].extension_id
  set $i = $i + 1
end
''')
    rows = [line.split()[1:] for line in out.splitlines() if line.startswith('F ')]
    if not rows:
        sys.exit('could not read s_font_resource_keys from the ELF')
    with open(os.path.join(GEN, 'font_table.h'), 'w') as f:
        f.write('// Generated by setup_fw.py from the emery QEMU firmware ELF.\n')
        f.write('static const struct { const char *key_name; uint32_t resource_id; '
                'uint32_t extension_id; } s_host_font_keys[] = {\n')
        for key, rid, ext in rows:
            f.write('  {"%s", %s, %s},\n' % (key, rid, ext))
        f.write('};\n')
    print('font keys:', len(rows))


def gen_resource_ids():
    out = gdb('ptype ResourceId')
    body = out[out.index('{') + 1: out.rindex('}')]
    lines, value = [], -1
    for item in body.split(','):
        item = item.strip()
        if '=' in item:
            name, v = [s.strip() for s in item.split('=')]
            value = int(v)
        else:
            name = item
            value += 1
        lines.append('  %s = %d,' % (name, value))
    os.makedirs(os.path.join(GEN, 'resource'), exist_ok=True)
    with open(os.path.join(GEN, 'resource', 'resource_ids.auto.h'), 'w') as f:
        f.write('#pragma once\n// Generated by setup_fw.py from the emery QEMU firmware ELF.\n')
        f.write('typedef enum {\n%s\n} ResourceId;\n' % '\n'.join(lines))
    print('resource ids:', len(lines))


def gen_sdk_rename():
    data = json.load(open(os.path.join(PEBBLEOS, 'tools', 'generate_native_sdk',
                                       'exported_symbols.json')))
    funcs = []

    def walk(node):
        if isinstance(node, dict):
            if node.get('type') == 'function':
                funcs.append(node)
            for v in node.values():
                if isinstance(v, (dict, list)):
                    walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data['exports'])
    names = []
    with open(os.path.join(GEN, 'sdk_rename.h'), 'w') as f:
        f.write('// Generated by setup_fw.py: SDK names -> firmware implementations,\n')
        f.write('// exactly as the app jump table resolves them on the watch.\n#pragma once\n')
        for fn in funcs:
            # skipDefinition only keeps a declaration out of pebble.h (libc functions are
            # declared by the libc headers); the jump table still routes the name to implName.
            if fn.get('removed'):
                continue
            impl = fn.get('implName', fn['name'])
            names.append(fn['name'])
            if impl != fn['name']:
                f.write('#undef %s\n#define %s %s\n' % (fn['name'], fn['name'], impl))
    with open(os.path.join(GEN, 'sdk_functions.txt'), 'w') as f:
        for fn in funcs:
            if not fn.get('removed'):
                f.write('%s %s\n' % (fn['name'], fn.get('implName', fn['name'])))
    print('sdk functions:', len(names))


def record_versions():
    """Remember which firmware the data came from, so render.py can spot SDK updates."""
    elf = open(ELF, 'rb').read()
    versions = re.findall(rb'v\d+\.\d+\.\d+', elf)
    fw_version = max(set(versions), key=versions.count).decode() if versions else '?'
    tag = subprocess.run(['git', '-C', PEBBLEOS, 'describe', '--tags', '--exact-match'],
                         capture_output=True, text=True).stdout.strip() or '?'
    st = os.stat(ELF)
    info = {'sdk_elf': os.path.realpath(ELF), 'sdk_elf_size': st.st_size,
            'sdk_elf_mtime': int(st.st_mtime), 'firmware_version': fw_version,
            'pebbleos_checkout': tag}
    with open(os.path.join(FW_OUT, 'firmware.json'), 'w') as f:
        json.dump(info, f, indent=2)
    print('firmware %s, pebbleos checkout %s' % (fw_version, tag))
    if fw_version != tag:
        print('WARNING: check out pebbleos %s to match the emulator firmware' % fw_version)


def main():
    os.makedirs(FW_OUT, exist_ok=True)
    os.makedirs(GEN, exist_ok=True)
    for p in (ELF, SPI_BZ2, GDB):
        if not os.path.exists(p):
            sys.exit('missing ' + p)
    extract_system_pack()
    extract_builtins()
    gen_font_table()
    gen_resource_ids()
    gen_sdk_rename()
    record_versions()


if __name__ == '__main__':
    main()
