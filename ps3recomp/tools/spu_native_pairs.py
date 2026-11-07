"""Pack independent 128-bit SPU arithmetic into native 256-bit operations.

Input is the pure local-register form emitted by a static lifter. A pending
operation may move down only across explicitly pure assignments/private loads.
Reads of its destination or writes to its inputs/destination end the search.
Stores, unknown helpers, labels, branches, calls and returns are barriers.
Only *_native functions change; original differential references stay intact.
"""
import re

OPS = {'fa': 2, 'fs': 2, 'fm': 2, 'fma': 3, 'fms': 3, 'fnms': 3}
REG = re.compile(r'\br\d+\b')
OP = re.compile(r'^(\s*)(r\d+) = spu_(fa|fs|fm|fma|fms|fnms)\(([^;]+)\);$')
ASSIGN = re.compile(r'^\s*(r\d+)(?:\._[us](?:8|16|32|64)\[\d+\])?\s*=\s*([^;{}]+);$')
FUNCTION = re.compile(r'^(static void \w+_native\(spu_context\* ctx\) \{\n)(.*?)^}',re.M|re.S)

def operation(line):
    match=OP.fullmatch(line)
    if not match:return None
    args=tuple(x.strip() for x in match[4].split(','))
    if len(args)!=OPS[match[3]] or not all(REG.fullmatch(x) for x in args):return None
    return match[1],match[2],match[3],args

def effects(line,pure_calls):
    text=line.strip()
    if not text or text.startswith('/*') and text.endswith('*/;'):return set(),set()
    assignment=ASSIGN.fullmatch(line)
    if not assignment:return None
    rhs=assignment[2]
    if re.search(r'\*\s*ls\b|\bls\s*\[|&\s*r\d+\b|\br\d+\s*\[|\bctx\b',rhs):return None
    calls=set(re.findall(r'\b(\w+)\s*\(',rhs))
    if calls-pure_calls:return None
    # The local form must not contain pointer access, comma expressions,
    # increments, assignments or opaque identifiers outside allowed calls.
    rest=REG.sub('0',rhs)
    rest=re.sub(r'\._[us](?:8|16|32|64)\[\d+\]','',rest)
    rest=re.sub(r'\b(?:'+ '|'.join(map(re.escape,sorted(pure_calls))) +r')\b','',rest)
    rest=re.sub(r'\b(?:0x[\da-fA-F]+|\d+)[uUlL]*\b','0',rest)
    rest=re.sub(r'\bls\b','0',rest)
    if re.search(r'[A-Za-z_=;{}]|\+\+|--|->',rest):return None
    return set(REG.findall(rhs)),{assignment[1]}

def pack_body(body,pure_calls,lookahead=24):
    lines=body.splitlines();pairs=[]
    allowed=set(pure_calls)|{'spu_'+op for op in OPS}
    for i,line in enumerate(lines):
        first=operation(line)
        if not first:continue
        indent,dest,op,args=first
        reads=set(args)
        for j in range(i+1,min(len(lines),i+lookahead+1)):
            second=operation(lines[j])
            if second and second[2]==op and second[1]!=dest and dest not in second[3]:
                # All arguments evaluate before either destination is assigned;
                # the second destination may be an input of the first operation.
                variable=f'spu_pair_{len(pairs)}'
                arguments=', '.join(args+second[3])
                lines[i]=indent+'/* arithmetic packed with a later independent operation */'
                lines[j]=(indent+'{ const spu_native_fp_pair '+variable+
                          ' = spu_native_pair_'+op+'('+arguments+'); '+
                          dest+' = '+variable+'.lo; '+second[1]+' = '+variable+'.hi; }')
                pairs.append({'first_line':i,'second_line':j,'op':op})
                break
            effect=effects(lines[j],allowed)
            if effect is None:break
            between_reads,between_writes=effect
            if dest in between_reads or between_writes & (reads|{dest}):break
    return '\n'.join(lines)+'\n',pairs

def pack_source(source,pure_calls):
    report=[]
    def replace(match):
        body,pairs=pack_body(match[2],pure_calls)
        if pairs:report.append({'function':match[1].split('(')[0].split()[-1],'pairs':pairs})
        return match[1]+body+'}'
    return FUNCTION.sub(replace,source),report
