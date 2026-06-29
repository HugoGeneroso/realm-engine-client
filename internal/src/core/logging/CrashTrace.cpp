#include "pch-il2cpp.h"
#include "CrashTrace.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace CrashTrace {

namespace {

constexpr int kRingSize = 64;

struct RingEntry {
    ULONGLONG tickMs = 0;
    DWORD     tid    = 0;
    char      text[160]{};
};

std::atomic<uint32_t> g_ringHead{ 0 };
RingEntry             g_ring[kRingSize];

char            g_presentStep[96]{};
std::atomic<uint32_t> g_presentFrame{ 0 };

void WriteCrashReport(EXCEPTION_POINTERS* ep, const char* reason)
{
    char local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
    char path[MAX_PATH]{};
    if (n > 0 && n < sizeof(local))
        std::snprintf(path, sizeof(path), "%s\\RotMG Exalt DLL Crash.log", local);
    else
        std::snprintf(path, sizeof(path), "C:\\RotMG Exalt DLL Crash.log");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "=== RealmEngine DLL crash report ===\n");
    fprintf(f, "time=%04u-%02u-%02u %02u:%02u:%02u.%03u reason=%s tid=%lu\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            reason ? reason : "?", GetCurrentThreadId());

    if (ep && ep->ExceptionRecord) {
        fprintf(f, "exception=0x%08lX at %p\n",
                static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress);
    }

    fprintf(f, "lastPresentStep=%s frame=%u\n",
            g_presentStep[0] ? g_presentStep : "(none)",
            g_presentFrame.load(std::memory_order_relaxed));

    const uint32_t head = g_ringHead.load(std::memory_order_acquire);
    const uint32_t count = head < static_cast<uint32_t>(kRingSize) ? head : kRingSize;
    const uint32_t start = head - count;
    fprintf(f, "--- ring (oldest -> newest, %u entries) ---\n", count);
    for (uint32_t i = start; i < head; ++i) {
        const RingEntry& e = g_ring[i % kRingSize];
        if (!e.text[0]) continue;
        fprintf(f, "[%llu tid=%lu] %s\n",
                static_cast<unsigned long long>(e.tickMs), e.tid, e.text);
    }
    fflush(f);
    fclose(f);
}

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool fatal = (code == EXCEPTION_ACCESS_VIOLATION)
                    || (code == EXCEPTION_ILLEGAL_INSTRUCTION)
                    || (code == EXCEPTION_STACK_OVERFLOW)
                    || (code == EXCEPTION_INT_DIVIDE_BY_ZERO)
                    || (code == EXCEPTION_PRIV_INSTRUCTION);
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;

    WriteCrashReport(ep, "VEH");
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install()
{
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;
    AddVectoredExceptionHandler(1, VectoredHandler);
    Push("CrashTrace installed");
}

void Push(const char* msg)
{
    if (!msg || !msg[0]) return;
    const uint32_t slot = g_ringHead.fetch_add(1, std::memory_order_relaxed) % kRingSize;
    RingEntry& e = g_ring[slot];
    e.tickMs = GetTickCount64();
    e.tid    = GetCurrentThreadId();
    std::snprintf(e.text, sizeof(e.text), "%s", msg);
}

void SetPresentStep(const char* step, uint32_t frame)
{
    g_presentFrame.store(frame, std::memory_order_relaxed);
    if (!step) {
        g_presentStep[0] = '\0';
        return;
    }
    std::snprintf(g_presentStep, sizeof(g_presentStep), "%s", step);
}

} // namespace CrashTrace
