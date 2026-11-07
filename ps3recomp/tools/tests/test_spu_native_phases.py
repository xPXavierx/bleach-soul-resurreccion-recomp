import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from spu_native_phases import closed_phase,dependencies_and_marks

ENTRY='p_00000010';CONT='p_00000020';LEAF='p_00000030'
def function(name,body):return f'void {name}(spu_context* ctx) {{\n{body}\n}}\n'
def call(name=LEAF):
    return ('ctx->gpr[0] = spu_link(0x28); { int32_t _si = (int32_t)ctx->image_id; '
     f'ctx->host_depth++; {name}(ctx); spu_drain_call(ctx, 0x28); '
     'ctx->host_depth--; spu_img_restore(ctx, _si); }')
def source(leaf='ctx->gpr[3] = spu_ai(ctx->gpr[3], 4);\nSPU_RET(ctx);',extra=''):
    head=('ctx->gpr[126] = spu_link(0x20); { ctx->pc = 0x20; '
          'g_spu_trampoline_fn = p_00000020; return; }\n'
          '{ ctx->pc = 0x20; g_spu_trampoline_fn = p_00000020; return; }')
    return function(ENTRY,head)+function(CONT,call()+'\n'+extra+'\nSPU_RET(ctx);')+function(LEAF,leaf)
def generate(s,opaque=set()):
    return closed_phase(s,ENTRY,CONT,{LEAF},opaque,value_ops={'spu_ai'})

class ClosedPhases(unittest.TestCase):
    def test_loop_backedge_requires_live_input(self):
        live,code=dependencies_and_marks('loc_loop:\nctx->gpr[3] = spu_ai(ctx->gpr[3], -1);\nif (ctx->gpr[3]._u32[0] != 0) goto loc_loop;')
        self.assertEqual(live,[3]);self.assertIn('loc_loop:\n        ps3_phase_result_mark_gprs',code)
    def test_conditional_write_does_not_key_old_output(self):
        live,code=dependencies_and_marks('if (ctx->gpr[2]._u32[0] == 0) goto done;\nctx->gpr[3] = spu_ai(ctx->gpr[4], 1);\ndone:;')
        self.assertEqual(live,[2,4]);self.assertEqual(code.count('ps3_phase_result_mark_gprs'),1)
    def test_partial_write_requires_original_register(self):
        live,_=dependencies_and_marks('ctx->gpr[3]._u32[0] = ctx->gpr[4]._u32[0];')
        self.assertEqual(live,[3,4])
    def test_overwritten_register_is_not_live(self):
        live,_=dependencies_and_marks('ctx->gpr[2] = spu_ai(ctx->gpr[4], 1);\nctx->gpr[3] = spu_ai(ctx->gpr[2], 3);')
        self.assertEqual(live,[4])
    def test_leaf_body_becomes_native_locals(self):
        code,report=generate(source())
        self.assertEqual(report['inline_calls'],1)
        self.assertIn('r3 = spu_ai(r3, 4);',code)
        self.assertIn('ctx->gpr[3]=r3;',code)
        self.assertNotIn('spu_drain_call',code)
    def test_local_loop_labels_are_unique_per_call(self):
        leaf='loc_00000030:\nctx->gpr[3] = spu_ai(ctx->gpr[3], -1);\nif (ctx->gpr[3]._u32[0] != 0) goto loc_00000030;\nSPU_RET(ctx);'
        code,report=generate(source(leaf,call()))
        self.assertEqual(report['inline_calls'],2)
        self.assertIn('phase_leaf_0_00000030:',code)
        self.assertIn('phase_leaf_1_00000030:',code)
    def test_channel_access_in_leaf_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'context escape'):
            generate(source('spu_wrch(ctx, MFC_Cmd, ctx->gpr[3]);\nSPU_RET(ctx);'))
    def test_unknown_helper_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'unapproved helper'):
            generate(source(extra='write_global(42);'))
    def test_dynamic_register_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'dynamic register'):
            generate(source(extra='ctx->gpr[index] = spu_ai(ctx->gpr[3], 1);'))
    def test_external_jump_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'non-local goto'):
            generate(source(extra='goto somewhere_else;'))
    def test_extra_context_mutation_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'context fields'):
            generate(source(extra='ctx->int_enable = 1;'))
    def test_undeclared_callee_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'Undeclared callee'):
            generate(source(extra=call('external')))
    def test_declared_opaque_call_has_spill_and_reload(self):
        code,report=generate(source(extra=call('external')),{'external'})
        self.assertEqual(report['opaque_barriers'],1)
        self.assertIn('external(ctx); spu_drain_call(ctx, 0x28);',code)
        self.assertIn('r3=ctx->gpr[3];',code)
    def test_mismatched_entry_continuation_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'declared unconditional continuation'):
            generate(source().replace('ctx->pc = 0x20;','ctx->pc = 0x40;',1))

if __name__=='__main__':unittest.main()
