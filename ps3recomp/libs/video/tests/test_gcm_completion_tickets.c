/* Exercise production flip delivery with host-fence completion tickets. */
#define main legacy_flip_stream_tests
#include "test_gcm_flip_stream.c"
#undef main
void rsx_draw_engine_set_display_buffer(u32 id,u32 loc,u32 off,u32 pitch,u32 w,u32 h)
{ (void)id;(void)loc;(void)off;(void)pitch;(void)w;(void)h; }

static unsigned calls, depth, max_depth, nested;
static u32 observed[1024];
#ifdef _WIN32
static HANDLE callback_entered,callback_release;
static int block_callback;
static DWORD WINAPI pump_thread(LPVOID ignored)
{ (void)ignored;ppu_gcm_pump();return 0; }
#endif
static void completed(u32 opd, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    (void)opd;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;
    ++depth;if(depth>max_depth)max_depth=depth;
    observed[calls++]=s_flip_status;
#ifdef _WIN32
    if(block_callback) {
        block_callback=0;SetEvent(callback_entered);
        WaitForSingleObject(callback_release,5000);
    }
#endif
    if(nested) { nested=0;cellGcmCompleteFlip(s_flip_req_seq);ppu_gcm_pump(); }
    --depth;
}

int main(void)
{
    if(legacy_flip_stream_tests())return 1;
    vm_base=calloc(1,ppu_vm_size);
    if(!vm_base)return 2;
    /* No early compatibility callbacks: the host owns completion delivery. */
#ifdef _WIN32
    _putenv("GCM_FLIPCB_ONTICK=1");
#else
    setenv("GCM_FLIPCB_ONTICK","1",1);
#endif
    s_flip_handler_opd=0x1000;g_ps3_guest_caller=completed;
    (void)GCM_PENDING_TAKE();
    cellGcmSetFlipCommand(1);
    const u64 first=cellGcmClaimFlipCompletion();
    check(first && s_flip_status==CELL_GCM_FLIP_STATUS_WAITING,"claim leaves GPU work waiting");
    check(!cellGcmClaimFlipCompletion(),"executed flip can be claimed only once");
    cellGcmTickFlip();ppu_gcm_pump();
    check(!calls,"legacy tick cannot deliver a claimed but unfinished flip");
    cellGcmSetFlipCommand(1);
    const u64 second=cellGcmClaimFlipCompletion();
    cellGcmCompleteFlip((u32)first);ppu_gcm_pump();
    check(calls==1 && observed[0]==CELL_GCM_FLIP_STATUS_WAITING,"old completion preserves newer WAITING request");
    cellGcmCompleteFlip((u32)second);ppu_gcm_pump();
    check(calls==2 && observed[1]==CELL_GCM_FLIP_STATUS_DONE,"matching completion marks DONE before callback");
    cellGcmTickFlip();ppu_gcm_pump();
    check(calls==2,"completed ticket receives no duplicate legacy callback");
    s_flip_req_seq=UINT32_MAX;
    cellGcmSetFlipCommand(1);
    const u64 zero=cellGcmClaimFlipCompletion();
    check(zero==(1ull<<32),"wrapped epoch zero remains a valid ticket");
    cellGcmCompleteFlip((u32)zero);ppu_gcm_pump();
    check(calls==3 && observed[2]==CELL_GCM_FLIP_STATUS_DONE,"wrapped completion is delivered");
    cellGcmResetFlipStatus();
    check(!cellGcmClaimFlipCompletion(),"reset status alone is not an executed flip");
    for(unsigned i=0;i<300;i++)cellGcmCompleteFlip(s_flip_req_seq-1);
    ppu_gcm_pump();
    check(calls==259,"one pump bounds its batch without coalescing repeated completions");
    ppu_gcm_pump();
    check(calls==303 && s_flip_status==CELL_GCM_FLIP_STATUS_WAITING,"remaining completions survive across queue blocks");
    nested=1;cellGcmCompleteFlip(s_flip_req_seq);ppu_gcm_pump();
    check(calls==305 && max_depth==1,"nested pump preserves nonreentrant callback delivery");
    ppu_gcm_pump();check(calls==305,"empty pump does not redeliver");
#ifdef _WIN32
    callback_entered=CreateEvent(NULL,TRUE,FALSE,NULL);
    callback_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    cellGcmResetFlipStatus();block_callback=1;
    cellGcmCompleteFlip(s_flip_req_seq-1);
    HANDLE worker=CreateThread(NULL,0,pump_thread,NULL,0,NULL);
    check(WaitForSingleObject(callback_entered,5000)==WAIT_OBJECT_0,"first callback running on concurrent pump");
    cellGcmCompleteFlip(s_flip_req_seq);ppu_gcm_pump();
    check(calls==306 && s_flip_status==CELL_GCM_FLIP_STATUS_WAITING,"concurrent pump cannot overtake active callback");
    SetEvent(callback_release);
    check(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0,"concurrent pump exits normally");
    check(calls==307 && max_depth==1 && observed[305]==CELL_GCM_FLIP_STATUS_WAITING &&
          observed[306]==CELL_GCM_FLIP_STATUS_DONE,"concurrent completion preserves callback order");
    CloseHandle(worker);CloseHandle(callback_entered);CloseHandle(callback_release);
#endif
    rsx_callback_queue_clear(&s_flip_completions);
    free(vm_base);
    printf("[gcm_completion_tickets] %s (%d failed)\n",fails?"FAIL":"PASS",fails);
    return fails?1:0;
}
