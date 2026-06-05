#include <stdio.h>
#include <windows.h>

typedef struct {
  HANDLE inputWrite;
  HANDLE outputRead;
} ThreadHandles;

DWORD WINAPI outputThread(LPVOID param) {
  ThreadHandles *handles = (ThreadHandles *)param;
  char buffer[4096];
  DWORD bytesRead;

  while (
      ReadFile(handles->outputRead, buffer, sizeof(buffer), &bytesRead, NULL) &&
      bytesRead > 0) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, bytesRead, NULL, NULL);
  }
  return 0;
}

DWORD WINAPI inputThread(LPVOID param) {
  ThreadHandles *handles = (ThreadHandles *)param;
  char buffer[4096];
  DWORD bytesRead;

  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer),
                  &bytesRead, NULL) &&
         bytesRead > 0) {
    WriteFile(handles->inputWrite, buffer, bytesRead, NULL, NULL);
  }
  return 0;
}

int main() {
  // ConPTY > Emacs
  HANDLE inputRead, inputWrite;
  HANDLE outputRead, outputWrite;

  if (!CreatePipe(&inputRead, &inputWrite, NULL, 0)) {
    printf("Error creating input pipe: %d\n", GetLastError());
    return 1;
  }
  if (!CreatePipe(&outputRead, &outputWrite, NULL, 0)) {
    printf("Error creating output pipe: %d\n", GetLastError());
    return 1;
  }

  printf("Pipes successfully created\n");

  COORD size = {80, 24};
  HPCON hPC;

  HRESULT hr = CreatePseudoConsole(size, inputRead, outputWrite, 0, &hPC);
  if (FAILED(hr)) {
    printf("Error creating ConPTY: %d\n", GetLastError());
    return 1;
  }

  printf("ConPTY successfully created\n");

  STARTUPINFOEX siEx = {0};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);

  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
  siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
  InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize);

  UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
                            sizeof(hPC), NULL, NULL);

  PROCESS_INFORMATION pi = {0};
  wchar_t cmd[] = L"C:\\Program Files\\Git\\bin\\bash.exe";

  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                      EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                      &siEx.StartupInfo, &pi)) {
    printf("Error creating process: %d\n", GetLastError());
    return 1;
  }

  printf("Bash successfully spawned, PID: %d\n", pi.dwProcessId);

  CloseHandle(inputRead);
  CloseHandle(outputWrite);

  ThreadHandles handles = {inputWrite, outputRead};
  HANDLE hOutputThread = CreateThread(NULL, 0, outputThread, &handles, 0, NULL);
  HANDLE hInputThread = CreateThread(NULL, 0, inputThread, &handles, 0, NULL);

  WaitForSingleObject(pi.hProcess, INFINITE);
  printf("Bash terminated\n");

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  DeleteProcThreadAttributeList(siEx.lpAttributeList);
  free(siEx.lpAttributeList);
  ClosePseudoConsole(hPC);
  CloseHandle(inputWrite);
  CloseHandle(outputRead);

  return 0;
}
