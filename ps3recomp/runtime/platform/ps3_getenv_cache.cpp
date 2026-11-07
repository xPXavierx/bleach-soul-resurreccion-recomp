/*
 * Lock-free getenv for the whole executable (link with -Wl,--wrap=getenv,
 * --wrap=_putenv_s,--wrap=_putenv).
 *
 * The UCRT getenv takes the process-wide environment lock and scans every
 * variable on each call. The runtime reads diagnostic switches with getenv on
 * per-job and per-DMA paths, so SPU job threads serialized on that lock: with
 * eight geometry lanes a third of their busy time was spent waiting for it.
 *
 * Each distinct name is resolved once and kept as an immutable node, so a
 * lookup is a hash and a string compare. The few in-process writers
 * (_putenv/_putenv_s, used at startup) clear the table; a reader racing such a
 * write may still return the previous value, which is the same answer it could
 * have received a moment earlier. Returned strings are never freed.
 */
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <mutex>

extern "C" char* __real_getenv(const char* name);
extern "C" int __real__putenv(const char* assignment);
extern "C" int __real__putenv_s(const char* name, const char* value);

namespace {
struct Node { const char* name; char* value; };
constexpr unsigned kSlots = 4096;
std::atomic<Node*> slots[kSlots];
std::mutex fill_lock;

uint32_t hash_name(const char* s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

char* copy_or_null(const char* s)
{
    if (!s) return nullptr;
    const size_t n = strlen(s) + 1;
    char* p = static_cast<char*>(malloc(n));
    if (p) memcpy(p, s, n);
    return p;
}

void clear_all()
{
    std::lock_guard<std::mutex> guard(fill_lock);
    for (auto& slot : slots) slot.store(nullptr, std::memory_order_release);
}
}

extern "C" char* __wrap_getenv(const char* name)
{
    if (!name) return nullptr;
    const uint32_t h = hash_name(name);
    for (unsigned probe = 0; probe < kSlots; probe++) {
        Node* node = slots[(h + probe) & (kSlots - 1)].load(std::memory_order_acquire);
        if (!node) break;
        if (!strcmp(node->name, name)) return node->value;
    }
    std::lock_guard<std::mutex> guard(fill_lock);
    for (unsigned probe = 0; probe < kSlots; probe++) {
        auto& slot = slots[(h + probe) & (kSlots - 1)];
        Node* node = slot.load(std::memory_order_acquire);
        if (node && !strcmp(node->name, name)) return node->value;
        if (node) continue;
        Node* created = new Node{copy_or_null(name), copy_or_null(__real_getenv(name))};
        slot.store(created, std::memory_order_release);
        return created->value;
    }
    return __real_getenv(name);   /* table full: fall back to the CRT */
}

extern "C" int __wrap__putenv(const char* assignment)
{
    const int rc = __real__putenv(assignment);
    clear_all();
    return rc;
}

extern "C" int __wrap__putenv_s(const char* name, const char* value)
{
    const int rc = __real__putenv_s(name, value);
    clear_all();
    return rc;
}
