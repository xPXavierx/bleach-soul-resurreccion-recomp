"""Keep legacy timing by default; expose an opt-in native frame ownership split."""
from pathlib import Path
import argparse
import re

def patch(directory):
    # The lifter and older kits use both newline conventions. Work in bytes
    # to preserve the original non-UTF8 disassembly comments.
    for filename in ('ppu_recomp_000.cpp','ppu_recomp_037.cpp','ppu_recomp_040.cpp'):
        path=directory/filename
        original=path.read_bytes()
        normalized=original.replace(b'\r\n',b'\n')
        if normalized!=original:path.write_bytes(normalized)
    # Remove the earlier capacity experiment if upgrading an existing lift.
    previous=directory/'ppu_recomp_000.cpp'
    old=previous.read_bytes()
    obsolete=(b'extern "C" uint32_t bleach_native_frame_target(uint32_t);\n'
        b'void func_00018D4C(ppu_context* ctx) {\n        /* BLEACH_NATIVE_FRAME_TARGET */\n'
        b'        ctx->gpr[3]=bleach_native_frame_target((uint32_t)ctx->gpr[3]);')
    clean=old.replace(obsolete,b'void func_00018D4C(ppu_context* ctx) {')
    if clean!=old:previous.write_bytes(clean)
    path=directory/'ppu_recomp_037.cpp'
    source=path.read_bytes()
    source=source.replace(b'/* BLEACH_NATIVE_DRAW_DEADLINE */\n'
        b'extern "C" uint32_t bleach_native_draw_deadline(uint32_t,uint32_t);\n',b'')
    for reg in (31,29):
        experimental=(f'ctx->gpr[0] = bleach_native_draw_deadline('
            f'vm_read32(ctx->gpr[{reg}] + -0x7774), (uint32_t)ctx->gpr[9]);').encode()
        original=f'ctx->gpr[0] = vm_read32(ctx->gpr[{reg}] + -0x7774);'.encode()
        source=source.replace(experimental,original)
    if source!=path.read_bytes():path.write_bytes(source)

    # Observe the task graph at its own virtual dispatch boundary. This keeps
    # diagnostic attribution independent of stale PPC link-register values.
    stages={
        'ppu_recomp_040.cpp':(b'0027B7E0',b'0027B8AC',b'0027BCB0'),
        'ppu_recomp_000.cpp':(b'00011B00',b'00015F80'),
    }
    for filename,names in stages.items():
        path=directory/filename
        source=path.read_bytes()
        if b'/* BLEACH_FRAME_DISPATCH */' not in source:
            declaration=(b'/* BLEACH_FRAME_DISPATCH */\n'
                b'extern "C" void bleach_frame_dispatch(ppu_context*,uint32_t);\n')
            signature=b'void func_'+names[0]+b'(ppu_context* ctx) {'
            if source.count(signature)!=1:raise RuntimeError('Task graph signature changed')
            source=source.replace(signature,declaration+signature)
        for name in names:
            a=source.index(b'void func_'+name+b'(')
            b=source.find(b'\nvoid func_',a+10)
            if b<0:b=len(source)
            body=source[a:b]
            body=re.sub(rb'ctx->lr = (0x[0-9A-F]+); ps3_indirect_call\(ctx\);',
                lambda m:b'ctx->lr = '+m[1]+b'; bleach_frame_dispatch(ctx,'+m[1]+b');',body)
            source=source[:a]+body+source[b:]
        if source!=path.read_bytes():path.write_bytes(source)

    path=directory/'ppu_recomp_000.cpp'
    source=path.read_bytes()
    if b'/* BLEACH_NATIVE_ENGINE_SPLIT */' not in source:
        declaration=(b'/* BLEACH_NATIVE_ENGINE_SPLIT */\n'
            b'extern "C" int bleach_engine_frame_begin(ppu_context*);\n'
            b'extern "C" void bleach_engine_frame_end();\n'
            b'extern "C" uint32_t bleach_engine_submission(uint32_t);\n')
        source=source.replace(b'#include "ppu_recomp.h"\n',b'#include "ppu_recomp.h"\n'+declaration,1)
        a=source.index(b'void func_0001CE58(');b=source.index(b'\nvoid func_',a+10)
        body=source[a:b]
        body=body.replace(b'(ppu_context* ctx) {',b'(ppu_context* ctx) {\n        int native_step=0;',1)
        anchor=b'        ctx->gpr[30] = vm_read32(ctx->gpr[2] + -0x7E84);\n'
        assert body.count(anchor)==1
        body=body.replace(anchor,anchor+
            b'        native_step=bleach_engine_frame_begin(ctx);\n'
            b'        if(native_step) {\n'
            b'            if(native_step==2) vm_write32(ctx->gpr[30]+0x14,vm_read32(ctx->gpr[30]+0x14)+1);\n'
            b'            goto loc_0001CEC0; /* drain original 60Hz semaphore without blocking rendering */\n'
            b'        }\n')
        anchor=b'        vm_write64(ctx->gpr[30] + 0x88, ctx->gpr[3]);\n'
        assert body.count(anchor)==2
        body=body.replace(anchor,anchor+b'        if(native_step==1) goto loc_0001CF28; /* scene prepare, no simulation */\n')
        anchor=b'        ctx->lr = 0x0001CFA8; func_002426E4(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        assert body.count(anchor)==1
        body=body.replace(anchor,anchor+b'        bleach_engine_frame_end();\n')
        source=source[:a]+body+source[b:]
        anchor=b'void func_00018D4C(ppu_context* ctx) {\n'
        assert source.count(anchor)==1
        source=source.replace(anchor,anchor+b'        ctx->gpr[3]=bleach_engine_submission((uint32_t)ctx->gpr[3]);\n')
    if b'/* BLEACH_NATIVE_ENGINE_ACCEPTANCE */' not in source:
        source=source.replace(b'/* BLEACH_NATIVE_ENGINE_SPLIT */',
            b'/* BLEACH_NATIVE_ENGINE_ACCEPTANCE */\nextern "C" void bleach_engine_acceptance(uint32_t);\n/* BLEACH_NATIVE_ENGINE_SPLIT */',1)
        anchor=b'        ctx->lr = 0x00018D5C; func_00248E24(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        assert source.count(anchor)==1
        source=source.replace(anchor,anchor+b'        bleach_engine_acceptance((uint32_t)ctx->gpr[3]);\n')
    if source!=path.read_bytes():path.write_bytes(source)

    path=directory/'ppu_recomp_037.cpp'
    source=path.read_bytes()
    if b'/* BLEACH_NATIVE_ENGINE_COMPLETION */' not in source:
        source=source.replace(b'#include "ppu_recomp.h"\n',b'#include "ppu_recomp.h"\n'
            b'/* BLEACH_NATIVE_ENGINE_COMPLETION */\n'
            b'extern "C" uint32_t bleach_engine_completion(uint32_t,uint32_t);\n',1)
        a=source.index(b'void func_0024C77C(');b=source.index(b'\nvoid func_',a+10)
        body=source[a:b]
        for reg in (31,29):
            old=f'ctx->gpr[0] = vm_read32(ctx->gpr[{reg}] + -0x7774);'.encode()
            new=f'ctx->gpr[0] = bleach_engine_completion(vm_read32(ctx->gpr[{reg}] + -0x7774), (uint32_t)ctx->gpr[9]);'.encode()
            assert body.count(old)==1
            body=body.replace(old,new)
        source=source[:a]+body+source[b:]
    if b'/* BLEACH_NATIVE_BUFFER_RELEASE */' not in source:
        source=source.replace(b'#include "ppu_recomp.h"\n',b'#include "ppu_recomp.h"\n'
            b'/* BLEACH_NATIVE_BUFFER_RELEASE */\n'
            b'extern "C" void bleach_engine_buffer_released(uint32_t);\n',1)
        a=source.index(b'void func_0024C77C(');b=source.index(b'\nvoid func_',a+10)
        body=source[a:b]
        for reg in (26,9):
            anchor=f'        vm_write8(ctx->gpr[{reg}] + 0x0, ctx->gpr[23]);\n'.encode()
            assert body.count(anchor)==1
            body=body.replace(anchor,anchor+f'        bleach_engine_buffer_released((uint32_t)ctx->gpr[{reg}]);\n'.encode())
        source=source[:a]+body+source[b:]
    if source!=path.read_bytes():path.write_bytes(source)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    patch(p.parse_args().directory)
