"""Form a native phase from one continuation and explicitly approved leaf calls.

This is an offline source transformation. Unknown callees, non-local jumps,
dynamic dispatch, and extra context fields are rejected. Opaque cold calls are
explicit barriers, retaining the original call and complete register spills.
The title must gate verification so an opaque I/O path is never executed twice.
"""
import re

FUNCTION=re.compile(r'^void (\w+)\(spu_context\* ctx\) \{\n(.*?)^}',re.M|re.S)
CALL=re.compile(r'ctx->gpr\[0\] = spu_link\(0x([0-9A-F]+)\); '
 r'\{ int32_t _si = \(int32_t\)ctx->image_id; ctx->host_depth\+\+; '
 r'(\w+)\(ctx\); spu_drain_call\(ctx, 0x\1\); ctx->host_depth--; spu_img_restore\(ctx, _si\); \}')

def _closed_source(source,entry,continuation,leaves,opaque,*,value_ops):
    functions=dict(FUNCTION.findall(source))
    head=functions[entry];body=functions[continuation]
    pc=continuation.rsplit('_',1)[-1].lstrip('0') or '0'
    tail=(r'ctx->gpr\[(\d+)\] = spu_link\(0x'+pc+r'\); '
          r'\{ ctx->pc = 0x'+pc+r'; g_spu_trampoline_fn = '+re.escape(continuation)+
          r'; return; \}\s*\{ ctx->pc = 0x'+pc+r'; g_spu_trampoline_fn = '+
          re.escape(continuation)+r'; return; \}\s*$')
    m=re.search(tail,head)
    if not m:raise ValueError('Entry is not the declared unconditional continuation')
    head=head[:m.start()]+f'ctx->gpr[{m[1]}] = spu_link(0x{pc});\n'
    # The original drain takes this continuation and clears its trampoline.
    head+=f'ctx->pc = 0x{pc}; g_spu_trampoline_fn = NULL;\n'
    inlined=[];barriers=[]
    def inline(match):
        ret,callee=match.groups()
        if callee in opaque:
            index=len(barriers);barriers.append(match[0]);return f'PHASE_BARRIER_{index};'
        if callee not in leaves:raise ValueError('Undeclared callee: '+callee)
        child=functions[callee]
        if child.count('SPU_RET(ctx);')!=1 or not child.rstrip().endswith('SPU_RET(ctx);'):
            raise ValueError('Leaf must have one terminal return: '+callee)
        if CALL.search(child) or 'g_spu_trampoline_fn' in child:
            raise ValueError('Leaf has nested calls/dispatch: '+callee)
        calls=set(re.findall(r'\b(\w+)\(ctx(?:,|\))',child))
        if calls-{'SPU_RET','spu_ls_read128','spu_ls_write128'}:
            raise ValueError('Leaf has context escape: '+callee)
        if set(re.findall(r'ctx->(\w+)',child))-{'gpr'}:
            raise ValueError('Leaf touches extra context fields: '+callee)
        prefix=f'phase_leaf_{len(inlined)}_'
        child=re.sub(r'\bloc_([0-9A-F]+)\b',lambda m:prefix+m[1],child)
        child=child.replace('SPU_RET(ctx);','ctx->pc = ctx->gpr[0]._u32[0];')
        inlined.append(callee)
        return f'ctx->gpr[0] = spu_link(0x{ret});\n'+child
    body=CALL.sub(inline,body)
    merged=head+body
    if re.search(r'ctx->gpr\[(?!\d+\])',merged):
        raise ValueError('Phase contains dynamic register access')
    labels=set(re.findall(r'^(\w+):',merged,re.M))
    if set(re.findall(r'\bgoto (\w+);',merged))-labels:
        raise ValueError('Phase contains a non-local goto')
    remaining=merged[len(head):]
    if 'g_spu_trampoline_fn' in remaining or 'spu_indirect_branch' in remaining:
        raise ValueError('Phase contains unresolved dispatch')
    calls=set(re.findall(r'\b(\w+)\(ctx(?:,|\))',remaining))
    if calls-{'SPU_RET','spu_ls_read128','spu_ls_write128','spu_halt'}:
        raise ValueError('Phase contains undeclared context calls: '+repr(calls))
    if set(re.findall(r'ctx->(\w+)',merged))-{'gpr','pc','status','image_id'}:
        raise ValueError('Phase touches unsupported context fields')
    all_calls=set(re.findall(r'\b(\w+)\(',merged))
    permitted=set(value_ops)|{'SPU_RET','spu_link','spu_ls_read128','spu_ls_write128','spu_halt','fprintf'}
    if all_calls-permitted:
        raise ValueError('Phase calls an unapproved helper: '+repr(all_calls-permitted))
    registers=sorted(set(map(int,re.findall(r'ctx->gpr\[(\d+)\]',merged))))
    written=sorted(set(map(int,re.findall(r'ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*=',merged))))
    return merged,barriers,dict(entry=entry,continuation=continuation,inline_calls=len(inlined),
                     leaf_functions=sorted(set(inlined)),opaque_barriers=len(barriers),
                     registers=len(registers),written_registers=len(written))

def _emit_native(merged,name,barriers,*,capture=False):
    registers=sorted(set(map(int,re.findall(r'ctx->gpr\[(\d+)\]',merged))))
    written=sorted(set(map(int,re.findall(r'ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*=',merged))))
    spill=' '.join(f'ctx->gpr[{r}]=r{r};' for r in written)
    reload=' '.join(f'r{r}=ctx->gpr[{r}];' for r in registers)
    native=re.sub(r'ctx->gpr\[(\d+)\]',lambda m:'r'+m[1],merged)
    native=native.replace('spu_ls_read128(ctx,',('ps3_phase_result_load' if capture else 'bleach_kernel_load')+'(ls,')
    native=native.replace('spu_ls_write128(ctx,',('ps3_phase_result_store' if capture else 'bleach_kernel_store')+'(ls,')
    native=native.replace('spu_halt(ctx);','{ '+spill+' spu_halt(ctx); }')
    native=native.replace('SPU_RET(ctx);','{ '+spill+' SPU_RET(ctx); }')
    for i,original in enumerate(barriers):
        native=native.replace(f'PHASE_BARRIER_{i};','{ '+('ps3_phase_result_abort(); ' if capture else '')+spill+' '+original+' '+reload+' }')
    # Read-only context fields and control output remain visible. The numerical
    # computation uses native locals throughout the complete function closure.
    text=f'static void {name}(spu_context* ctx) {{\n'
    text+='    uint8_t* const ls=ctx->ls;\n'
    text+='\n'.join(f'    u128 r{r}=ctx->gpr[{r}];' for r in registers)+'\n'
    text+=native+'\n'+(spill if capture else '')+'\n}\n'
    return text

def closed_phase(source,entry,continuation,leaves,opaque,*,value_ops):
    merged,barriers,report=_closed_source(source,entry,continuation,leaves,opaque,value_ops=value_ops)
    return _emit_native(merged,entry+'_phase_native',barriers),report

def dependencies_and_marks(body):
    """CFG liveness with empty live-out; record actual write sets per block.

    Outputs cannot use a union of all possibly written registers: an untaken
    branch must retain THIS invocation's incoming values on a cache hit.
    Partial-register stores read their previous full register before defining it.
    """
    lines=body.splitlines();n=len(lines)
    labels={m[1]:i for i,line in enumerate(lines) if (m:=re.fullmatch(r'\s*(\w+):\s*;?\s*',line))}
    uses=[];defs=[];edges=[];starts={0}|set(labels.values())
    for i,line in enumerate(lines):
        d=re.match(r'\s*ctx->gpr\[(\d+)\]([^=]*?)\s*=\s*(.*);\s*$',line)
        read=set(map(int,re.findall(r'ctx->gpr\[(\d+)\]',d[3] if d else line)))
        define={int(d[1])} if d else set()
        if d and d[2].strip():read|=define
        uses.append(read);defs.append(define)
        jumps=re.findall(r'\bgoto (\w+);',line)
        if len(jumps)>1:raise ValueError('Unsupported compound branch')
        successors=[]
        if jumps:
            if jumps[0] not in labels:raise ValueError('Non-local middle jump')
            successors.append(labels[jumps[0]])
            if i+1<n:starts.add(i+1)
        if (not jumps or line.lstrip().startswith('if ')) and i+1<n:successors.append(i+1)
        edges.append(successors)
    live=[set() for _ in lines]
    changed=True
    while changed:
        changed=False
        for i in reversed(range(n)):
            after=set().union(*(live[j] for j in edges[i]))
            before=uses[i]|(after-defs[i])
            if live[i]!=before:live[i]=before;changed=True
    marked=[];points=sorted(starts)+[n]
    for start,end in zip(points,points[1:]):
        writes=set().union(*defs[start:end]);low=sum(1<<r for r in writes if r<64)
        high=sum(1<<(r-64) for r in writes if r>=64)
        block=lines[start:end]
        if writes:
            mark=f'        ps3_phase_result_mark_gprs(UINT64_C(0x{low:x}),UINT64_C(0x{high:x}));'
            block.insert(1 if start in labels.values() else 0,mark)
        marked.extend(block)
    return sorted(live[0]),'\n'.join(marked)

def result_phase(source,entry,continuation,leaves,opaque,*,value_ops,start_guard,end_label,guarded_opaque=False):
    merged,barriers,report=_closed_source(source,entry,continuation,leaves,opaque,value_ops=value_ops)
    if barriers and not guarded_opaque:raise ValueError('Result phase needs a proven opaque-call guard')
    lines=merged.splitlines();cuts=[i for i,line in enumerate(lines) if start_guard in line]
    ends=[i for i,line in enumerate(lines) if line.strip()==end_label+':']
    if len(cuts)!=1 or len(ends)!=1 or cuts[0]>=ends[0]:raise ValueError('Ambiguous result phase boundary')
    first,last=cuts[0]+1,ends[0]
    prefix='\n'.join(lines[:first]);suffix='\n'.join(lines[last+1:])
    middle='\n'.join(lines[first:last])+'\n'+end_label+':;'
    # Only leaf-return PC writes are removed. The fresh epilogue publishes the
    # actual caller PC; private phase guards exclude intermediate observers/I/O.
    middle=re.sub(r'ctx->pc = ctx->gpr\[0\]\._u32\[0\];','',middle)
    if re.search(r'ctx->(?!gpr\b)',middle):raise ValueError('Middle has non-register context access')
    live,marked=dependencies_and_marks(middle)
    name=entry+'_result_middle'
    code=_emit_native(marked,name,barriers,capture=True)
    code+=f'static void {entry}_result_native(spu_context* ctx) {{\n'+prefix+'\n'
    code+='    static const uint8_t inputs[]={'+','.join(map(str,live))+'};\n'
    code+=f'    ps3_phase_result_run(ctx,bleach_phase_result_version(),inputs,sizeof(inputs),{name},bleach_phase_result_hint(ctx));\n'
    code+=suffix+'\n}\n'
    report['result_live_in_registers']=live
    report['result_boundary']=[start_guard,end_label]
    return code,report
