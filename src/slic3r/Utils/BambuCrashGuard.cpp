#include "BambuCrashGuard.hpp"

#if defined(_MSC_VER) || defined(_WIN32)

#include <boost/log/trivial.hpp>
#include <sstream>
#include <iomanip>
#include <mutex>

namespace Slic3r {

// Static member initialization
uintptr_t             BambuCrashGuard::s_dll_base = 0;
uintptr_t             BambuCrashGuard::s_dll_end = 0;
void                 *BambuCrashGuard::s_veh_handle = nullptr;
std::atomic<int>      BambuCrashGuard::s_crash_count{0};
std::string           BambuCrashGuard::s_last_crash_summary;

// Mutex for crash summary string (VEH can fire from any thread)
static std::mutex s_summary_mutex;

void BambuCrashGuard::install(HMODULE module_handle)
{
    if (!module_handle) return;
    if (s_veh_handle) {
        BOOST_LOG_TRIVIAL(warning) << "BambuCrashGuard: already installed, skipping";
        return;
    }

    // Get the DLL's base address and size from its PE headers
    s_dll_base = reinterpret_cast<uintptr_t>(module_handle);

    // Parse the PE header to find the image size
    auto *dos_header = reinterpret_cast<IMAGE_DOS_HEADER *>(module_handle);
    if (dos_header->e_magic == IMAGE_DOS_SIGNATURE) {
        auto *nt_headers = reinterpret_cast<IMAGE_NT_HEADERS *>(
            s_dll_base + dos_header->e_lfanew);
        if (nt_headers->Signature == IMAGE_NT_SIGNATURE) {
            s_dll_end = s_dll_base + nt_headers->OptionalHeader.SizeOfImage;
        }
    }

    if (s_dll_end <= s_dll_base) {
        // Fallback: assume a generous size (256 MB should cover any DLL)
        s_dll_end = s_dll_base + 0x10000000;
        BOOST_LOG_TRIVIAL(warning) << "BambuCrashGuard: could not parse PE header, "
            << "using fallback range";
    }

    // Install VEH as FIRST handler (1 = first in chain)
    s_veh_handle = AddVectoredExceptionHandler(1, vectored_handler);

    if (s_veh_handle) {
        BOOST_LOG_TRIVIAL(info) << "BambuCrashGuard: installed VEH, "
            << "DLL range [0x" << std::hex << s_dll_base
            << " - 0x" << s_dll_end << std::dec << "]";
    } else {
        BOOST_LOG_TRIVIAL(error) << "BambuCrashGuard: AddVectoredExceptionHandler failed";
    }
}

void BambuCrashGuard::uninstall()
{
    if (s_veh_handle) {
        RemoveVectoredExceptionHandler(s_veh_handle);
        s_veh_handle = nullptr;
        s_dll_base = 0;
        s_dll_end = 0;
        BOOST_LOG_TRIVIAL(info) << "BambuCrashGuard: uninstalled VEH";
    }
}

bool BambuCrashGuard::has_caught_crash()
{
    return s_crash_count.load() > 0;
}

int BambuCrashGuard::crash_count()
{
    return s_crash_count.load();
}

std::string BambuCrashGuard::last_crash_summary()
{
    std::lock_guard<std::mutex> lock(s_summary_mutex);
    return s_last_crash_summary;
}

// Trampoline: the crashed thread's RIP is redirected here.
// This function runs on the crashed thread's stack. It cleanly
// exits just this thread without tearing down the process.
void NTAPI BambuCrashGuard::thread_exit_trampoline()
{
    // ExitThread terminates only the current thread.
    // Using exit code 0xDEAD to make it identifiable in logs.
    ExitThread(0xDEAD);
}

LONG CALLBACK BambuCrashGuard::vectored_handler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only handle access violations and stack overflows
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != STATUS_STACK_BUFFER_OVERRUN)
        return EXCEPTION_CONTINUE_SEARCH;

    // Check if the faulting instruction is inside the Bambu DLL
    uintptr_t rip = ep->ContextRecord->Rip;
    if (rip < s_dll_base || rip >= s_dll_end)
        return EXCEPTION_CONTINUE_SEARCH; // Not in the Bambu DLL — let normal handling proceed

    // This crash IS in the Bambu DLL. Suppress it by redirecting to our trampoline.
    int count = ++s_crash_count;

    // Build crash summary (best-effort, keep it simple for VEH context)
    {
        std::lock_guard<std::mutex> lock(s_summary_mutex);
        std::ostringstream oss;
        oss << "Bambu DLL crash #" << count
            << " caught and suppressed. "
            << "Exception 0x" << std::hex << code
            << " at RIP 0x" << rip
            << " (DLL offset 0x" << (rip - s_dll_base) << ")"
            << std::dec
            << " on thread " << GetCurrentThreadId();
        s_last_crash_summary = oss.str();
    }

    // Log it (BOOST_LOG may not be fully safe in VEH but works in practice)
    BOOST_LOG_TRIVIAL(error) << "BambuCrashGuard: " << s_last_crash_summary;

    // Redirect the thread to our exit trampoline.
    // When the exception handler returns EXCEPTION_CONTINUE_EXECUTION,
    // Windows resumes the thread at the modified RIP — which is our
    // trampoline that calls ExitThread().
    ep->ContextRecord->Rip = reinterpret_cast<DWORD64>(thread_exit_trampoline);

    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace Slic3r

#endif // _WIN32
