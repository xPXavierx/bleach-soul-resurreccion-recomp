/* Link the production PPU loader; only external device operations are fakes. */
#define PPU_NO_INLINE_LOADS
#include "ppu_recomp.h"
#include "ps3emu/ps3_write_watch.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} }while(0)
extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base=0x20000000u;
extern uint32_t ppu_vm_size,g_barrier_sync_watch,g_ww_lo,g_ww_hi;
void ppu_ww_note_atomic(uint32_t,uint32_t,int,void*);
static unsigned reserved,locked,notified,committed;
int spu_coh_is_reserved(uint32_t) {return reserved;}
void spu_lockline_lock(void) {CHECK(!locked);locked=1;}
void spu_lockline_unlock(void) {CHECK(locked);locked=0;}
void spu_coh_notify_write(uint32_t) {CHECK(locked);++notified;}
void spu_raw_reg_store(uint32_t,uint32_t,unsigned) {std::abort();}
void cellGcm_fifo_notify_put(void) {std::abort();}
/* The COFF linker resolves these unused loader/syscall roots too. This fixture
 * must fail if it accidentally invokes a subsystem outside memory stores. */
unsigned spu_registry_size(void) {return 0;}
void spu_registry_entry(...) {std::abort();}
void spu_raw_reg_load(...) {std::abort();}
void cellGcm_ref_on_poll(...) {std::abort();}
uint64_t ps3_hle_count=0;
void ps3_msf(...) {std::abort();}
void ppu_prof_stamp(...) {std::abort();}
void sys_mmapper_allocate_address(...) {std::abort();}
void ps3_mmapper_block_size(...) {std::abort();}
void sys_mmapper_search_map_address(...) {std::abort();}
void lv2_try_syscall(...) {std::abort();}
void sys_memory_set_image_query(...) {std::abort();}
void ppu_thread_register_main(...) {std::abort();}
void* g_ppu_thread_entry_trampoline=nullptr;
}
const func_entry function_table[]={{0,nullptr,nullptr}};
const uint64_t function_table_count=0;
static void commit(uint32_t a,uint32_t n,uint32_t source)
{
    CHECK(!locked && a==0x11000 && n && source==PS3_WRITE_PPU);
    ++committed;
}
int main()
{
    vm_base=(uint8_t*)VirtualAlloc(nullptr,0x20000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(vm_base);ppu_vm_size=0x20000;
    g_ps3_write_watch.commit=commit;g_ps3_write_watch.addr=0x11000;
    for(reserved=0;reserved<2;reserved++) {
        vm_write8(0x11000,0x89);CHECK(vm_base[0x11000]==0x89);
        vm_write16(0x11000,0xabcd);CHECK(vm_base[0x11000]==0xab && vm_base[0x11001]==0xcd);
        vm_write32(0x11000,0x12345678);CHECK(!memcmp(vm_base+0x11000,"\x12\x34\x56\x78",4));
        vm_write64(0x11000,UINT64_C(0x1122334455667788));
        CHECK(!memcmp(vm_base+0x11000,"\x11\x22\x33\x44\x55\x66\x77\x88",8));
    }
    CHECK(notified==4 && committed==8 && !locked);reserved=0;
    /* Runtime arming must remain observable even if no environment observer
     * existed during static initialization. Includes disarming and rearming. */
    g_barrier_sync_watch=0x10fc0;
    vm_write32(0x11000,0xabcdef01);
    g_barrier_sync_watch=0;
    vm_write32(0x11000,0xabcdef02);
    g_barrier_sync_watch=0x10fc0;
    ppu_ww_note_atomic(0x11004,0xabcdef03,4,nullptr);
    g_barrier_sync_watch=0;
    CHECK(committed==10);
    if(getenv("PPU_WWATCH"))CHECK(g_ww_lo==0x11000 && g_ww_hi==0x11020);
    printf("PASS production stores: endian widths, reserved-line notifications, publication observer, dynamic diagnostic watches.\n");
    VirtualFree(vm_base,0,MEM_RELEASE);
}
