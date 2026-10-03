// ECS 2-10: send CTRL_C_EVENT (0) or CTRL_BREAK_EVENT (1) to the console of another process, or (9) report
// whether that console has a selection (GetConsoleSelectionInfo; C10 checks its precondition with it).
// A helper of Tests\console_close_check.ps1 (built by that script into Tests\build\console_close\). Not a suite.
// usage: ConsoleCtrlSend <pid> <0|1|9> <result-file>
// Attaches to the target's console, makes itself immune, generates the event for every process on that console
// (process group 0), detaches. The result goes to a file (the console is not ours after FreeConsole).
#include <windows.h>
#include <cstdio>
#include <cstdlib>

static BOOL WINAPI Ignore(DWORD) { return TRUE; }   // this helper must not die from the signal it sends

int main(int argc, char** argv) {
  if (argc < 4) { return 1; }
  const DWORD pid = std::strtoul(argv[1], nullptr, 10);
  const DWORD ev  = std::strtoul(argv[2], nullptr, 10);
  FILE* out = nullptr;
  if (fopen_s(&out, argv[3], "w") != 0 || out == nullptr) { return 1; }
  ::FreeConsole();
  if (!::AttachConsole(pid)) {
    std::fprintf(out, "attach failed %lu\n", ::GetLastError());
    std::fclose(out);
    return 2;
  }
  if (ev == 9) {                           // query only: nothing is sent
    CONSOLE_SELECTION_INFO info{};
    const BOOL got = ::GetConsoleSelectionInfo(&info);
    const DWORD queryErr = ::GetLastError();
    ::FreeConsole();
    if (got) { std::fprintf(out, "selection flags 0x%lx\n", info.dwFlags); }
    else     { std::fprintf(out, "selection query failed %lu\n", queryErr); }
    std::fclose(out);
    return got ? 0 : 3;
  }
  ::SetConsoleCtrlHandler(Ignore, TRUE);   // after AttachConsole (it resets the handler list)
  const BOOL ok = ::GenerateConsoleCtrlEvent(ev, 0);
  const DWORD err = ::GetLastError();
  ::Sleep(200);                            // let the signal be delivered before we detach
  ::FreeConsole();
  std::fprintf(out, "generate %s (event %lu, error %lu)\n", ok ? "ok" : "failed", ev, ok ? 0UL : err);
  std::fclose(out);
  return ok ? 0 : 3;
}
