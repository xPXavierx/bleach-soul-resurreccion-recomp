#ifndef BLEACH_NATIVE_FRAME_CLOCK_H
#define BLEACH_NATIVE_FRAME_CLOCK_H

#include <stdint.h>
#include <stdlib.h>

/* Integer rational deadlines: no 16 ms approximation and no accumulated
 * rounding error. This clock controls simulation pulses, never GPU completion.
 * The caller owns catch-up policy and must advance once per delivered pulse. */
struct BleachClockDeadline {
    uint64_t deadline, quotient, remainder, carry;
    uint32_t rate;
    BleachClockDeadline(uint64_t now, uint64_t frequency, uint32_t hz)
        : deadline(now), quotient(frequency / hz), remainder(frequency % hz),
          carry(0), rate(hz) {}
    void advance() {
        deadline += quotient;
        carry += remainder;
        if (carry >= rate) { ++deadline; carry -= rate; }
    }
};

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

/* A dedicated sleeping clock. Render command encoding and Present cannot hold
 * up its deadlines. Callback delivery remains serialized by the GCM runtime.
 * No graphics calls or artificial frame completions belong on this thread. */
class BleachNativeFrameClock {
public:
    using Tick = void (*)(void*);
    bool start(Tick tick, void* user) {
        if (thread_) return false;
        tick_ = tick; user_ = user;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!stop_ || !timer_) { release(); return false; }
        thread_ = CreateThread(nullptr, 4u * 1024 * 1024, run, this, 0, nullptr);
        if (!thread_) { release(); return false; }
        return true;
    }
    void stop() {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_); thread_ = nullptr;
            fprintf(stderr, "[native-clock] stopped: ticks=%llu resyncs=%llu max_late_ms=%.3f\n",
                (unsigned long long)ticks_, (unsigned long long)resyncs_, max_late_ms_);
        }
        release();
    }
    ~BleachNativeFrameClock() { stop(); }
    BleachNativeFrameClock() = default;
    BleachNativeFrameClock(const BleachNativeFrameClock&) = delete;
    BleachNativeFrameClock& operator=(const BleachNativeFrameClock&) = delete;
private:
    HANDLE stop_ = nullptr, timer_ = nullptr, thread_ = nullptr;
    Tick tick_ = nullptr;
    void* user_ = nullptr;
    uint64_t ticks_ = 0, resyncs_ = 0;
    double max_late_ms_ = 0;
    void release() {
        if (timer_) { CancelWaitableTimer(timer_); CloseHandle(timer_); timer_ = nullptr; }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    }
    static DWORD WINAPI run(void* arg) {
        auto& self = *static_cast<BleachNativeFrameClock*>(arg);
        LARGE_INTEGER frequency, current;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&current);
        BleachClockDeadline clock(current.QuadPart, frequency.QuadPart, 60);
        HANDLE waits[] = {self.stop_, self.timer_};
        fprintf(stderr, "[native-clock] independent 60 Hz simulation pulses; render pacing is separate\n");
        for (;;) {
            if (WaitForSingleObject(self.stop_, 0) == WAIT_OBJECT_0) return 0;
            QueryPerformanceCounter(&current);
            if ((uint64_t)current.QuadPart < clock.deadline) {
                const uint64_t left = clock.deadline - current.QuadPart;
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((left * 10000000ull + frequency.QuadPart - 1) / frequency.QuadPart);
                if (!SetWaitableTimer(self.timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    fprintf(stderr, "[native-clock] timer failed: %lu\n", GetLastError());
                    abort();
                }
                const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) return 0;
                if (result != WAIT_OBJECT_0 + 1) abort();
                continue; // Recheck QPC: a timer signal alone is not a deadline.
            }
            const uint64_t late = current.QuadPart - clock.deadline;
            const double late_ms = (double)late * 1000.0 / frequency.QuadPart;
            if (late_ms > self.max_late_ms_) self.max_late_ms_ = late_ms;
            // Suspend/debugger gaps do not issue minutes of stale callbacks.
            // Ordinary lateness retains its phase and delivers every pulse.
            if (late > (uint64_t)frequency.QuadPart * 4) {
                clock = BleachClockDeadline(current.QuadPart, frequency.QuadPart, 60);
                ++self.resyncs_;
            }
            self.tick_(self.user_);
            ++self.ticks_;
            clock.advance();
        }
    }
};
#endif
#endif
