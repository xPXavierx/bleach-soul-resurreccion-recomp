#ifndef PS3RECOMP_AUDIO_THREAD_H
#define PS3RECOMP_AUDIO_THREAD_H
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
/* Scope multimedia scheduling to threads that actually produce/consume audio.
 * A module handle stays alive until the matching task registration is reverted.
 * Loading only the system DLL avoids introducing a redistributable dependency. */
typedef struct {
    HMODULE module;
    HANDLE task;
    BOOL (WINAPI *revert)(HANDLE);
} AudioThreadClass;
static inline AudioThreadClass audio_thread_class_begin(const char* role)
{
    AudioThreadClass c = {0};
    c.module = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (c.module) {
        HANDLE (WINAPI *begin)(LPCWSTR, LPDWORD) =
            (HANDLE (WINAPI *)(LPCWSTR, LPDWORD))GetProcAddress(c.module, "AvSetMmThreadCharacteristicsW");
        BOOL (WINAPI *priority)(HANDLE, int) =
            (BOOL (WINAPI *)(HANDLE, int))GetProcAddress(c.module, "AvSetMmThreadPriority");
        c.revert = (BOOL (WINAPI *)(HANDLE))GetProcAddress(c.module, "AvRevertMmThreadCharacteristics");
        DWORD index = 0;
        if (begin && c.revert) c.task = begin(L"Pro Audio", &index);
        if (c.task && priority) priority(c.task, 1 /* AVRT_PRIORITY_HIGH */);
    }
    if (!c.task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    printf("[audio-scheduler] %s: %s\n", role, c.task ? "MMCSS Pro Audio" : "highest-priority fallback");
    return c;
}
static inline void audio_thread_class_end(AudioThreadClass* c)
{
    if (c->task) c->revert(c->task);
    if (c->module) FreeLibrary(c->module);
}
#endif
#endif
