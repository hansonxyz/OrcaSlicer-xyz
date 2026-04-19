#pragma once

// xyz fork: Vectored Exception Handler that catches crashes in the Bambu
// networking DLL's background threads and terminates just the offending
// thread instead of the entire process.
//
// The Bambu DLL (bambu_networking_*.dll) creates its own background threads
// for MQTT, SSDP, and other networking tasks. These threads occasionally
// crash with ACCESS_VIOLATION due to bugs in the closed-source DLL. Without
// this guard, the crash tears down the entire OrcaSlicer process.
//
// How it works:
// 1. After LoadLibrary, we compute the DLL's address range [base, base+size)
// 2. We install a Vectored Exception Handler (runs before SEH)
// 3. On ACCESS_VIOLATION, we check if the faulting RIP is inside the DLL
// 4. If so, we redirect the thread's instruction pointer to a trampoline
//    that calls ExitThread(1), killing only that thread
// 5. The main OrcaSlicer process continues running
//
// Limitations:
// - After a caught crash, networking functionality may be degraded
// - Any mutexes held by the crashed thread remain locked (potential deadlocks)
// - This is a safety net, not a fix for the underlying DLL bugs

#if defined(_MSC_VER) || defined(_WIN32)

#include <Windows.h>
#include <atomic>
#include <string>

namespace Slic3r {

class BambuCrashGuard {
public:
    // Install the VEH after loading the Bambu DLL.
    // module_handle: the HMODULE returned by LoadLibrary
    static void install(HMODULE module_handle);

    // Remove the VEH (call before unloading the DLL or at shutdown).
    static void uninstall();

    // Check if a crash has been caught and suppressed.
    static bool has_caught_crash();

    // Get the count of suppressed crashes.
    static int crash_count();

    // Get a human-readable summary of the last caught crash.
    static std::string last_crash_summary();

private:
    // The VEH callback
    static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep);

    // Trampoline function — the crashed thread is redirected here
    static void NTAPI thread_exit_trampoline();

    // DLL address range
    static uintptr_t s_dll_base;
    static uintptr_t s_dll_end;

    // VEH handle
    static void *s_veh_handle;

    // Crash tracking
    static std::atomic<int> s_crash_count;
    static std::string s_last_crash_summary;
};

} // namespace Slic3r

#endif // _WIN32
