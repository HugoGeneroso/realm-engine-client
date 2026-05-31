#pragma once

#include <Windows.h>

namespace IpcFraming {

bool WriteMessage(HANDLE hPipe, const char* json, int len);
int ReadMessage(HANDLE hPipe, char* buf, int bufSize);

} // namespace IpcFraming
