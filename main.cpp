#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <winnls.h>

void debug(std::string str, HANDLE outHandle) {
  std::string escaped;
  for (char c : str) {
    switch (c) {
    case '\r':
      escaped += "\\r";
      break;
    case '\n':
      escaped += "\\n";
      break;
    default:
      escaped += c;
    }
  }
  std::string msg = "\n=====[DEBUG] " + escaped + "=====\n";
  WriteFile(outHandle, msg.c_str(), msg.size(), nullptr, nullptr);
}

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

  std::vector<wchar_t> cmdBuf(fullCommand.begin(), fullCommand.end());
  cmdBuf.push_back(L'\0');

  HANDLE ptyIn, ptyOut;
  HANDLE curIn, curOut;

  if (!CreatePipe(&ptyIn, &curIn, nullptr, 0)) {
    std::wcout << L"Error creating input pipe: " << GetLastError() << L"\n";
    return 1;
  }

  if (!CreatePipe(&curOut, &ptyOut, nullptr, 0)) {
    std::wcout << L"Error creating output pipe: " << GetLastError() << L"\n";
    return 1;
  }

  HPCON hPC = INVALID_HANDLE_VALUE;
  HRESULT hr = CreatePseudoConsole(size, ptyIn, ptyOut, 0, &hPC);

  CloseHandle(ptyIn);
  CloseHandle(ptyOut);

  if (FAILED(hr)) {

    std::wcout << L"Error creating ConPTY: " << hr << "\n";

    return 1;
  }

  STARTUPINFOEXW siEx{};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
  siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
  siEx.StartupInfo.hStdInput = nullptr;
  siEx.StartupInfo.hStdOutput = nullptr;
  siEx.StartupInfo.hStdError = nullptr;

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
  static HANDLE hLog =
      CreateFileW(L"conpty.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_ALWAYS, 0, nullptr);

  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

  DWORD stdinType = GetFileType(hStdin);
  DWORD stdoutType = GetFileType(hStdout);

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
  DeleteProcThreadAttributeList(siEx.lpAttributeList);

  if (!ok) {
    std::wcout << L"Error creating process: " << GetLastError() << L"\n";
    return 1;
  }

  std::thread outputThr([curOut, hStdout, stdinType, stdoutType]() {
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(curOut, buffer, sizeof(buffer), &bytesRead, nullptr) &&
           bytesRead > 0) {
      DWORD written = 0;
      std::string content(buffer, bytesRead);
      BOOL w =
          WriteFile(hStdout, content.data(), content.size(), &written, nullptr);
    }
  });

  std::thread inputThr([curIn, hStdin, hPC]() {
    char buffer[4096];
    DWORD bytesRead;
    std::string apc;
    int state = 0;
    while (ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, nullptr) &&
           bytesRead > 0) {
      std::string fwd;
      for (DWORD i = 0; i < bytesRead; i++) {
        unsigned char c = buffer[i];
        switch (state) {
        case 0:
          if (c == 0x1b)
            state = 1;
          else
            fwd += c;
          break;
        case 1:
          if (c == '_') {
            state = 2;
            apc.clear();
          } else {
            fwd += 0x1b;
            fwd += c;
            state = 0;
          }
          break;
        case 2:
          if (c == 0x1b)
            state = 3;
          else
            apc += c;
          break;
        case 3:
          if (c == '\\') { // ST -> fim da APC
            if (apc.rfind("RESIZE;", 0) == 0) {
              int cols = 0, rows = 0;
              sscanf(apc.c_str(), "RESIZE;%d;%d", &cols, &rows);
              ResizePseudoConsole(hPC, COORD{(SHORT)cols, (SHORT)rows});
            }
            state = 0;
          } else {
            apc += 0x1b;
            apc += c;
            state = 2;
          }
          break;
        }
      }
      if (!fwd.empty())
        WriteFile(curIn, fwd.data(), fwd.size(), nullptr, nullptr);
    }
  });

  WaitForSingleObject(piClient.hProcess, INFINITE);
  CloseHandle(piClient.hThread);
  CloseHandle(piClient.hProcess);
  ClosePseudoConsole(hPC);
  CloseHandle(curIn);
  CloseHandle(curOut);
  outputThr.join();
  inputThr.detach();

  return 0;
}
