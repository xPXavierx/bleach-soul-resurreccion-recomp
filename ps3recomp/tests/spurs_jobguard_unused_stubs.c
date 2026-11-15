/* Link-only dependencies of unrelated SPURS exports in the Windows test TU.
 * Any accidental call fails the test, rather than simulating SPU work. */
#include <stdlib.h>
#include <stdint.h>
#define UNUSED_DEP(name) void name(void) { abort(); }
UNUSED_DEP(spu_taskset_signal_task)
UNUSED_DEP(spu_ef_bits_post)
UNUSED_DEP(spu_taskset_signal_parked_obj)
UNUSED_DEP(spu_taskset_latch_wake)
UNUSED_DEP(spurs_policy_release_instance)
UNUSED_DEP(sys_event_queue_cancel_by_id)
UNUSED_DEP(sys_event_queue_uncancel_by_id)
UNUSED_DEP(spurs_taskset_init)
UNUSED_DEP(spurs_taskset_take_slot)
UNUSED_DEP(spu_taskset_mark_running)
UNUSED_DEP(spu_taskset2_set_exit)
UNUSED_DEP(spurs_taskset_add_task)
UNUSED_DEP(spu_elf_image_size)
UNUSED_DEP(spu_workload_dispatch_async)
UNUSED_DEP(spurs_policy_release_workload)
UNUSED_DEP(spu_run_policy_module)
UNUSED_DEP(spurs_job_set_spu_num)
UNUSED_DEP(spurs_job_set_chain_handle)
UNUSED_DEP(spu_workload_fingerprint)
UNUSED_DEP(spu_workload_find_img)
UNUSED_DEP(spu_workload_dispatch_job)
UNUSED_DEP(sys_event_queue_push_by_id)
_Thread_local volatile unsigned g_spurs_pm_polls;
_Thread_local int g_spurs_job_mbox_valid;
_Thread_local uint32_t g_spurs_job_mbox;
_Thread_local uint32_t g_spurs_job_cmd;
void (*g_spu_line_commit_hook)(uint32_t);
