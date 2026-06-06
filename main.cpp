#include <iostream>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <winnls.h>

int wmain(int argc, wchar_t *argv[]) {
  if (argc < 4) {
    std::wcout
        << L"Usage: conpty-emacs.exe <program> <cols> <rows> [extra args...]\n";
    return 1;
  }

  std::wstring command = argv[1];
  std::wstring fullCommand = L"\"" + command + L"\"";

  int cols = std::stoi(std::wstring(argv[2]));
  int rows = std::stoi(std::wstring(argv[3]));
  COORD size = {(SHORT)cols, (SHORT)rows};

  for (int i = 4; i < argc; i++) {
    fullCommand += L" ";
    fullCommand += argv[i];
  }

  std::wcout << L"Full Command: " << fullCommand << L"\n";

  // CreateProcessW needs a mutable buffer
  std::vector<wchar_t> cmdBuf(fullCommand.begin(), fullCommand.end());
  cmdBuf.push_back(L'\0');

  // --- Pipes ---
  HANDLE ptyIn, ptyOut; // PTY side (child end)
  HANDLE curIn, curOut; // Our side (parent end)

  // Input pipe: parent writes to curIn, PTY reads from ptyIn
  if (!CreatePipe(&ptyIn, &curIn, nullptr, 0)) {
    std::wcout << L"Error creating input pipe: " << GetLastError() << L"\n";
    return 1;
  }
  // Output pipe: PTY writes to ptyOut, parent reads from curOut
  if (!CreatePipe(&curOut, &ptyOut, nullptr, 0)) {
    std::wcout << L"Error creating output pipe: " << GetLastError() << L"\n";
    return 1;
  }

  std::wcout << L"Pipes successfully created\n";

  // --- ConPTY ---
  HPCON hPC = INVALID_HANDLE_VALUE;
  HRESULT hr = CreatePseudoConsole(size, ptyIn, ptyOut, 0, &hPC);

  // PTY-side handles are owned by the ConPTY now   close our copies
  CloseHandle(ptyIn);
  CloseHandle(ptyOut);

  if (FAILED(hr)) {
    printf("Error creating ConPTY: 0x%08lx\n", hr);
    std::wcout << L"Error creating ConPTY: " << hr << "\n";
    return 1;
  }

  std::wcout << L"ConPTY successfully created\n";

  // --- Thread attribute list (must stay alive until after CreateProcessW) ---
  STARTUPINFOEXW siEx{};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);

  SIZE_T attrListSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
  std::vector<BYTE> attrListBuf(attrListSize);
  siEx.lpAttributeList =
      reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrListBuf.data());

  if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0,
                                         &attrListSize)) {
    std::wcout << L"Error initializing attribute list: " << GetLastError()
               << L"\n";
    return 1;
  }

  if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
                                 sizeof(HPCON), nullptr, nullptr)) {
    std::wcout << L"Error updating proc thread attribute: " << GetLastError()
               << L"\n";
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
    std::wcout << L"Error creating process: " << GetLastError() << L"\n";
    return 1;
  }

  std::wcout << L"Process successfully spawned, PID: " << piClient.dwProcessId
             << L"\n";

  // --- I/O threads ---
  std::thread outputThr([curOut, hStdout]() {
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(curOut, buffer, sizeof(buffer), &bytesRead, nullptr) &&
           bytesRead > 0) {
      DWORD written;
      WriteFile(hStdout, buffer, bytesRead, &written, nullptr);
    }
  });

  std::thread inputThr([curIn, hStdin]() {
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, nullptr) &&
           bytesRead > 0) {
      DWORD written;
      WriteFile(curIn, buffer, bytesRead, &written, nullptr);
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
  inputThr.detach();

  std::wcout << L"Exiting program\n";
  return 0;
}
