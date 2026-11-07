"""Schedule architectural register writeback in lifted, private-LS SPU kernels.

The caller supplies its complete-statement purity predicate. Inside each straight
line block, publish a register immediately after its last assignment instead of
keeping its value alive until the entire kernel returns. This preserves ALL
architectural registers; it does not assume the SPU calling convention discards
temporaries. A later local use still uses the local value.

Labels, branches, calls, guards and unknown statements end the block. A register
written by ANY unparsed statement retains the original final/unwind spill.
Unknown writes are never moved out of their conditional scope. Private local
store must not alias the register file, as required by the caller's promotion.
The returned commit markers must be lowered AFTER replacing register reads by
local variables. Loops can execute additional writebacks: profitability must be
checked, not assumed from register pressure alone.
"""
import re

WRITE = re.compile(r'ctx->gpr\[(\d+)\](?:\._\w+\[\d+\])?\s*=(?!=)')
MARKER = re.compile(r'SPU_STATE_COMMIT\((\d+)\);')


def schedule_writeback(body, is_pure_line):
    lines = body.splitlines()
    unknown = {int(m[1]) for line in lines if not is_pure_line(line)
               for m in WRITE.finditer(line)}
    output, block, committed = [], [], set()

    def flush():
        last = {}
        for i, line in enumerate(block):
            for match in WRITE.finditer(line):
                reg = int(match[1])
                if reg not in unknown:
                    last[reg] = i
        at = {}
        for reg, i in last.items():
            at.setdefault(i, []).append(reg)
        for i, line in enumerate(block):
            output.append(line)
            for reg in sorted(at.get(i, [])):
                output.append(f'    SPU_STATE_COMMIT({reg});')
                committed.add(reg)
        block.clear()

    for line in lines:
        if is_pure_line(line):
            block.append(line)
        else:
            flush()
            output.append(line)
    flush()
    return '\n'.join(output), committed


def lower_commits(body):
    return MARKER.sub(lambda m: f'ctx->gpr[{m[1]}]=r{m[1]};', body)
