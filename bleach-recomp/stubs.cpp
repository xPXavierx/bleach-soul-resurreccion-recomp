// Bleach BLUS30769 host integration. No game code is stored in this file.
#include "ppu_recomp.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void ps3_hle_register_ctx(unsigned, const char*, void (*)(ppu_context*));
extern "C" int _cellSpursJobChainAttributeInitialize(unsigned, unsigned, uint64_t,
    uint64_t, unsigned, unsigned, uint64_t, unsigned);
static std::atomic<unsigned> spu_printf_callbacks[4];

static void printf_initialize(ppu_context* ctx)
{
    for (unsigned i = 0; i < 4; ++i)
        spu_printf_callbacks[i].store((unsigned)ctx->gpr[3+i]);
    fprintf(stderr, "[bleach] SPU printf callbacks registered\n");
    ctx->gpr[3] = 0;
}

static void printf_finalize(ppu_context* ctx)
{
    for (auto& callback : spu_printf_callbacks) callback.store(0);
    ctx->gpr[3] = 0;
}

static void jobchain_attribute_initialize(ppu_context* ctx)
{
    const unsigned attr = (unsigned)ctx->gpr[5];
    if (!attr || (attr & 7)) {
        ctx->gpr[3] = attr ? 0x80410A10u : 0x80410A11u;
        return;
    }
    int rc = _cellSpursJobChainAttributeInitialize((unsigned)ctx->gpr[3],
        (unsigned)ctx->gpr[4], attr, ctx->gpr[6], (unsigned)ctx->gpr[7],
        (unsigned)ctx->gpr[8], ctx->gpr[9], (unsigned)ctx->gpr[10]);
    // PPC64 ABI: overflow arguments start after linkage (48) + 8 GPR slots.
    const unsigned stack = (unsigned)ctx->gpr[1] + 0x70;
    if (!rc) {
        vm_write8(attr + 0x1c, (unsigned)vm_read64(stack) != 0);
        vm_write32(attr + 0x20, (unsigned)vm_read64(stack + 8));
        vm_write32(attr + 0x24, (unsigned)vm_read64(stack + 16));
        vm_write8(attr + 0x28, (unsigned)vm_read64(stack + 24) != 0);
        vm_write32(attr + 0x2c, (unsigned)vm_read64(stack + 32));
        vm_write32(attr + 0x30, (unsigned)vm_read64(stack + 40));
        vm_write8(attr + 0x3c, 0);
    }
    ctx->gpr[3] = (unsigned)rc;
}

static void jobchain_halt_on_error(ppu_context* ctx)
{
    const unsigned attr = (unsigned)ctx->gpr[3];
    if (!attr || (attr & 7)) {
        ctx->gpr[3] = attr ? 0x80410A10u : 0x80410A11u;
        return;
    }
    vm_write32(attr + 0x34, 1);
    ctx->gpr[3] = 0;
}

#ifdef _WIN32
// cellL10n SJISstoUTF8s(src, s32* src_len, dst, s32* dst_len): Shift-JIS bytes to UTF-8
// (no terminator). *dst_len is the capacity in, the produced length out; a null dst only
// measures. 0 = ConversionOK, 1 = SRCIllegal, 2 = DSTExhausted. Reached from the
// Collection menu, which stopped the title as an unresolved import.
static void sjis_to_utf8s(ppu_context* ctx)
{
    const unsigned src = (unsigned)ctx->gpr[3], src_len = (unsigned)ctx->gpr[4];
    const unsigned dst = (unsigned)ctx->gpr[5], dst_len = (unsigned)ctx->gpr[6];
    if (!src || !src_len || !dst_len) { ctx->gpr[3] = 1; return; }
    const int in = (int)vm_read32(src_len);
    if (in <= 0) { vm_write32(dst_len, 0); ctx->gpr[3] = 0; return; }
    std::string bytes(in, '\0');
    for (int i = 0; i < in; ++i) bytes[i] = (char)vm_read8(src + i);
    const int wide = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), in, nullptr, 0);
    if (wide <= 0) { ctx->gpr[3] = 1; return; }
    std::wstring w(wide, L'\0');
    MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(), in, &w[0], wide);
    const int out = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string utf8(out, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, &utf8[0], out, nullptr, nullptr);
    if (dst) {
        if (out > (int)vm_read32(dst_len)) { ctx->gpr[3] = 2; return; }
        for (int i = 0; i < out; ++i) vm_write8(dst + i, (uint8_t)utf8[i]);
    }
    vm_write32(dst_len, (unsigned)out);
    ctx->gpr[3] = 0;
}
#endif

// Imports reachable from menus that would otherwise stop the title as unresolved
// (BLEACH_IMPORT_AUDIT). No online play: only "offline" answers and a plain OK dialog.
extern "C" void ps3_hle_register(unsigned, const char*, void*);
extern "C" int cellSaveDataFixedDelete(void*, void*, void*, void*, unsigned, void*);
extern "C" int cellMsgDialogOpen2(unsigned, const char*, void*, void*, void*);

// cellMsgDialogOpenErrorCode(errorNum, callback, userData, extParam): the system error
// dialog (network failures and the like) as an OK dialog showing the code.
static void msg_dialog_error_code(ppu_context* ctx)
{
    const unsigned text = (unsigned)ctx->gpr[1] - 0x200;   // below the caller's frame
    char msg[64]; snprintf(msg, sizeof msg, "An error occurred. (%08X)", (unsigned)ctx->gpr[3]);
    for (unsigned i = 0; i <= strlen(msg); ++i) vm_write8(text + i, (uint8_t)msg[i]);
    ctx->gpr[3] = (unsigned)cellMsgDialogOpen2(0x20u /* SE_TYPE_ERROR | BUTTON_TYPE_OK */,
        (const char*)(uintptr_t)text, (void*)(uintptr_t)(unsigned)ctx->gpr[4],
        (void*)(uintptr_t)(unsigned)ctx->gpr[5], (void*)(uintptr_t)(unsigned)ctx->gpr[6]);
}

// sceNpProfileCallGui(npId, handler, arg, options): PSN profile viewer. Offline.
static void np_profile_call_gui(ppu_context* ctx) { ctx->gpr[3] = 0x8002AA0Cu; /* SCE_NP_ERROR_OFFLINE */ }

extern "C" void bleach_register_sail(void);

extern "C" void bleach_register_hle(void)
{
    ps3_hle_register_ctx(0x45FE2FCE, "_sys_spu_printf_initialize", printf_initialize);
    ps3_hle_register_ctx(0xDD3B27AC, "_sys_spu_printf_finalize", printf_finalize);
    ps3_hle_register_ctx(0xBB68D76E, "cellSpursJobChainAttributeSetHaltOnError", jobchain_halt_on_error);
    ps3_hle_register_ctx(0x3548F483, "_cellSpursJobChainAttributeInitialize", jobchain_attribute_initialize);
#ifdef _WIN32
    ps3_hle_register_ctx(0x33435818, "SJISstoUTF8s", sjis_to_utf8s);
#endif
    ps3_hle_register(0xCED17573, "cellSaveDataFixedDelete", (void*)cellSaveDataFixedDelete);
    ps3_hle_register_ctx(0x3E22CB4B, "cellMsgDialogOpenErrorCode", msg_dialog_error_code);
    ps3_hle_register_ctx(0xCEEEBC7A, "sceNpProfileCallGui", np_profile_call_gui);
    bleach_register_sail();
}
