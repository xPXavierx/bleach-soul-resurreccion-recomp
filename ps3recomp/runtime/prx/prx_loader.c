/*
 * prx_loader.c — see prx_loader.h for the design.
 *
 * Two steps per module: place the relocated image into guest RAM at its load
 * base, then register every lifted function in the host dispatch table.
 */
#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's guest memory: host_ptr = vm_base + guest_addr. Declared by
 * runtime/memory/vm.h; we only need the symbol, so extern it directly to keep
 * the loader independent of the memory backend's init path. */
extern uint8_t* vm_base;

/* ---- export registry ---------------------------------------------------- *
 * Flat NID -> guest-address table, filled as modules load. A handful of system
 * PRXs export a few hundred symbols total, so a linear array with linear lookup
 * is plenty to fill; lookups go through the cache below. */
typedef struct { uint32_t nid; uint32_t guest_addr; } prx_export_reg_entry;
#define PRX_EXPORT_REG_CAP 2048
static prx_export_reg_entry s_export_reg[PRX_EXPORT_REG_CAP];
static uint32_t             s_export_reg_n = 0;

/* The HLE dispatcher resolves the NID on every import call, so the answers
 * (misses included) are kept in a direct-mapped cache: (address << 32) | nid,
 * one 64-bit word per entry so other threads never see half of one. Any
 * registration clears it. */
#define PRX_RESOLVE_CACHE 4096u
static volatile uint64_t s_resolve_cache[PRX_RESOLVE_CACHE];
static volatile uint32_t s_resolve_generation;   /* odd while a registration runs */

static void prx_export_register(uint32_t nid, uint32_t guest_addr)
{
    __atomic_add_fetch(&s_resolve_generation, 1u, __ATOMIC_SEQ_CST);
    memset((void*)s_resolve_cache, 0, sizeof(s_resolve_cache));
    /* Last writer wins if two modules export the same NID (matches the PS3
     * loader's link order). Overwrite an existing entry rather than duplicate. */
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].nid == nid) {
            s_export_reg[i].guest_addr = guest_addr;
            __atomic_add_fetch(&s_resolve_generation, 1u, __ATOMIC_SEQ_CST);
            return;
        }
    }
    if (s_export_reg_n < PRX_EXPORT_REG_CAP)
        s_export_reg[s_export_reg_n++] = (prx_export_reg_entry){ nid, guest_addr };
    else
        fprintf(stderr, "[prx] export registry full, dropping NID 0x%08X\n", nid);
    __atomic_add_fetch(&s_resolve_generation, 1u, __ATOMIC_SEQ_CST);
}

uint32_t prx_resolve_export(uint32_t nid)
{
    const uint32_t slot = (nid * 0x9E3779B1u) >> 20;
    const uint64_t cached = s_resolve_cache[slot];
    if (cached && (uint32_t)cached == nid)
        return (uint32_t)(cached >> 32);
    const uint32_t generation = __atomic_load_n(&s_resolve_generation, __ATOMIC_SEQ_CST);
    uint32_t guest_addr = 0;
    for (uint32_t i = 0; i < s_export_reg_n; i++)
        if (s_export_reg[i].nid == nid) {
            guest_addr = s_export_reg[i].guest_addr;
            break;
        }
    if (!(generation & 1u) &&
        __atomic_load_n(&s_resolve_generation, __ATOMIC_SEQ_CST) == generation) {
        s_resolve_cache[slot] = ((uint64_t)guest_addr << 32) | nid;
        /* A registration that began after the check may already have cleared. */
        if (__atomic_load_n(&s_resolve_generation, __ATOMIC_SEQ_CST) != generation)
            s_resolve_cache[slot] = 0;
    }
    return guest_addr;
}

uint32_t prx_export_registry_count(void) { return s_export_reg_n; }

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    r.base       = m->base;
    r.image_size = m->image_size;

    /* Step A — place the relocated image into guest RAM at the load base.
     * The bytes already carry base-applied addresses (prx_relocate.py), so the
     * module's data, OPDs and pointer tables are immediately consistent with
     * the lifted code and the OPD-walking dispatcher. */
    memcpy(vm_base + m->base, m->image, m->image_size);

    /* Step B — register every lifted function at its guest address. The lift
     * baked the load base in (--base B), so e->addr is already B + offset and
     * needs no adjustment here. A null host function (rare: a table hole) is
     * skipped rather than registered. */
    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    /* Step C — publish exports as NID -> (base + vaddr) so a title's import
     * resolver can dispatch into this module by NID. Function exports point at
     * an OPD in the freshly-placed image; ps3_indirect_call walks it. */
    uint32_t exports_published = 0;
    if (m->exports) {
        for (uint32_t i = 0; i < m->export_count; i++) {
            const prx_export* e = &m->exports[i];
            if (e->nid == 0)
                continue;
            prx_export_register(e->nid, m->base + e->vaddr);
            exports_published++;
        }
    }

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}
