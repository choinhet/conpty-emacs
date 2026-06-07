// conpty-emacs: bridges a Windows pseudoconsole (ConPTY) to Emacs over stdin/
// stdout pipes, so term.el / eat / vterm can drive a real console program.
//
// Resize is sent out-of-band by Emacs inside the input stream as an APC string:
//     ESC _ "RESIZE;<cols>;<rows>" ESC '\'
// The bridge consumes those bytes (does not forward them to the PTY) and calls
// ResizePseudoConsole.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

// --------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------

// Move-only RAII wrapper for a Win32 HANDLE.
class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE h) : h_(h) {}
  ~Handle() { reset(); }

  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;

  Handle(Handle &&o) noexcept : h_(o.h_) { o.h_ = INVALID_HANDLE_VALUE; }
  Handle &operator=(Handle &&o) noexcept {
    if (this != &o) {
      reset();
      h_ = o.h_;
      o.h_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }

  HANDLE get() const { return h_; }
  HANDLE *put() { // for APIs that fill a HANDLE* (e.g. CreatePipe)
    reset();
    return &h_;
  }
  void reset(HANDLE h = INVALID_HANDLE_VALUE) {
    if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr)
      CloseHandle(h_);
    h_ = h;
  }

private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

HANDLE gLog = INVALID_HANDLE_VALUE; // enabled only if $CONPTY_LOG is set

void logLine(const std::string &msg) {
  if (gLog == INVALID_HANDLE_VALUE)
    return;
  std::string escaped;
  for (char c : msg) {
    if (c == '\r')
      escaped += "\\r";
    else if (c == '\n')
      escaped += "\\n";
    else
      escaped += c;
  }
  std::string line = "[DEBUG] " + escaped + "\n";
  DWORD written = 0;
  WriteFile(gLog, line.data(), static_cast<DWORD>(line.size()), &written,
            nullptr);
}

bool reportError(const wchar_t *what) {
  std::wcerr << what << L" failed: " << GetLastError() << L"\n";
  return false;
}

// --------------------------------------------------------------------------
// Argument parsing
// --------------------------------------------------------------------------

struct Args {
  std::wstring commandLine; // quoted program + extra args
  COORD size{};
};

bool parseArgs(int argc, wchar_t *argv[], Args &out) {
  if (argc < 4) {
    std::wcerr
        << L"Usage: conpty-emacs.exe <program> <cols> <rows> [extra args...]\n";
    return false;
  }
  out.size.X = static_cast<SHORT>(std::stoi(std::wstring(argv[2])));
  out.size.Y = static_cast<SHORT>(std::stoi(std::wstring(argv[3])));

  std::wstring cmd = L"\"" + std::wstring(argv[1]) + L"\"";
  for (int i = 4; i < argc; i++) {
    cmd += L" ";
    cmd += argv[i];
  }
  out.commandLine = std::move(cmd);
  return true;
}

// --------------------------------------------------------------------------
// Pseudoconsole creation
// --------------------------------------------------------------------------

struct PseudoConsole {
  HPCON handle = nullptr;
  Handle write; // our end: write client input here  -> ConPTY stdin
  Handle read;  // our end: read client output here  <- ConPTY stdout
};

bool createPseudoConsole(COORD size, PseudoConsole &out) {
  // ConPTY's ends of the pipes; closed once ConPTY has duplicated them.
  Handle ptyIn, ptyOut;

  // input pipe:  ConPTY reads ptyIn(read), we write out.write(write)
  if (!CreatePipe(ptyIn.put(), out.write.put(), nullptr, 0))
    return reportError(L"CreatePipe(input)");
  // output pipe: we read out.read(read), ConPTY writes ptyOut(write)
  if (!CreatePipe(out.read.put(), ptyOut.put(), nullptr, 0))
    return reportError(L"CreatePipe(output)");

  HRESULT hr =
      CreatePseudoConsole(size, ptyIn.get(), ptyOut.get(), 0, &out.handle);
  if (FAILED(hr)) {
    std::wcerr << L"CreatePseudoConsole failed: " << hr << L"\n";
    return false;
  }
  // ptyIn / ptyOut close here (RAII): ConPTY holds its own duplicates.
  return true;
}

// --------------------------------------------------------------------------
// Client process
// --------------------------------------------------------------------------

bool spawnClient(const std::wstring &commandLine, HPCON pc,
                 PROCESS_INFORMATION &pi) {
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nullptr;
  si.StartupInfo.hStdOutput = nullptr;
  si.StartupInfo.hStdError = nullptr;

  // Attribute list carrying the pseudoconsole. It must stay alive through
  // CreateProcessW, so it lives on this stack frame.
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<BYTE> attrBuf(attrSize);
  si.lpAttributeList =
      reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());

  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize))
    return reportError(L"InitializeProcThreadAttributeList");

  if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc,
                                 sizeof(HPCON), nullptr, nullptr))
    return reportError(L"UpdateProcThreadAttribute");

  // CreateProcessW needs a mutable, null-terminated command line.
  std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
  cmd.push_back(L'\0');

  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                           FALSE, // ConPTY is passed via the attribute list
                           EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                           &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(si.lpAttributeList);

  if (!ok)
    return reportError(L"CreateProcessW");
  return true;
}

// --------------------------------------------------------------------------
// Resize protocol (pure-ish): strip APC resize sequences, resize the PTY.
// --------------------------------------------------------------------------

void handleResize(const std::string &payload, HPCON pc) {
  int cols = 0, rows = 0;
  if (sscanf_s(payload.c_str(), "RESIZE;%d;%d", &cols, &rows) == 2 &&
      cols > 0 && rows > 0) {
    logLine("resize -> cols=" + std::to_string(cols) +
            " rows=" + std::to_string(rows));
    ResizePseudoConsole(
        pc, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
  }
}

// Returns the bytes that should be forwarded to the PTY (everything except the
// APC resize sequences). Assumes each resize sequence arrives whole within a
// single chunk, which holds because Emacs writes it as one atomic message.
std::string applyResizeProtocol(const std::string &chunk, HPCON pc) {
  static const std::string kStart = "\x1b_"; // ESC _
  static const std::string kEnd = "\x1b\\";  // ESC '\'

  std::string out;
  size_t i = 0;
  while (i < chunk.size()) {
    size_t s = chunk.find(kStart, i);
    if (s == std::string::npos) {
      out.append(chunk, i, std::string::npos);
      break;
    }
    out.append(chunk, i, s - i); // bytes before the sequence pass through

    size_t e = chunk.find(kEnd, s + kStart.size());
    if (e == std::string::npos) {
      // Incomplete here: treat as ordinary bytes rather than swallow them.
      out.append(chunk, s, std::string::npos);
      break;
    }
    size_t payloadStart = s + kStart.size();
    handleResize(chunk.substr(payloadStart, e - payloadStart), pc);
    i = e + kEnd.size();
  }
  return out;
}

// --------------------------------------------------------------------------
// I/O pumps (run on their own threads)
// --------------------------------------------------------------------------

void pumpOutput(HANDLE src, HANDLE dst) {
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(src, buf, sizeof(buf), &n, nullptr) && n > 0) {
    DWORD written = 0;
    WriteFile(dst, buf, n, &written, nullptr);
  }
}

void pumpInput(HANDLE src, HANDLE dst, HPCON pc) {
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(src, buf, sizeof(buf), &n, nullptr) && n > 0) {
    std::string fwd = applyResizeProtocol(std::string(buf, n), pc);
    if (!fwd.empty()) {
      DWORD written = 0;
      WriteFile(dst, fwd.data(), static_cast<DWORD>(fwd.size()), &written,
                nullptr);
      FlushFileBuffers(dst);
    }
  }
}

} // namespace

// --------------------------------------------------------------------------

int wmain(int argc, wchar_t *argv[]) {
  Args args;
  if (!parseArgs(argc, argv, args))
    return 1;

  // const std::wstring logPath = L"conpty.log";
  // gLog = CreateFileW(logPath.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
  //                    CREATE_ALWAYS, 0, nullptr);

  PseudoConsole pty;
  if (!createPseudoConsole(args.size, pty))
    return 1;

  PROCESS_INFORMATION client{};
  if (!spawnClient(args.commandLine, pty.handle, client))
    return 1;

  HANDLE stdIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD inMode = 0;
  if (GetConsoleMode(stdIn, &inMode)) {
    DWORD raw = inMode;
    raw &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    raw |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(stdIn, raw);
  }

  HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD outMode = 0;
  if (GetConsoleMode(hStdout, &outMode)) {
    SetConsoleMode(hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);

  std::thread outThread(pumpOutput, pty.read.get(), stdOut);
  std::thread inThread(pumpInput, stdIn, pty.write.get(), pty.handle);

  WaitForSingleObject(client.hProcess, INFINITE);

  ClosePseudoConsole(pty.handle); // EOF on the output read end -> unblocks pump
  outThread.join();
  inThread.detach(); // still blocked on stdin ReadFile; let the process exit

  CloseHandle(client.hThread);
  CloseHandle(client.hProcess);
  // pty's pipe handles are closed by RAII as it goes out of scope.
  return 0;
}
