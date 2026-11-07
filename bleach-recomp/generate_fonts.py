"""Lift locally supplied font and display firmware PRXs for the private build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

MODULES = [('libfont', 0x02000000), ('libfontFT', 0x02100000),
           ('libfreetype', 0x02200000), ('libresc', 0x02300000)]

def generate(sdk, firmware, output):
    output.mkdir(parents=True, exist_ok=True)
    tools = sdk / 'tools'
    manifest = []
    declarations, loads = [], []
    for name, base in MODULES:
        source = firmware / (name + '.prx')
        target = output / name
        target.mkdir(exist_ok=True)
        def run(*args):
            subprocess.run([sys.executable, *map(str, args)], check=True)
        run(tools / 'lift_prx.py', source, '--base', hex(base), '--output', target)
        exports = json.loads((target / (name + '_exports.json')).read_text())
        imports = json.loads((target / (name + '_imports.json')).read_text())
        stubs = [{'library': lib, 'nid': nid, 'stub': stub}
                 for lib, entries in imports.items() for nid, stub in entries.items()]
        (target / 'hle-stubs.json').write_text(json.dumps(stubs, indent=2))
        # All executable import trampolines are at the end of the text region;
        # exclude the following export tables and string data from discovery.
        code_end = max(int(s['stub'], 16) + 0x20 for s in stubs)
        seeds_file = target / (name + '_functions.json')
        seeds = json.loads(seeds_file.read_text())
        seeds = [dict(s, end=hex(min(int(s['end'], 16), code_end)))
                 for s in seeds if int(s['start'], 16) < code_end]
        seeds_file.write_text(json.dumps(seeds, indent=2))
        run(tools / 'ppu_lifter.py', target / (name + '_image.bin'), '--raw',
            '--base', hex(base), '--toc', exports['toc'], '--code-end', hex(code_end),
            '--functions', seeds_file, '--hle-stubs', target / 'hle-stubs.json',
            '--symbol-prefix', name + '_', '--chunks', '4', '--jobs', '1', '--output', target)
        entries = [(nid, int(opd, 16)-base) for lib, values in exports['exports'].items()
                   if lib != '!syslib' for nid, opd in values.items()]
        declarations += [f'extern const prx_func_entry {name}_function_table[];',
                         f'extern const uint64_t {name}_function_table_count;',
                         f'static const prx_export {name}_exports[] = {{']
        declarations += [f'    {{{nid}u, 0x{opd:08X}u}},' for nid, opd in entries]
        declarations += ['};']
        image = target / (name + '_image.bin')
        raw = image.read_bytes()
        fnv = 0xcbf29ce484222325
        for b in raw:
            fnv = ((fnv ^ b) * 0x100000001b3) & 0xffffffffffffffff
        loads += [f'    load_font("{name}", 0x{base:08X}u, {len(raw)}u, 0x{fnv:016X}ull,',
                  f'              {name}_function_table, {name}_function_table_count,',
                  f'              {name}_exports, {len(entries)}u);']
        manifest.append({'name': name, 'base': hex(base), 'code_end': hex(code_end),
                         'prx_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                         'image_sha256': hashlib.sha256(raw).hexdigest(),
                         'image_size': len(raw), 'exports': len(entries)})
    registry = '''// Generated from local firmware metadata; images stay outside the source kit.
#include "prx/prx_loader.h"
#include <cstdio>
#include <cstdlib>
#include <string>
extern "C" void ppu_register_function(uint64_t, void (*)(void*));
static void register_font(uint32_t addr, void (*fn)(void*)) { ppu_register_function(addr, fn); }
static void load_font(const char* name, uint32_t base, uint32_t expected_size,
                      uint64_t expected_hash, const prx_func_entry* funcs,
                      uint64_t count, const prx_export* exports, uint32_t export_count)
{
    const char* root = std::getenv("BLEACH_PRX_ROOT");
    if (!root) { std::fprintf(stderr, "[bleach] BLEACH_PRX_ROOT required\\n"); std::exit(80); }
    std::string path = std::string(root) + "/" + name + "/" + name + "_image.bin";
    uint32_t size = 0;
    uint8_t* image = prx_image_load_file(path.c_str(), &size);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i=0; image && i<size; ++i) hash = (hash ^ image[i]) * 0x100000001b3ull;
    if (!image || size != expected_size || hash != expected_hash) {
        std::fprintf(stderr, "[bleach] missing or mismatched firmware image: %s\\n", path.c_str());
        std::exit(80);
    }
    prx_module module = {name, base, image, size, funcs, count, exports, export_count};
    auto result = prx_load_module(&module, register_font);
    std::free(image);
    if (!result.ok) std::exit(80);
}
'''
    registry += '\n'.join(declarations)
    registry += '\nextern "C" void ps3_load_prx_modules(void)\n{\n' + '\n'.join(loads) + '\n}\n'
    (output / 'font_modules.cpp').write_text(registry)
    (output / 'font-manifest.json').write_text(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.sdk.resolve(), args.firmware.resolve(), args.output.resolve())
