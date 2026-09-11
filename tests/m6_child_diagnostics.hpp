#pragma once

// Test-only Win32 boundary. No production hooks, SDK access, or heap use in Mark.
#include <Windows.h>
#include <cstddef>
#include <type_traits>

namespace m6_diagnostics {

enum class Phase : LONG {
    none, started, pre_injection, before_handle_call, handler_backend_entered,
    handle_returned, response_serialization_validated, empty_fallback,
    normal_return_selected, before_server_call
};

struct SharedTrace {
    DWORD magic{0x4D365431U};
    DWORD version{1U};
    DWORD size{sizeof(SharedTrace)};
    alignas(LONG) volatile LONG last_phase{};
    volatile LONG visited{};
};
static_assert(std::is_standard_layout_v<SharedTrace>);
static_assert(std::is_trivially_destructible_v<SharedTrace>);
static_assert(offsetof(SharedTrace, last_phase) % alignof(LONG) == 0);
static_assert(offsetof(SharedTrace, visited) % alignof(LONG) == 0);

inline SharedTrace* trace{}; // Set before launching any test worker; cleared after join.
inline thread_local bool observing_handle{};

inline bool Valid(const SharedTrace* value) noexcept {
    return value && value->magic == 0x4D365431U && value->version == 1U &&
        value->size == sizeof(SharedTrace);
}

inline void Mark(Phase phase) noexcept {
    if (!trace) return;
    // Only aligned Win32 interlocked operations while allocation faults are armed.
    InterlockedOr(&trace->visited, 1L << static_cast<LONG>(phase));
    InterlockedExchange(&trace->last_phase, static_cast<LONG>(phase));
}

inline LONG Read(volatile LONG& value) noexcept {
    return InterlockedCompareExchange(&value, 0, 0);
}

inline void BeforeHandle() noexcept {
    observing_handle = true;
    Mark(Phase::before_handle_call);
}
inline void BackendEntered() noexcept {
    if (observing_handle) Mark(Phase::handler_backend_entered);
}
inline void HandleReturned() noexcept {
    Mark(Phase::handle_returned);
    observing_handle = false;
}

inline const char* PhaseName(LONG phase) noexcept {
    switch (static_cast<Phase>(phase)) {
    case Phase::none: return "none";
    case Phase::started: return "started";
    case Phase::pre_injection: return "pre_injection";
    case Phase::before_handle_call: return "before_handle_call";
    case Phase::handler_backend_entered: return "handler_backend_entered";
    case Phase::handle_returned: return "handle_returned";
    case Phase::response_serialization_validated: return "response_serialization_validated";
    case Phase::empty_fallback: return "empty_fallback";
    case Phase::normal_return_selected: return "normal_return_selected";
    case Phase::before_server_call: return "before_server_call";
    }
    return "invalid";
}

struct WaitResult {
    bool attempted{};
    DWORD value{WAIT_FAILED};
    DWORD error{};
};
struct BoolResult {
    bool attempted{};
    bool succeeded{};
    DWORD error{};
};
struct ExitResult {
    BoolResult query;
    DWORD code{STILL_ACTIVE};
};
struct Outcome {
    WaitResult wait;
    BoolResult termination;
    DWORD requested_exit{};
    WaitResult drain;
    ExitResult exit;

    bool ExitKnown() const noexcept {
        return exit.query.succeeded && exit.code != STILL_ACTIVE;
    }
    bool CleanupComplete() const noexcept {
        return ExitKnown() && ((wait.attempted && wait.value == WAIT_OBJECT_0) ||
            (drain.attempted && drain.value == WAIT_OBJECT_0));
    }
    bool CompletedWithoutIntervention() const noexcept {
        return wait.attempted && wait.value == WAIT_OBJECT_0 && CleanupComplete();
    }
};

inline const char* WaitName(WaitResult wait) noexcept {
    if (!wait.attempted) return "not_attempted";
    switch (wait.value) {
    case WAIT_OBJECT_0: return "WAIT_OBJECT_0";
    case WAIT_TIMEOUT: return "WAIT_TIMEOUT";
    case WAIT_FAILED: return "WAIT_FAILED";
    default: return "unexpected";
    }
}

// The same seam is called by the real child launcher and deterministic tests.
// The original 10-second wait and 5-second cleanup bound are deliberately fixed.
template <class Api>
Outcome Observe(Api& api) {
    Outcome result;
    result.wait = api.Wait(10000U);
    if (result.wait.value != WAIT_OBJECT_0) {
        result.requested_exit = result.wait.value == WAIT_TIMEOUT ? 124U :
            (result.wait.value == WAIT_FAILED ? 125U : 126U);
        result.termination = api.Terminate(result.requested_exit);
        // Even a failed terminate may race with natural process exit. Observe it.
        result.drain = api.Wait(5000U);
    }
    result.exit = api.QueryExit();
    return result;
}

struct Win32Api {
    HANDLE process{};
    WaitResult Wait(DWORD milliseconds) const noexcept {
        const DWORD value = WaitForSingleObject(process, milliseconds);
        const DWORD error = value == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        return {true, value, error};
    }
    BoolResult Terminate(DWORD exit_code) const noexcept {
        const BOOL succeeded = TerminateProcess(process, exit_code);
        const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
        return {true, succeeded != FALSE, error};
    }
    ExitResult QueryExit() const noexcept {
        DWORD code = STILL_ACTIVE;
        const BOOL succeeded = GetExitCodeProcess(process, &code);
        const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
        return {{true, succeeded != FALSE, error}, code};
    }
};

struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct View {
    SharedTrace* value{};
    ~View() { if (value) UnmapViewOfFile(value); }
};
struct AttributeList {
    LPPROC_THREAD_ATTRIBUTE_LIST value{};
    bool initialized{};
    ~AttributeList() {
        if (initialized) DeleteProcThreadAttributeList(value);
        if (value) HeapFree(GetProcessHeap(), 0, value);
    }
};

} // namespace m6_diagnostics
