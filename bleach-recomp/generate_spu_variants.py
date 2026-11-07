"""Build AVX2 copies of lifted SPU images without changing the original lift.

Only the copy's symbols are renamed. Registration selects a complete image,
including its indirect-branch table, once before any guest threads start.
"""
import argparse
from pathlib import Path
import re
import json
import sys
from generate_native_kernels import promote, VALUE_OPS

def install_skin4(source):
    """Replace only the bulk of the proven four-influence loop; retain its tail.

    The original reference and all surrounding phase/publication code stay
    intact. Each duplicate continuation containing the same loop is covered.
    """
    pattern=re.compile(r'^static void (\w+_loop_region_\d+)_native\(spu_context\* ctx\) \{\n(.*?)^}',re.M|re.S)
    found=[]
    def patch(m):
        name,body=m.groups()
        if 'loc_00009FB8:' not in body:return m[0]
        found.append(name)
        return f'static void {name}_native(spu_context* ctx) {{\n'+(
            '    const unsigned bulk=bleach_skin4_bulk(ctx);\n'
            '    if (bleach_native_skinning_mode) bleach_native_skinning_record(bulk,bleach_skin4_fast);\n')+body+'}'
    source=pattern.sub(patch,source)
    if not found:raise ValueError('Four-influence skinning loop missing')
    for name in found:
        marker=f'    if (checks < bleach_kernel_verify_limit()) {{\n        bleach_kernel_verify(ctx,{name}_native,'
        assert source.count(marker)==1
        guard=('    static SPU_THREAD_LOCAL unsigned skin_calls,skin_checks;\n'
            '    if (bleach_native_skinning_mode && bleach_native_skinning_stride &&\n'
            '        ++skin_calls % bleach_native_skinning_stride == 0) {\n'
            f'        bleach_kernel_verify(ctx,{name}_native,{name}_reference,"typed-skin4",++skin_checks);\n'
            '        bleach_native_skinning_verified();return;\n    }\n')
        source=source.replace(marker,guard+marker)
    return source,found


def instrument_geometry_phases(source):
    """Observe complete top-level calls, including their drained continuations.

    All duplicate lifted bodies must receive the same call-site observer.
    This never replaces a guest call or infers semantics from its address.
    """
    sites = ((0x358,0x1DA8),(0x3D8,0x1148),(0x408,0x9828),
             (0x418,0x7F58),(0x480,0x98E0),(0x490,0x65D0),
             (0x574,0x5110),(0x590,0x1A58),(0x598,0xB08),
             (0x5B0,0x2408),(0x5E0,0x2120),(0x5E8,0x4190),
             (0x5F8,0x2270),(0x610,0x2710),(0x620,0x16A0),
             (0x508,0x2710),(0x520,0x2A00))
    for index,(ret,target) in enumerate(sites):
        pattern = re.compile(r'^([ \t]*)(ctx->gpr\[0\] = spu_link\(0x'+f'{ret:X}'+
            r'\); \{[^\n]+job_de1065fcb7151ac3_spu_func_'+f'{target:08X}'+
            r'\(ctx\); spu_drain_call\(ctx, 0x'+f'{ret:X}'+r'\);[^\n]+\})$',re.M)
        def wrap(m):
            call=f'if (bleach_geometry_phase_observer) bleach_geometry_phase_observer(ctx,{index},0x{target:X},0x{ret:X},'
            body=m[0]
            # The resident adapter already measures decode/blend/pose. Leave
            # those boundaries untouched so cache substitution stays identical.
            if ret not in (0x408,0x418,0x480):
                body=(m[1]+'{ const uint64_t phase_trace=ps3_geo_trace_begin();\n'
                      +body+'\n'+m[1]+f'ps3_geo_trace_end(120,0x{target:X},0x{ret:X},phase_trace); }}')
            return m[1]+call+'0);\n'+body+'\n'+m[1]+call+'1);'
        source,count=pattern.subn(wrap,source)
        if not count:
            raise ValueError(f'Missing geometry phase call site {ret:x}/{target:x}')
    return source


def write_changed(path: Path, text: str):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")

def install_geometry_decode(source):
    # Replace the complete data-decoding stage at its top-level consumer.
    # Keep all other entry points and unsupported formats on the original path.
    target='job_de1065fcb7151ac3_spu_func_00009828'
    pattern=re.compile(r'^(\s*)(ctx->gpr\[0\] = spu_link\(0x408\); \{[^\n]+'+target+r'\(ctx\);[^\n]+\})$',re.M)
    source,count=pattern.subn(lambda m:m[1]+f'if (!bleach_geometry_decode_try(ctx,{target})) {{ '+m[2]+' }',source)
    if not count:raise ValueError('Missing complete geometry-decode call site')
    header=Path(__file__).with_name('native_geometry_decode_runtime.h').resolve().as_posix()
    return '#include "'+header+'"\n'+source,count

def install_geometry_pose(source):
    # Replace the complete pose stage at its top-level consumer.
    # Keep all other entry points and unsupported formats on the original path.
    target='job_de1065fcb7151ac3_spu_func_000098E0'
    pattern=re.compile(r'^(\s*)(ctx->gpr\[0\] = spu_link\(0x480\); \{[^\n]+'+target+r'\(ctx\);[^\n]+\})$',re.M)
    source,count=pattern.subn(lambda m:m[1]+f'if (!bleach_geometry_pose_try(ctx,{target})) {{ '+m[2]+' }',source)
    if not count:raise ValueError('Missing complete geometry-pose call site')
    header=Path(__file__).with_name('native_geometry_pose_runtime.h').resolve().as_posix()
    return '#include "'+header+'"\n'+source,count



def install_geometry_triangles(source):
    # Replace the complete triangle stage at its top-level consumer.
    # Keep all other entry points and unsupported formats on the original path.
    target='job_de1065fcb7151ac3_spu_func_00005110'
    pattern=re.compile(r'^(\s*)(ctx->gpr\[0\] = spu_link\(0x574\); \{[^\n]+'+target+r'\(ctx\);[^\n]+\})$',re.M)
    source,count=pattern.subn(lambda m:m[1]+f'if (!bleach_geometry_handoff_capture(ctx) && !bleach_geometry_triangle_try(ctx,{target})) {{ '+m[2]+' }',source)
    if not count:raise ValueError('Missing complete geometry-triangle call site')
    header=Path(__file__).with_name('native_geometry_triangle_runtime.h').resolve().as_posix()
    handoff=Path(__file__).with_name('native_geometry_handoff_runtime.h').resolve().as_posix()
    indices=re.compile(r'^([ \t]*)(ctx->gpr\[0\] = spu_link\(0x5E0\); \{[^\n]+job_de1065fcb7151ac3_spu_func_00002120\(ctx\);[^\n]+\})$',re.M)
    source,n=indices.subn(lambda m:m[1]+'bleach_geometry_handoff_indices(ctx);\n'+m[0],source)
    if not n:raise ValueError('Missing index publication')
    for ret in (0x520,0x978):
        publish=re.compile(r'^([ \t]*)(ctx->gpr\[0\] = spu_link\(0x'+f'{ret:X}'+r'\); \{[^\n]+job_de1065fcb7151ac3_spu_func_00002A00\(ctx\);[^\n]+\})$',re.M)
        source,n=publish.subn(lambda m:m[1]+f'bleach_geometry_handoff_publish(ctx,{int(ret==0x978)});\n'+m[0],source)
        if not n:raise ValueError(f'Missing geometry publication {ret:x}')
    return '#include "'+header+'"\n#include "'+handoff+'"\n'+source,count



def install_geometry_resident(source,raw):
    # Retain the entire decode -> pose value, independently of view/culling.
    # Helpers preserve the original inter-stage ABI; only pure data is retained.
    reference=re.findall(r'^\s*ctx->gpr\[0\] = spu_link\(0x408\);[^\n]*\n.*?^[ \t]*ctx->gpr\[0\] = spu_link\(0x480\);[^\n]*$',raw,re.M|re.S)
    if not reference:raise ValueError('Missing decode/pose pipeline')
    original=reference[0].strip()
    if any(x.strip()!=original for x in reference):raise ValueError('Pipeline variants need explicit contracts')
    between='\n'.join(original.splitlines()[1:-1])
    pattern=re.compile(r'^[ \t]*if \(!bleach_geometry_decode_try[^\n]*\n.*?^[ \t]*if \(!bleach_geometry_pose_try[^\n]*$',re.M|re.S)
    blocks=pattern.findall(source)
    if not blocks:raise ValueError('Missing native geometry pipeline')
    helpers=('\nstatic void bleach_resident_reference(spu_context* ctx){\n'+original+'\n}\n'
        +'static void bleach_resident_between(spu_context* ctx){\n'+between+'\n}\n')
    declarations='static void bleach_resident_reference(spu_context*);\nstatic void bleach_resident_between(spu_context*);\n'
    unique=list(dict.fromkeys(blocks))
    for i,block in enumerate(unique):
        declarations+=f'static void bleach_resident_run_{i}(spu_context*);\n'
        helpers+=f'static void bleach_resident_run_{i}(spu_context* ctx){{\n'+block+'\n}\n'
    def replace(m):
        i=unique.index(m[0])
        return (f'        if (!bleach_geometry_resident_try(ctx,bleach_resident_run_{i},bleach_resident_between,bleach_resident_reference))\n'
            +f'            bleach_resident_run_{i}(ctx);')
    source=pattern.sub(replace,source)
    marker='#include "spu_helpers.h"\n'
    if source.count(marker)!=1:raise ValueError('Missing helper insertion point')
    source=source.replace(marker,marker+declarations,1)+helpers
    header=Path(__file__).with_name('native_geometry_resident_runtime.h').resolve().as_posix()
    return '#include "'+header+'"\n'+source

def generate(source: Path, output: Path, sdk_tools: Path, register_writeback=False):
    sys.path.insert(0,str(sdk_tools))
    from spu_native_pairs import pack_source
    from spu_native_phases import closed_phase, result_phase, FUNCTION
    from spu_native_state import schedule_writeback
    output.mkdir(parents=True, exist_ok=True)
    registry = (source / "spu_workloads.c").read_text(encoding="utf-8")
    symbols = {}
    fma_symbols = {}
    pair_symbols = {}
    for lifted in sorted(source.glob("*/spu_recomp.c")):
        prefix = lifted.parent.name
        names = sorted(set(re.findall(r"\b" + re.escape(prefix) + r"_spu_\w+", lifted.read_text(encoding="utf-8"))))
        renames = {name: name.replace(prefix + "_", prefix + "_avx2_", 1) for name in names}
        symbols.update(renames)
        wrapper = "/* Generated AVX2 translation unit; the original lift remains unchanged. */\n"
        wrapper += "".join(f"#define {old} {new}\n" for old, new in renames.items())
        if prefix == "job_de1065fcb7151ac3":
            native_header = Path(__file__).with_name("native_spu_conversions.h")
            wrapper += '#include "spu_helpers.h"\n'
            wrapper += f'#include "{native_header.resolve().as_posix()}"\n'
            wrapper += '#define spu_csflt bleach_spu_csflt_avx2\n'
            wrapper += '#define spu_cuflt bleach_spu_cuflt_avx2\n'
            integer_header = Path(__file__).with_name('native_spu_integer.h')
            wrapper += f'#include "{integer_header.resolve().as_posix()}"\n'
            wrapper += ''.join(f'#define spu_{op} bleach_int_{op}\n' for op in (
                'a', 'sf', 'and', 'or', 'xor', 'nor', 'andc', 'ceq', 'cgt', 'clgt',
                'ai', 'sfi', 'andi', 'ori', 'xori', 'ceqi', 'cgti', 'clgti', 'selb',
                'shufb', 'shlqbyi', 'rotqbyi', 'shli', 'rotmi', 'rotmai', 'roti'))
            kernel_header = Path(__file__).with_name("native_spu_kernels.h")
            wrapper += f'#include "{kernel_header.resolve().as_posix()}"\n'
            raw=lifted.read_text(encoding="utf-8")
            promoted, selected = promote(raw, state_scheduler=schedule_writeback if register_writeback else None)
            entry=prefix+'_spu_func_000098E0'
            closure,phase_report=closed_phase(raw,entry,prefix+'_spu_func_00009908',
                {prefix+'_spu_func_'+n for n in ('00001148','00001160','00001188','00001190','000011A0','00001AE8')},
                {prefix+'_spu_func_0000AA40'},value_ops=VALUE_OPS)
            result_code,result_report=result_phase(raw,entry,prefix+'_spu_func_00009908',
                {prefix+'_spu_func_'+n for n in ('00001148','00001160','00001188','00001190','000011A0','00001AE8')},
                {prefix+'_spu_func_0000AA40'},value_ops=VALUE_OPS,
                start_guard='pc=0x09974',end_label='loc_0000A86C',guarded_opaque=True)
            phase_report['owned_result']=result_report
            def inject_phase(m):
                name,body=m.groups()
                if name!=entry:return m[0]
                fallback=entry+'_phase_reference_entry'
                reference=entry+'_phase_reference'
                return (f'static void {fallback}(spu_context* ctx) {{\n{body}\n}}\n'
                    +f'static void {reference}(spu_context* ctx) {{\n'
                    +f' uint32_t ret=ctx->gpr[0]._u32[0]; {fallback}(ctx); spu_drain_call(ctx,ret);\n}}\n'
                    +closure+result_code+f'void {entry}(spu_context* ctx) {{\n'
                    +f' if (!bleach_phase_enabled(ctx)) {{ {fallback}(ctx); return; }}\n'
                    +f' void(*run)(spu_context*)=bleach_native_phase_mode==2?{entry}_result_native:{entry}_phase_native;\n'
                    +' static SPU_THREAD_LOCAL unsigned calls,checks;\n'
                    +' if (bleach_native_phase_stride && ++calls % bleach_native_phase_stride == 0) {\n'
                    +f'  bleach_kernel_verify(ctx,run,{reference},"closed-phase-98E0",++checks);\n'
                    +'  bleach_native_phase_verified();\n'
                    +' } else run(ctx);\n}')
            promoted=FUNCTION.sub(inject_phase,promoted)
            promoted,skin_loops=install_skin4(promoted)
            phase_report['typed_skin4_loop_adapters']=skin_loops
            skin_header=Path(__file__).with_name('native_skinning.h').resolve().as_posix()
            promoted=f'#include "{skin_header}"\n'+promoted
            write_changed(output/'native_phase_report.json',json.dumps(phase_report,indent=2)+'\n')
            promoted = instrument_geometry_phases(promoted)
            promoted,decode_sites=install_geometry_decode(promoted)
            promoted,pose_sites=install_geometry_pose(promoted)
            promoted,triangle_sites=install_geometry_triangles(promoted)
            promoted=install_geometry_resident(promoted,raw)
            # Resolve the generated source's private header before moving it.
            promoted = promoted.replace('#include "spu_recomp.h"',
                f'#include "{lifted.with_suffix(".h").resolve().as_posix()}"')
            promoted_path = output / f"{prefix}_kernels.inc"
            write_changed(promoted_path, promoted)
            write_changed(output / "native_kernel_list.txt", "\n".join(selected)+"\n")
            wrapper += f'#include "{promoted_path.resolve().as_posix()}"\n'
        else:
            wrapper += f'#include "{lifted.resolve().as_posix()}"\n'
        write_changed(output / f"{prefix}_avx2.c", wrapper)
        if prefix == "job_de1065fcb7151ac3":
            fma_symbols = {name: new.replace('_avx2_', '_fma_', 1) for name,new in renames.items()}
            fma_wrapper = wrapper.replace(prefix+'_avx2_',prefix+'_fma_')
            float_header=Path(__file__).with_name('native_spu_float.h')
            definitions=f'#include "{float_header.resolve().as_posix()}"\n'
            definitions+=''.join(f'#define spu_{op} bleach_fp_{op}\n'
                                 for op in ('fa','fs','fm','fma','fms','fnms'))
            fma_wrapper=fma_wrapper.replace(f'#include "{promoted_path.resolve().as_posix()}"',
                definitions+f'#include "{promoted_path.resolve().as_posix()}"')
            write_changed(output/f'{prefix}_fma.c',fma_wrapper)
            pair_symbols={name:new.replace('_fma_','_fma2_',1) for name,new in fma_symbols.items()}
            packed,report=pack_source(promoted,VALUE_OPS|{'bleach_kernel_load'})
            packed_path=output/f'{prefix}_pairs.inc'
            write_changed(packed_path,packed)
            write_changed(output/'native_pair_report.json',json.dumps(report,indent=2)+'\n')
            paired_wrapper=fma_wrapper.replace(prefix+'_fma_',prefix+'_fma2_')
            paired_wrapper=paired_wrapper.replace(f'#include "{promoted_path.resolve().as_posix()}"',
                '#include "spu_native_pairs_x86.h"\n'+f'#include "{packed_path.resolve().as_posix()}"')
            write_changed(output/f'{prefix}_pairs_fma.c',paired_wrapper)

    # Keep declarations intact; select both direct entry and indirect table.
    head, body = registry.split("void bleach_spu_register(void)", 1)
    declarations = re.findall(r"extern void (\w+)(\([^;]+;)", head)
    head += "\n" + "\n".join(f"extern void {symbols[name]}{tail}" for name, tail in declarations if name in symbols) + "\n"
    head += "\n" + "\n".join(f"extern void {fma_symbols[name]}{tail}" for name, tail in declarations if name in fma_symbols) + "\n"
    head += "\n" + "\n".join(f"extern void {pair_symbols[name]}{tail}" for name, tail in declarations if name in pair_symbols) + "\n"
    head += r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int bleach_native_geometry_fp;
static int bleach_native_vector_pairs;
static int bleach_spu_select_avx2(void)
{
    /* This TU is compiled for baseline x86-64. The compiler's CPU detector
     * includes OS support for saving the extended vector register state. */
    __builtin_cpu_init();
    const int supported = !!__builtin_cpu_supports("avx2");
    const char* mode = getenv("BLEACH_SPU_ISA");
    const int baseline = mode && !strcmp(mode, "baseline");
    const char* native_fp=getenv("BLEACH_NATIVE_FLOAT");
    bleach_native_geometry_fp=supported && !baseline && __builtin_cpu_supports("fma") &&
        (!native_fp || strcmp(native_fp,"0"));
    const char* pairs=getenv("SPU_NATIVE_VECTOR_PAIRS");
    bleach_native_vector_pairs=bleach_native_geometry_fp && pairs && !strcmp(pairs,"1");
    fprintf(stderr,"[spu-isa] native_vector_pairs=%d\n",bleach_native_vector_pairs);
    fprintf(stderr,"[spu-isa] native_geometry_float=%d\n",bleach_native_geometry_fp);
    if (mode && strcmp(mode, "auto") && strcmp(mode, "baseline") && strcmp(mode, "avx2"))
        fprintf(stderr, "[spu-isa] unknown BLEACH_SPU_ISA=%s; using auto\n", mode);
    if (mode && !strcmp(mode, "avx2") && !supported)
        fprintf(stderr, "[spu-isa] AVX2 unavailable; keeping baseline\n");
    fprintf(stderr, "[spu-isa] selected=%s avx2_supported=%d\n",
            supported && !baseline ? "avx2" : "baseline", supported);
    return supported && !baseline;
}
'''
    def selection(m):
        name=m[0]
        avx=f"(bleach_avx2 ? {symbols[name]} : {name})"
        if name not in fma_symbols:return avx
        fp=f"(bleach_native_geometry_fp ? {fma_symbols[name]} : {avx})"
        return f"(bleach_native_vector_pairs ? {pair_symbols[name]} : {fp})"
    body = re.sub(r"\b(?:" + "|".join(map(re.escape, symbols)) + r")\b", selection, body)
    body = body.replace("{", "{\n    const int bleach_avx2 = bleach_spu_select_avx2();", 1)
    write_changed(output / "spu_workloads_dispatch.c", head + "void bleach_spu_register(void)" + body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk-tools",type=Path,default=Path(__file__).resolve().parent.parent/'ps3recomp/tools')
    parser.add_argument('--register-writeback', action='store_true')
    args = parser.parse_args()
    generate(args.source, args.output, args.sdk_tools, args.register_writeback)
