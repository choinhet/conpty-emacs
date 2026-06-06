#include <stdio.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <winnls.h>

int main(int argc, char *argv[]) {
  // argv[1] = program path
  // argv[2] = width/cols
  // argv[3] = height/rows
  // argv[4+] = optional extra args

  if (argc < 4) {
    printf("Usage: conpty-emacs.exe <program> <cols> <rows> [extra args...]\n");
    return 1;
  }

  int cols = atoi(argv[2]);
  int rows = atoi(argv[3]);
  COORD size = {(SHORT)cols, (SHORT)rows};

  // Build full command: argv[1] + any extra args after argv[3]
  int shellLen = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, NULL, 0);
  std::vector<wchar_t> shellW(shellLen);
  MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, shellW.data(), shellLen);

  std::wstring fullCmdStr(shellW.data()); // starts with program path
  for (int i = 4; i < argc; i++) {
    int argLen = MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, NULL, 0);
    std::vector<wchar_t> argW(argLen);
    MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, argW.data(), argLen);
    fullCmdStr += L" ";
    fullCmdStr += argW.data();
  }
  // CreateProcessW needs a mutable buffer
  std::vector<wchar_t> cmdBuf(fullCmdStr.begin(), fullCmdStr.end());
  cmdBuf.push_back(0);

  // --- Pipes ---
  HANDLE ptyIn, ptyOut; // PTY side (child end)
  HANDLE curIn, curOut; // Our side (parent end)

  // Input pipe: parent writes to curIn, PTY reads from ptyIn
  if (!CreatePipe(&ptyIn, &curIn, NULL, 0)) {
    printf("Error creating input pipe: %lu\n", GetLastError());
    return 1;
  }
  // Output pipe: PTY writes to ptyOut, parent reads from curOut
  if (!CreatePipe(&curOut, &ptyOut, NULL, 0)) {
    printf("Error creating output pipe: %lu\n", GetLastError());
    return 1;
  }

  printf("Pipes successfully created\n");

  // --- ConPTY ---
  HPCON hPC = INVALID_HANDLE_VALUE;
  HRESULT hr = CreatePseudoConsole(size, ptyIn, ptyOut, 0, &hPC);

  // PTY-side handles are owned by the ConPTY now   close our copies
  CloseHandle(ptyIn);
  CloseHandle(ptyOut);

  if (FAILED(hr)) {
    printf("Error creating ConPTY: 0x%08lx\n", hr);
    return 1;
  }

  printf("ConPTY successfully created\n");

  // --- Thread attribute list (must stay alive until after CreateProcessW) ---
  STARTUPINFOEXW siEx{};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);

  SIZE_T attrListSize = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
  std::vector<BYTE> attrListBuf(attrListSize);
  siEx.lpAttributeList =
      reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrListBuf.data());

  if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0,
                                         &attrListSize)) {
    printf("Error initializing attribute list: %lu\n", GetLastError());
    return 1;
  }

  if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
                                 sizeof(HPCON), NULL, NULL)) {
    printf("Error updating proc thread attribute: %lu\n", GetLastError());
    return 1;
  }

  // --- Enable VT processing on the parent console ---
  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

  DWORD consoleMode = 0;
  if (GetConsoleMode(hStdout, &consoleMode)) {
    SetConsoleMode(hStdout, consoleMode | ENABLE_PROCESSED_OUTPUT |
                                ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                DISABLE_NEWLINE_AUTO_RETURN);
  }
  if (GetConsoleMode(hStdin, &consoleMode)) {
    SetConsoleMode(hStdin, ENABLE_VIRTUAL_TERMINAL_INPUT);
  }

  // --- Spawn the child process ---
  PROCESS_INFORMATION piClient{};
  BOOL ok =
      CreateProcessW(nullptr,       // application name (use cmdBuf instead)
                     cmdBuf.data(), // mutable command line
                     nullptr,       // process security attrs
                     nullptr,       // thread security attrs
                     FALSE, // don't inherit handles (ConPTY uses attr list)
                     EXTENDED_STARTUPINFO_PRESENT, // dwCreationFlags
                     nullptr,                      // inherit environment
                     nullptr,                      // inherit CWD
                     &siEx.StartupInfo, &piClient);

  // Attribute list no longer needed after CreateProcessW
  DeleteProcThreadAttributeList(siEx.lpAttributeList);

  if (!ok) {
    printf("Error creating process: %lu\n", GetLastError());
    return 1;
  }

  printf("Process successfully spawned, PID: %lu\n", piClient.dwProcessId);

  // --- I/O threads ---
  std::thread outputThr([curOut, hStdout]() {
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(curOut, buffer, sizeof(buffer), &bytesRead, NULL) &&
           bytesRead > 0) {
      DWORD written;
      WriteFile(hStdout, buffer, bytesRead, &written, NULL);
    }
  });

  std::thread inputThr([curIn, hStdin]() {
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, NULL) &&
           bytesRead > 0) {
      DWORD written;
      WriteFile(curIn, buffer, bytesRead, &written, NULL);
    }
  });

  // Wait for child to exit, then clean up
  WaitForSingleObject(piClient.hProcess, INFINITE);

  CloseHandle(piClient.hThread);
  CloseHandle(piClient.hProcess);

  ClosePseudoConsole(hPC);

  // Closing the pipe ends blocks ReadFile in the threads
  CloseHandle(curIn);
  CloseHandle(curOut);

  outputThr.join();
  inputThr.join();

  return 0;
}
