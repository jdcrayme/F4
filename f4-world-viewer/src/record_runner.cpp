// f4-world-viewer/src/record_runner.cpp
//
// MISSION-QC-RECORD: the record spawn (see record_runner.hpp for why
// this is its own TU — windows.h vs raylib.h).

#include "record_runner.hpp"

#include <cstdio>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace f4::viewer {

namespace {

namespace fs = std::filesystem;

// Append one verdict line to the run log (best-effort — the log is a
// diagnostic, not a product; a failed open just skips it).
void append_result(const fs::path& log_path, const std::string& line) {
#ifdef _WIN32
    // FILE_APPEND_DATA alone = true append-at-EOF semantics. Adding
    // FILE_WRITE_DATA silently disables the append behavior and writes
    // land at the (zero-initialized) file pointer instead — the log
    // overwrote itself head-first until this was FILE_APPEND_DATA only.
    HANDLE h = CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
#else
    (void)log_path;
    (void)line;
#endif
}

} // namespace

int run_recorder(const std::filesystem::path& tool,
                 const std::filesystem::path& scenario,
                 const std::filesystem::path& out_dir,
                 std::string* diag) {
#ifdef _WIN32
    // The child's console output has nowhere to go (the parent is a
    // windowless GUI process) — route it into a log beside the out-dir
    // so a failed record keeps its recorder messages for the status
    // line / the user.
    const fs::path log_path =
        out_dir.parent_path() / (out_dir.filename().wstring() + L".log");
    std::error_code mk_ec;
    fs::create_directories(out_dir.parent_path(), mk_ec);
    append_result(log_path, "spawn: \"" + tool.string() + "\" --scenario \"" +
                                scenario.string() + "\" --out-dir \"" +
                                out_dir.string() + "\"\n");
    // Append-only desired access so the child's buffered CRT writes and
    // this process's verdict line can never overwrite each other's bytes
    // (inherited handles share one file pointer; FILE_APPEND_DATA forces
    // every write to the current EOF regardless). The handle must be
    // created INHERITABLE (bInheritHandle=TRUE) — bInheritHandles=TRUE
    // on CreateProcessW only duplicates handles so marked, and without
    // it the child's stdout/stderr silently go nowhere.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log_h = CreateFileW(log_path.c_str(),
                               FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool logging = log_h != INVALID_HANDLE_VALUE;

    std::wstring cmd = L"\"" + tool.wstring() + L"\" --scenario \"" +
                       scenario.wstring() + L"\" --out-dir \"" +
                       out_dir.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (logging) {
        // The child inherits the log handle as its stdout/stderr — the
        // recorder's gate messages land in the log even though the
        // parent has no console.
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = log_h;
        si.hStdError = log_h;
    }
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: a GUI app spawning a console host would own a
    // taskbar blip for the run's duration otherwise.
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                   logging, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    if (log_h != INVALID_HANDLE_VALUE) CloseHandle(log_h);
    if (!ok) {
        const DWORD err = GetLastError();
        if (diag) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "CreateProcessW failed (err %lu)",
                          static_cast<unsigned long>(err));
            *diag = buf;
        }
        append_result(log_path, "spawn failed, err " +
                                    std::to_string(static_cast<unsigned long>(err)) +
                                    "\n");
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    append_result(log_path, "exit " + std::to_string(code) + "\n");
    return static_cast<int>(code);
#else
    std::string cmd = "\"" + tool.string() + "\" --scenario \"" +
                      scenario.string() + "\" --out-dir \"" +
                      out_dir.string() + "\"";
    const int status = std::system(cmd.c_str());
    if (status == -1) {
        if (diag) *diag = "std::system failed";
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (diag) *diag = "child terminated abnormally";
    return -1;
#endif
}

} // namespace f4::viewer
