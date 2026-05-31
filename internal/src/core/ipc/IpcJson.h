#pragma once

namespace IpcJson {

char* GetString(char* json, const char* key, char* valBuf, int valBufSize);
bool GetBool(char* json, const char* key);
bool GetNumberToken(char* json, const char* key, char* outBuf, int outBufSize);

} // namespace IpcJson
