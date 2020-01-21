#include "utest.h"
#include <Windows.h>

int debugger_present()
{
  return IsDebuggerPresent();
}

#if defined ( _MSC_VER )
#  define DBG_TOOLS_BREAKPOINT __debugbreak()
#elif defined( __GNUC__ )
#  if defined(__i386__) || defined( __x86_64__ )
void inline __attribute__((always_inline)) _dbg_tools_gcc_break_helper() { __asm("int3"); }
#    define DBG_TOOLS_BREAKPOINT _dbg_tools_gcc_break_helper()
#  else
#    define DBG_TOOLS_BREAKPOINT __builtin_trap()
#  endif
#else
#  define DBG_TOOLS_BREAKPOINT exit(1)
#endif

UTEST_STATE();

static void setupconsole() 
{
/* Based on https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences */
#if defined(_WIN32) && !defined(_XBOX_VER)
  // Set output mode to handle virtual terminal sequences\colors
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }

  DWORD dwMode = 0;
  if (!GetConsoleMode(hOut, &dwMode)) {
    return;
  }

  dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  if (!SetConsoleMode(hOut, dwMode)) {
    return;
  }
#endif
}

int main(int argc, const char* const argv[]) 
{
  setupconsole();

  // do your own thing
  int result = utest_main(argc, argv);

  if (debugger_present()) {
    DBG_TOOLS_BREAKPOINT;
  }

  return result;
}
