"""Promote register state in pure SPU kernels/regions; retain exact references.

Only fixed GPR accesses, local-store access, guard failures and return/trampoline
transfers are eligible. DMA, channels, guest calls, unsupported opcodes and
arbitrary context helpers stay in the original translation. Pure straight-line
regions and closed control-flow regions in those functions have separate references,
so differential verification never repeats their parent's I/O. Guard failures spill
architectural state before the halt unwinds. The runtime can compare the entire
resulting register file/local store against the original on real inputs.
"""
import re

FUNCTION = re.compile(r'^void (\w+)\(spu_context\* ctx\) \{\n(.*?)^}', re.M | re.S)
ALLOWED = {'spu_ls_read128', 'spu_ls_write128', 'SPU_RET', 'SPU_RET_REG', 'spu_halt'}

# Straight-line regions can be promoted even when their parent has I/O or
# indirect calls. Keep an explicit value-operation allowlist: unknown helpers,
# channel accesses, control flow and guards are architectural-state barriers.
VALUE_OPS = {'spu_' + op for op in '''
a addx ah ahi ai and andbi andc andhi andi cbd cdd ceq ceqb ceqbi ceqhi ceqi
cflts cfltu cg cgt cgtbi cgth cgthi cgti chd clgt clgtbi clgth clgthi clgti clz
cntb csflt cuflt cwd cwx fa fceq fcgt fi fm fma fms fnms frest frsqest fs fsm
fsmb fsmbi gb gbb gbh il ila ilh ilhu iohl mpy mpya mpyh mpyhh mpyhhu mpyi
mpyu mpyui nor or orbi orhi ori orx pref_u32 rot roth rothm rothma rothmi roti
rotm rotma rotmai rotmi rotqbii rotqby rotqbyi rotqmbi rotqmbii rotqmby
rotqmbybi rotqmbyi selb sf sfh sfhi sfi shl shlhi shli shlqbii shlqby shlqbyi
shufb xor xorbi xorhi xori xsbh xshw xswd
'''.split()}
REGISTER = r'ctx->gpr\[\d+\](?:\._[us](?:8|16|32|64)\[\d+\])?'
LABEL = re.compile(r'([A-Za-z_]\w*):')
BRANCH = re.compile(r'(?:if \(ctx->gpr\[\d+\]\._[us](?:16|32)\[\d+\] (?:==|!=) 0\) )?goto (\w+);')


def pure_region_line(line):
    """Accept only a complete value assignment or private-LS quadword store."""
    stripped = line.strip()
    if stripped in {'', '/* nop */;', '/* branch hint (ignored) */;'}:
        return True
    if not (re.fullmatch(REGISTER + r'\s*=\s*[^;{}]+;', stripped) or
            re.fullmatch(r'spu_ls_write128\(ctx, [^;{}]+\);', stripped)):
        return False
    calls = set(re.findall(r'\b(\w+)\(', stripped))
    if calls - VALUE_OPS - {'spu_ls_read128', 'spu_ls_write128'}:
        return False
    # Reject local C variables, dynamic register indices and context escape.
    rest = re.sub(REGISTER, '0', stripped)
    rest = re.sub(r'spu_ls_(?:read|write)128\(ctx,', '(', rest)
    rest = re.sub(r'\b(?:' + '|'.join(sorted(VALUE_OPS)) + r')\b', '', rest)
    rest = re.sub(r'\b(?:0x[0-9a-fA-F]+|\d+)[uUlL]*\b', '0', rest)
    return not re.search(r'[A-Za-z_]', rest)


def closed_control_flow(body):
    """Find contiguous pure regions with one entry and a fall-through exit.

    Every included jump must stay inside the region. An incoming jump may
    target only its first line. Include ALL original goto edges in the entry
    check, including edges from unsupported C statements outside the region.
    Shrinking at a crossing edge reaches a fixed point before outlining.
    """
    lines = body.splitlines()
    labels = {m[1]: i for i, s in enumerate(lines) if (m := LABEL.fullmatch(s.strip()))}
    edges = [(i, labels.get(t, -1)) for i, s in enumerate(lines)
             for t in re.findall(r'\bgoto\s+(\w+)\s*;', s)]
    pure = [pure_region_line(s) for s in lines]
    safe = [p or LABEL.fullmatch(s.strip()) or BRANCH.fullmatch(s.strip())
            for p, s in zip(pure, lines)]
    regions, start = [], 0
    while start < len(lines):
        if not safe[start]:
            start += 1
            continue
        end = start
        while end < len(lines) and safe[end]:
            end += 1
        while True:
            previous = end
            for origin, target in edges:
                if start <= origin < end and not start <= target < end:
                    end = min(end, origin)
                if start < target < end and not start <= origin < end:
                    end = min(end, target)
            if end == previous:
                break
        operations = sum(pure[i] and 'ctx->gpr[' in lines[i] for i in range(start, end))
        if operations >= 24 and any(BRANCH.fullmatch(lines[i].strip()) for i in range(start, end)):
            regions.append((start, end))
        start = max(start + 1, end)
    return lines, regions


def emit_region(symbol, region, selected, *, fallback=None, gate='regions', state_scheduler=None):
    registers = sorted(set(map(int, re.findall(r'ctx->gpr\[(\d+)\]', region))))
    written = sorted(set(map(int, re.findall(
        r'ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*=', region))))
    loads = '\n'.join(f'    u128 r{r}=ctx->gpr[{r}];' for r in registers)
    scheduled, committed = state_scheduler(region, pure_region_line) if state_scheduler else (region, set())
    spill = ' '.join(f'ctx->gpr[{r}]=r{r};' for r in written if r not in committed)
    native = re.sub(r'ctx->gpr\[(\d+)\]', lambda m: f'r{m[1]}', scheduled)
    native = re.sub(r'SPU_STATE_COMMIT\((\d+)\);', lambda m: f'ctx->gpr[{m[1]}]=r{m[1]};', native)
    native = native.replace('spu_ls_read128(ctx,', 'bleach_kernel_load(ls,')
    native = native.replace('spu_ls_write128(ctx,', 'bleach_kernel_store(ls,')
    ls = '    uint8_t* const ls=ctx->ls;\n' if 'bleach_kernel_' in native else ''
    selected.append(symbol)
    fallback = fallback or f'{symbol}_reference'
    return (
        f'static void {symbol}_reference(spu_context* ctx) {{\n{region}\n}}\n'
        f'static void {symbol}_native(spu_context* ctx) {{\n'
        f'{ls}{loads}\n{native}\n    {spill}\n}}\n'
        f'static inline void {symbol}(spu_context* ctx) {{\n'
        f'    static SPU_THREAD_LOCAL unsigned checks;\n'
        f'    if (!bleach_kernel_{gate}_enabled(ctx)) {{ {fallback}(ctx); return; }}\n'
        f'    if (checks < bleach_kernel_verify_limit()) {{\n'
        f'        bleach_kernel_verify(ctx,{symbol}_native,{symbol}_reference,"{symbol}",++checks);\n'
        f'    }} else {symbol}_native(ctx);\n}}\n')


def promote_regions(name, body, selected, *, allow_cfg=True, linkage='', state_scheduler=None):
    if allow_cfg:
        lines, regions = closed_control_flow(body)
        if regions:
            helpers, output, cursor = [], [], 0
            for index, (start, end) in enumerate(regions):
                symbol = f'{name}_cfg_region_{index}'
                region = '\n'.join(lines[start:end])
                legacy = f'{symbol}_legacy'
                helpers.append(promote_regions(legacy, region, selected, allow_cfg=False, linkage='static ', state_scheduler=state_scheduler))
                helpers.append(emit_region(symbol, region, selected, fallback=legacy, gate='cfg', state_scheduler=state_scheduler))
                output.extend(lines[cursor:start])
                # Preserve the only possible externally targeted label in the
                # parent. Internal backedges retain their own helper label.
                if LABEL.fullmatch(lines[start].strip()):
                    output.append(lines[start])
                output.append(f'        {symbol}(ctx);')
                cursor = end
            output.extend(lines[cursor:])
            prepared = f'{name}_cfg_parent'
            helpers.append(promote_regions(prepared, '\n'.join(output), selected,
                                           allow_cfg=False, linkage='static ', state_scheduler=state_scheduler))
            # Disabling regions still executes the intact original parent.
            return (''.join(helpers) + f'static void {name}_unsegmented(spu_context* ctx) {{\n{body}\n}}\n'
                    + f'{linkage}void {name}(spu_context* ctx) {{\n'
                    + f'    if (!bleach_kernel_regions_enabled(ctx)) {{ {name}_unsegmented(ctx); return; }}\n'
                    + f'    {prepared}(ctx);\n}}')
    helpers, output, pending = [], [], []

    def flush(loop=False):
        if not pending:
            return
        region = '\n'.join(pending)
        # Small blocks are better left to the normal compiler. This threshold
        # counts actual register/memory operations, not comments or blank lines.
        if sum('ctx->gpr[' in line for line in pending) < 24:
            output.extend(pending)
            pending.clear()
            return
        symbol = f'{name}_{"loop_" if loop else ""}region_{len(helpers)}'
        helpers.append(emit_region(symbol, region, selected, state_scheduler=state_scheduler))
        output.append(f'        {symbol}(ctx);')
        pending.clear()

    lines = body.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        # A closed single-block loop has no calls, exits or intermediate entry
        # labels. Keep the original label in the caller for incoming branches,
        # but run all iterations with one local register state. This permits
        # loop-invariant loads/shuffles to remain native across iterations.
        label = re.fullmatch(r'(loc_\w+):', line.strip())
        if label:
            end = index + 1
            while end < len(lines) and pure_region_line(lines[end]):
                end += 1
            branch = (end < len(lines) and re.fullmatch(
                r'\s*if \(ctx->gpr\[\d+\]\._[us](?:16|32)\[0\] (?:==|!=) 0\) goto '
                + re.escape(label[1]) + r';', lines[end]))
            if branch and sum('ctx->gpr[' in s for s in lines[index+1:end]) >= 24:
                flush()
                output.append(line)
                pending.extend(lines[index:end+1])
                flush(loop=True)
                index = end + 1
                continue
        if pure_region_line(line):
            pending.append(line)
        else:
            flush()
            output.append(line)
        index += 1
    flush()
    if not helpers:
        return f'{linkage}void {name}(spu_context* ctx) {{\n{body}\n}}'
    # Disabling the experiment restores the unsplit parent, not a chain of
    # outlined reference blocks. Thus the fallback retains the original
    # optimizer visibility and call overhead as well as its semantics.
    return (''.join(helpers) + f'static void {name}_unsegmented(spu_context* ctx) {{\n{body}}}\n'
            + f'{linkage}void {name}(spu_context* ctx) {{\n'
            + f'    if (!bleach_kernel_regions_enabled(ctx)) {{ {name}_unsegmented(ctx); return; }}\n'
            + '\n'.join(output) + '\n}')


def promote(source: str, *, state_scheduler=None):
    selected = []
    def replace(match):
        name, body = match.groups()
        calls = set(re.findall(r'\b(\w+)\(ctx(?:,|\))', body))
        fields = set(re.findall(r'ctx->(\w+)', body))
        if (calls - ALLOWED or fields - {'gpr', 'pc', 'image_id', 'status'} or
            'spu_unsupported' in body or len(body.splitlines()) < 12 or
            re.search(r'ctx->gpr\[(?!\d+\])', body)):
            return promote_regions(name, body, selected, state_scheduler=state_scheduler)
        registers = sorted(set(map(int, re.findall(r'ctx->gpr\[(\d+)\]', body))))
        if not registers:
            return match[0]
        written = sorted(set(map(int, re.findall(r'ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*=', body))))
        # Keeping all stores also preserves dead temporaries visible at exits.
        scheduled, committed = state_scheduler(body, pure_region_line) if state_scheduler else (body, set())
        spill = ' '.join(f'ctx->gpr[{r}]=r{r};' for r in written if r not in committed)
        native = re.sub(r'ctx->gpr\[(\d+)\]', lambda m: f'r{m[1]}', scheduled)
        native = re.sub(r'SPU_STATE_COMMIT\((\d+)\);', lambda m: f'ctx->gpr[{m[1]}]=r{m[1]};', native)
        native = native.replace('spu_ls_read128(ctx,', 'bleach_kernel_load(ls,')
        native = native.replace('spu_ls_write128(ctx,', 'bleach_kernel_store(ls,')
        # A stack guard can unwind with longjmp: its register state must be
        # visible before calling the runtime, not only at a C return below.
        native = native.replace('spu_halt(ctx);', '{ ' + spill + ' spu_halt(ctx); }')
        native = re.sub(r'\breturn;', '{ ' + spill + ' return; }', native)
        native = re.sub(r'\b(SPU_RET(?:_REG)?\(ctx(?:,\s*\d+)?\));',
                        lambda m: '{ ' + spill + ' ' + m[1] + '; }', native)
        loads = '\n'.join(f'    u128 r{r}=ctx->gpr[{r}];' for r in registers)
        selected.append(name)
        return (f'static void {name}_reference(spu_context* ctx) {{\n{body}}}\n'
                f'static void {name}_native(spu_context* ctx) {{\n'
                f'    uint8_t* const ls=ctx->ls;\n{loads}\n{native}\n    {spill}\n}}\n'
                f'void {name}(spu_context* ctx) {{\n'
                f'    static SPU_THREAD_LOCAL unsigned checks;\n'
                f'    if (!bleach_kernel_enabled(ctx)) {{ {name}_reference(ctx); return; }}\n'
                f'    if (checks < bleach_kernel_verify_limit()) {{\n'
                f'        bleach_kernel_verify(ctx,{name}_native,{name}_reference,"{name}",++checks);\n'
                f'    }} else {name}_native(ctx);\n}}')
    return FUNCTION.sub(replace, source), selected
