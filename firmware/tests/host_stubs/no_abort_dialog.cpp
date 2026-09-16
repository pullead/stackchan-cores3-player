// Make a failing assert() print and exit instead of opening a modal dialog.
//
// On MSVC debug builds, abort() pops up the "Debug Error!" window and waits for
// a human to click it.  That turns any red test into a hung ctest run, which is
// useless for automation.  This translation unit runs before main() and routes
// assertion failures to stderr with a non-zero exit code instead.
#if defined(_MSC_VER)

#include <crtdbg.h>
#include <stdlib.h>

namespace {

int configure_silent_abort() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    return 0;
}

// The CRT calls every function pointer in the .CRT$XCU section before main().
#pragma section(".CRT$XCU", read)
int silent_abort_configured = configure_silent_abort();
__declspec(allocate(".CRT$XCU")) int (*silent_abort_initializer)() = configure_silent_abort;

}  // namespace

#endif  // _MSC_VER
